/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-ports-move — moves the ports folder between the console's card and
 * the SD2 card for nuubos-portsd (PortMaster page, Ports on SD2, GitHub #21).
 *
 *   nuubos-ports-move <from> <to>
 *       @progress <percent>... then @done <conflicts>
 *
 * Failures print "@error space|write|cancelled" and exit non-zero; <from>
 * is then left as it was.
 *
 * Every entry of <from> (a port's script, its folder, gamelist.xml...) moves
 * to <to> unless <to> already has an entry of that name: the destination's
 * copy is kept and the source's stays where it is (a script counts as one
 * conflict). PortMaster: the newer release wins (the older one stays where
 * it was). nuubOS's own .nuubos (the ports' HOME, their saves) is merged
 * file by file, never overwriting.
 *
 * Phases: plan and space check; copy into <to>/.nuubos-move (SIGTERM
 * removes it, <from> untouched); commit by renames on <to>'s card; then
 * the moved files are removed from <from>.
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

#define STAGING ".nuubos-move"
#define OWN ".nuubos"
#define PORTMASTER "PortMaster"
/* Room left on the destination beyond what is copied. */
#define SPACE_MARGIN (64LL << 20)

static volatile sig_atomic_t cancelled;
static const char *from, *to;
static char staging[PATH_MAX];
static long long total, done;
static int last_percent = -1;

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

