/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-data-move — moves user data folders between SD1 and the SD2 card
 * for nuubos-storagectl (Settings → Storage, Data on SD2, GitHub #24).
 *
 *   nuubos-data-move <conflicts> <name> <from> <to> [<name> <from> <to>...]
 *       @progress <percent>... then @done <conflicts>
 *
 * Failures print "@error space|write|cancelled" and exit non-zero; every
 * <from> is then left as it was.
 *
 * File by file: a file <to> does not have moves; a file <to> already has
 * with the same content only goes from <from>; a different one is a
 * conflict: <to>'s copy is kept and <from>'s moves to
 * <conflicts>/<name>/<path> (on <to>'s card, never lost, never hidden under
 * a later mount). <from> keeps its own folder, emptied.
 *
 * Phases: plan and space check; copy into <to>/../.<to name>.nuubos-move
 * (SIGTERM removes it, <from> untouched); commit by renames on <to>'s
 * card, each source file removed after its rename. An interrupted run is
 * simply run again: what was already committed is now the same content.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Room left on the destination beyond what is copied. */
#define SPACE_MARGIN (64LL << 20)

static volatile sig_atomic_t cancelled;
static long long total, done;
static int last_percent = -1;

enum action { MOVE, SAME, CONFLICT };

struct file {
	int pair;
	enum action action;
	char *rel;
};

struct pair {
	const char *name, *from, *to;
	char staging[PATH_MAX];
};

static struct pair *pairs;
static int npairs;
static struct file *files;
static size_t count, cap;

static void on_term(int sig)
{
	(void)sig;
	cancelled = 1;
}

static bool pathf(char *out, size_t size, const char *fmt, const char *a, const char *b)
{
	int n = snprintf(out, size, fmt, a, b);

	return n >= 0 && (size_t)n < size;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

static int mkdir_parent(const char *path)
{
	char tmp[PATH_MAX];
	char *slash;

	snprintf(tmp, sizeof(tmp), "%s", path);
	slash = strrchr(tmp, '/');
	if (!slash || slash == tmp)
		return 0;
	*slash = '\0';
	return mkdir_p(tmp);
}

static void progress(void)
{
	int percent = total > 0 ? (int)(done * 100 / total) : 100;

	if (percent > 99)
		percent = 99;
	if (percent != last_percent) {
		printf("@progress %d\n", percent);
		fflush(stdout);
		last_percent = percent;
	}
}

static int remove_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	dir = opendir(path);
	while (dir && (de = readdir(dir)) != NULL) {
		char sub[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!pathf(sub, sizeof(sub), "%s/%s", path, de->d_name) || remove_tree(sub) != 0)
			rc = -1;
	}
	if (dir)
		closedir(dir);
	return rmdir(path) != 0 ? -1 : rc;
}

/* Folders the move emptied below a root (the root itself stays). */
static void prune_empty(const char *path, bool root)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;

	if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return;
	dir = opendir(path);
	while (dir && (de = readdir(dir)) != NULL) {
		char sub[PATH_MAX];

		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") && pathf(sub, sizeof(sub), "%s/%s", path, de->d_name))
			prune_empty(sub, false);
	}
	if (dir)
		closedir(dir);
	if (!root)
		rmdir(path);
}

static bool same_content(const char *a, const char *b, off_t size)
{
	static char x[1 << 16], y[1 << 16];
	struct stat st;
	int fa, fb;
	bool same = true;
	ssize_t n;

	if (stat(b, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != size)
		return false;
	fa = open(a, O_RDONLY | O_CLOEXEC);
	fb = open(b, O_RDONLY | O_CLOEXEC);
	if (fa < 0 || fb < 0) {
		same = false;
	} else {
		while (same && (n = read(fa, x, sizeof(x))) > 0)
			same = read(fb, y, (size_t)n) == n && !memcmp(x, y, (size_t)n);
		if (n < 0)
			same = false;
	}
	if (fa >= 0)
		close(fa);
	if (fb >= 0)
		close(fb);
	return same;
}

static int add_file(int pair, enum action action, const char *rel)
{
	if (count == cap) {
		cap = cap ? cap * 2 : 256;
		files = realloc(files, cap * sizeof(*files));
		if (!files)
			return -1;
	}
	files[count].pair = pair;
	files[count].action = action;
	files[count].rel = strdup(rel);
	return files[count++].rel ? 0 : -1;
}

/* Every regular file below <from>/<rel> (symbolic links and devices stay). */
static int plan(int pair, const char *rel)
{
	struct pair *p = &pairs[pair];
	char dir_path[PATH_MAX];
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	if (!pathf(dir_path, sizeof(dir_path), rel[0] ? "%s/%s" : "%s%s", p->from, rel))
		return -1;
	dir = opendir(dir_path);
	if (!dir)
		return errno == ENOENT ? 0 : -1;
	while (rc == 0 && (de = readdir(dir)) != NULL) {
		char sub[PATH_MAX], src[PATH_MAX], dst[PATH_MAX];
		struct stat st;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!pathf(sub, sizeof(sub), rel[0] ? "%s/%s" : "%s%s", rel, de->d_name) ||
		    !pathf(src, sizeof(src), "%s/%s", p->from, sub) || !pathf(dst, sizeof(dst), "%s/%s", p->to, sub)) {
			rc = -1;
			break;
		}
		if (lstat(src, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			rc = plan(pair, sub);
		} else if (S_ISREG(st.st_mode)) {
			struct stat dst_st;
			enum action action = MOVE;

			if (lstat(dst, &dst_st) == 0)
				action = same_content(src, dst, st.st_size) ? SAME : CONFLICT;
			if (action != SAME)
				total += st.st_size;
			rc = add_file(pair, action, sub);
		}
	}
	closedir(dir);
	return rc;
}

static int copy_file(const char *src, const char *dst)
{
	static char buf[1 << 17];
	struct stat st;
	int in = open(src, O_RDONLY | O_CLOEXEC);
	int out;
	ssize_t n;
	int rc = 0;

	if (in < 0 || fstat(in, &st) != 0 || mkdir_parent(dst) != 0) {
		if (in >= 0)
			close(in);
		return -1;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 0777);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0 && !cancelled) {
		if (write(out, buf, (size_t)n) != n) {
			rc = -1;
			break;
		}
		done += n;
		progress();
	}
	if (n < 0 || cancelled)
		rc = -1;
	/* exFAT: the data on the card before the rename makes it visible. */
	if (fsync(out) != 0 || close(out) != 0)
		rc = -1;
	close(in);
	return rc;
}