static bool exists(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

/* Bytes under a path (files only). */
static long long tree_size(const char *path)
{
	struct stat st;
	long long sum = 0;
	DIR *dir;
	struct dirent *de;

	if (lstat(path, &st) != 0)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return S_ISREG(st.st_mode) ? st.st_size : 0;
	dir = opendir(path);
	while (dir && (de = readdir(dir)) != NULL) {
		char sub[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (pathf(sub, sizeof(sub), "%s/%s", path, de->d_name))
			sum += tree_size(sub);
	}
	if (dir)
		closedir(dir);
	return sum;
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

static int copy_file(const char *src, const char *dst, mode_t mode)
{
	static char buf[1 << 17];
	int in = open(src, O_RDONLY | O_CLOEXEC);
	int out = in < 0 ? -1 : open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 0777);
	ssize_t n;
	int rc = 0;

	if (in < 0 || out < 0) {
		if (in >= 0)
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

/* A copy of a tree; with skip_existing (the .nuubos merge) only what
 * `existing` does not have yet. */
static int copy_tree(const char *src, const char *dst, const char *existing)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	if (cancelled || lstat(src, &st) != 0)
		return -1;
	if (existing && exists(existing) && !S_ISDIR(st.st_mode))
		return 0;
	if (S_ISLNK(st.st_mode)) {
		char target[PATH_MAX];
		ssize_t n = readlink(src, target, sizeof(target) - 1);

		if (n < 0)
			return -1;
		target[n] = '\0';
		/* A card without symbolic links (exFAT, FAT32): the file itself. */
		if (symlink(target, dst) == 0)
			return 0;
		return stat(src, &st) == 0 && S_ISREG(st.st_mode) ? copy_file(src, dst, st.st_mode) : 0;
	}
	if (S_ISREG(st.st_mode))
		return copy_file(src, dst, st.st_mode);
	if (!S_ISDIR(st.st_mode))
		return 0;
	if (mkdir(dst, 0755) != 0 && errno != EEXIST)
		return -1;
	dir = opendir(src);
	if (!dir)
		return -1;
	while ((de = readdir(dir)) != NULL && rc == 0) {
		char s[PATH_MAX], d[PATH_MAX], e[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!pathf(s, sizeof(s), "%s/%s", src, de->d_name) || !pathf(d, sizeof(d), "%s/%s", dst, de->d_name) ||
		    (existing && !pathf(e, sizeof(e), "%s/%s", existing, de->d_name)))
			rc = -1;
		else
			rc = copy_tree(s, d, existing ? e : NULL);
	}
	closedir(dir);
	return rc;
}

/* The merged .nuubos: every staged file into place (it did not exist),
 * then its source file goes (only a file that really moved). */
static int merge_into(const char *staged, const char *dst, const char *src)
{
	struct stat st;
	DIR *dir;
	struct dirent *de;
	int rc = 0;

	if (lstat(staged, &st) != 0)
		return -1;
	if (!S_ISDIR(st.st_mode)) {
		if (exists(dst) || rename(staged, dst) != 0)
			return -1;
		unlink(src);
		return 0;
	}
	if (mkdir(dst, 0755) != 0 && errno != EEXIST)
		return -1;
	dir = opendir(staged);
	while (dir && (de = readdir(dir)) != NULL) {
		char s[PATH_MAX], d[PATH_MAX], o[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!pathf(s, sizeof(s), "%s/%s", staged, de->d_name) || !pathf(d, sizeof(d), "%s/%s", dst, de->d_name) ||
		    !pathf(o, sizeof(o), "%s/%s", src, de->d_name) || merge_into(s, d, o) != 0)
			rc = -1;
	}
	if (dir)
		closedir(dir);
	return rc;
}

/* Folders the merge emptied in the source (rmdir: only empty ones). */
static void prune_empty(const char *path)
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
			prune_empty(sub);
	}
	if (dir)
		closedir(dir);
	rmdir(path);
}

/* PortMaster's release ("2026.06.23-0015"), "" without one. */
static void pm_version(const char *root, char *out, size_t size)
{
	char path[PATH_MAX];
	FILE *fp;

	out[0] = '\0';
	if (!pathf(path, sizeof(path), "%s/%s", root, PORTMASTER "/version"))
		return;
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (fgets(out, (int)size, fp))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(fp);
}

enum action { SKIP, MOVE, REPLACE, MERGE };

struct entry {
	char name[NAME_MAX + 1];
	enum action action;
};

static int fail(const char *why)
{
	remove_tree(staging);
	printf("@error %s\n", why);
	fflush(stdout);
	return 1;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	struct statvfs vfs;
	struct entry *list = NULL;
	size_t count = 0, cap = 0;
	int conflicts = 0;
	DIR *dir;
	struct dirent *de;

	if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/') {
		fprintf(stderr, "Usage: nuubos-ports-move <from> <to>\n");
		return 2;
	}
	from = argv[1];
	to = argv[2];
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	if (!pathf(staging, sizeof(staging), "%s/%s", to, STAGING) ||
	    mkdir_p(to) != 0)
		return fail("write");
	remove_tree(staging); /* a stop of an earlier run */

	/* Plan. */
	dir = opendir(from);
	while (dir && (de = readdir(dir)) != NULL) {
		char src[PATH_MAX], dst[PATH_MAX];
		struct entry e = { .action = MOVE };

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, STAGING))
			continue;
		if (!pathf(src, sizeof(src), "%s/%s", from, de->d_name) || !pathf(dst, sizeof(dst), "%s/%s", to, de->d_name))
			continue;
		snprintf(e.name, sizeof(e.name), "%s", de->d_name);
		if (!strcmp(de->d_name, OWN)) {
			e.action = exists(dst) ? MERGE : MOVE;
		} else if (exists(dst)) {
			e.action = SKIP;
			if (!strcmp(de->d_name, PORTMASTER)) {
				char a[64], b[64];

				pm_version(from, a, sizeof(a));
				pm_version(to, b, sizeof(b));
				/* Dates: a later string is a later release. */
				if (a[0] && strcmp(a, b) > 0)
					e.action = REPLACE;
			} else if (strlen(de->d_name) > 3 && !strcmp(de->d_name + strlen(de->d_name) - 3, ".sh")) {
				conflicts++;
			}
		}
		if (e.action != SKIP)
			total += tree_size(src);
		if (count == cap) {
			cap = cap ? cap * 2 : 64;
			list = realloc(list, cap * sizeof(*list));
			if (!list)
				return fail("write");
		}
		list[count++] = e;
	}
	if (dir)
		closedir(dir);
	if (statvfs(to, &vfs) == 0 && (long long)vfs.f_bavail * (long long)vfs.f_frsize < total + SPACE_MARGIN)
		return fail("space");
	progress();

	/* Copy into the staging folder (the source untouched). */
	if (mkdir(staging, 0755) != 0)
		return fail("write");
	for (size_t i = 0; i < count; i++) {
		char src[PATH_MAX], dst[PATH_MAX], existing[PATH_MAX];

		if (list[i].action == SKIP)
			continue;
		if (!pathf(src, sizeof(src), "%s/%s", from, list[i].name) ||
		    !pathf(dst, sizeof(dst), "%s/%s", staging, list[i].name) ||
		    !pathf(existing, sizeof(existing), "%s/%s", to, list[i].name) ||
		    copy_tree(src, dst, list[i].action == MERGE ? existing : NULL) != 0)
			return fail(cancelled ? "cancelled" : "write");
	}
	sync();
	if (cancelled)
		return fail("cancelled");

	/* Commit: renames on the destination's card; no stop from here. */
	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	for (size_t i = 0; i < count; i++) {
		char staged[PATH_MAX], dst[PATH_MAX], old[PATH_MAX];

		if (list[i].action == SKIP)
			continue;
		if (!pathf(staged, sizeof(staged), "%s/%s", staging, list[i].name) ||
		    !pathf(dst, sizeof(dst), "%s/%s", to, list[i].name))
			return fail("write");
		if (list[i].action == MERGE) {
			char src[PATH_MAX];

			if (!pathf(src, sizeof(src), "%s/%s", from, list[i].name) || merge_into(staged, dst, src) != 0)
				return fail("write");
			continue;
		}
		if (list[i].action == REPLACE) {
			/* The older PortMaster out of the way, removed after. */
			if (!pathf(old, sizeof(old), "%s/%s.old", staging, list[i].name) || rename(dst, old) != 0)
				return fail("write");
		}
		if (rename(staged, dst) != 0)
			return fail("write");
	}
	sync();

	/* Only now: the moved files leave the source. */
	for (size_t i = 0; i < count; i++) {
		char src[PATH_MAX];

		if (list[i].action == SKIP || !pathf(src, sizeof(src), "%s/%s", from, list[i].name))
			continue;
		if (list[i].action == MERGE) {
			prune_empty(src);
		} else if (remove_tree(src) != 0) {
			fprintf(stderr, "nuubos-ports-move: %s not removed\n", src);
		}
	}
	remove_tree(staging);
	sync();
	free(list);
	printf("@progress 100\n@done %d\n", conflicts);
	fflush(stdout);
	return 0;
}