static int fail(const char *why)
{
	for (int i = 0; i < npairs; i++)
		remove_tree(pairs[i].staging);
	printf("@error %s\n", why);
	fflush(stdout);
	return 1;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	const char *conflicts_dir;
	int conflicts = 0;

	if (argc < 5 || (argc - 2) % 3 != 0 || argv[1][0] != '/') {
		fprintf(stderr, "Usage: nuubos-data-move <conflicts> <name> <from> <to> [<name> <from> <to>...]\n");
		return 2;
	}
	conflicts_dir = argv[1];
	npairs = (argc - 2) / 3;
	pairs = calloc((size_t)npairs, sizeof(*pairs));
	if (!pairs)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	for (int i = 0; i < npairs; i++) {
		struct pair *p = &pairs[i];
		char parent[PATH_MAX];
		const char *base;
		char *slash;

		p->name = argv[2 + i * 3];
		p->from = argv[3 + i * 3];
		p->to = argv[4 + i * 3];
		if (p->from[0] != '/' || p->to[0] != '/' || !p->name[0] || strstr(p->name, ".."))
			return fail("write");
		snprintf(parent, sizeof(parent), "%s", p->to);
		slash = strrchr(parent, '/');
		base = slash + 1;
		*slash = '\0';
		if (!*base || !pathf(p->staging, sizeof(p->staging), "%s/.%s.nuubos-move",
				     slash == parent ? "" : parent, base))
			return fail("write");
		if (mkdir_p(p->to) != 0)
			return fail("write");
		remove_tree(p->staging); /* a stop of an earlier run */
	}

	/* Plan: every file of every pair, then the room on the destination
	 * (one card for all of them). */
	for (int i = 0; i < npairs; i++)
		if (plan(i, "") != 0)
			return fail("write");
	{
		struct statvfs vfs;

		if (statvfs(pairs[0].to, &vfs) == 0 &&
		    (long long)vfs.f_bavail * (long long)vfs.f_frsize < total + SPACE_MARGIN)
			return fail("space");
	}
	progress();

	/* Copy into the staging folders (the sources untouched). Conflicts go
	 * to their own subfolder. */
	for (size_t i = 0; i < count; i++) {
		struct pair *p = &pairs[files[i].pair];
		char src[PATH_MAX], dst[PATH_MAX];

		if (files[i].action == SAME)
			continue;
		if (!pathf(src, sizeof(src), "%s/%s", p->from, files[i].rel) ||
		    !pathf(dst, sizeof(dst), files[i].action == CONFLICT ? "%s/c/%s" : "%s/m/%s", p->staging, files[i].rel) ||
		    copy_file(src, dst) != 0)
			return fail(cancelled ? "cancelled" : "write");
	}
	sync();
	if (cancelled)
		return fail("cancelled");

	/* Commit: renames on the destination's card; no stop from here. Each
	 * source file goes once its copy is in place. */
	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	for (size_t i = 0; i < count; i++) {
		struct pair *p = &pairs[files[i].pair];
		char src[PATH_MAX], staged[PATH_MAX], dst[PATH_MAX], kept[PATH_MAX];

		if (!pathf(src, sizeof(src), "%s/%s", p->from, files[i].rel))
			return fail("write");
		if (files[i].action != SAME) {
			bool conflict = files[i].action == CONFLICT;

			if (!pathf(staged, sizeof(staged), conflict ? "%s/c/%s" : "%s/m/%s", p->staging, files[i].rel) ||
			    !pathf(kept, sizeof(kept), "%s/%s", conflicts_dir, p->name) ||
			    !pathf(dst, sizeof(dst), "%s/%s", conflict ? kept : p->to, files[i].rel) ||
			    mkdir_parent(dst) != 0 || rename(staged, dst) != 0)
				return fail("write");
			if (conflict)
				conflicts++;
		}
		unlink(src);
	}
	sync();
	for (int i = 0; i < npairs; i++) {
		remove_tree(pairs[i].staging);
		prune_empty(pairs[i].from, true);
	}
	sync();
	printf("@progress 100\n@done %d\n", conflicts);
	fflush(stdout);
	return 0;
}
