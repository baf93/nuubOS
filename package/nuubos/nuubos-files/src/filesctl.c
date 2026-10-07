/* SPDX-License-Identifier: MIT */
/*
 * nuubos-filesctl — File Manager operations (EPIC-031).
 *
 *   roots                         user-accessible locations (name, path, state)
 *   list PATH                     entry=<d|f>\t<size>\t<mtime>\t<name> ... end=1
 *   info PATH                     type, size (recursive for folders), mtime
 *   mkdir PATH | rename PATH NEWNAME | delete PATH
 *   copy SRC DSTDIR | move SRC DSTDIR       (nuubos-jobd workers)
 *
 * Only paths under the allowed roots are accepted after symlink resolution:
 * USERDATA (/userdata), removable media (/media/<volume>) and mounted
 * network shares (/run/nuubos/shares/<name>). SYSTEM, STATE and nuubOS
 * internal folders are never reachable. Copy/move report @progress by bytes
 * and write into "<name>.part" first; SIGTERM (cancel) or an error removes
 * the partial file and stops. Existing names are never overwritten: the
 * copy gets " (2)", " (3)"... Replies: OK / ERR <reason>.
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef USERDATA_ROOT
#define USERDATA_ROOT "/userdata"
#endif
#ifndef MEDIA_ROOT
#define MEDIA_ROOT "/media"
#endif
#ifndef SHARES_ROOT
#define SHARES_ROOT "/run/nuubos/shares"
#endif

#define CHUNK (1024 * 1024)

static volatile sig_atomic_t cancelled;
static bool as_job;
static long long total_bytes;
static long long done_bytes;
static int last_percent = -1;
static char partial[PATH_MAX];

static void on_term(int sig)
{
	(void)sig;
	cancelled = 1;
}

static int fail(const char *reason)
{
	if (as_job)
		printf("@error %s\n", reason);
	else
		printf("ERR %s\n", reason);
	return 1;
}

/* Hidden nuubOS internals inside USERDATA. */
static bool internal_name(const char *name)
{
	return !strcmp(name, ".nuubos-upload") || !strcmp(name, "lost+found") ||
	       !strncmp(name, ".stage.", 7);
}

static bool under(const char *path, const char *root)
{
	size_t n = strlen(root);

	return !strncmp(path, root, n) && (path[n] == '\0' || path[n] == '/');
}

/*
 * Resolve path (it may not exist yet when must_exist is false: then its
 * parent must) and check it lies under an allowed root. The root itself is
 * allowed for listing only (allow_root).
 */
static bool resolve(const char *in, char *out, bool must_exist, bool allow_root)
{
	char tmp[PATH_MAX];
	const char *roots[] = { USERDATA_ROOT, MEDIA_ROOT, SHARES_ROOT };

	if (!in[0] || in[0] != '/' || strstr(in, "/../") || strlen(in) >= PATH_MAX)
		return false;
	if (realpath(in, out) == NULL) {
		char *slash;

		if (must_exist || errno != ENOENT)
			return false;
		snprintf(tmp, sizeof(tmp), "%s", in);
		slash = strrchr(tmp, '/');
		if (!slash || !slash[1] || !strcmp(slash + 1, "..") || !strcmp(slash + 1, "."))
			return false;
		*slash = '\0';
		if (realpath(tmp[0] ? tmp : "/", out) == NULL)
			return false;
		if (strlen(out) + strlen(slash + 1) + 2 >= PATH_MAX)
			return false;
		strcat(out, "/");
		strcat(out, slash + 1);
	}
	for (size_t i = 0; i < 3; i++) {
		if (!under(out, roots[i]))
			continue;
		/* /media and /run/nuubos/shares themselves hold volumes only. */
		if (!strcmp(out, roots[i]))
			return allow_root && i == 0;
		if (i > 0) {
			/* A volume folder itself: listing only. */
			const char *rest = out + strlen(roots[i]) + 1;

			if (!strchr(rest, '/') && !allow_root)
				return false;
		}
		/* Internal names anywhere in the path are refused. */
		for (const char *p = out; (p = strchr(p, '/')); ) {
			char part[NAME_MAX + 1];
			size_t len;

			p++;
			len = strcspn(p, "/");
			if (len > NAME_MAX)
				return false;
			memcpy(part, p, len);
			part[len] = '\0';
			if (internal_name(part))
				return false;
		}
		return true;
	}
	return false;
}

static void progress(void)
{
	int percent;

	if (!as_job || total_bytes <= 0)
		return;
	percent = (int)(done_bytes * 100 / total_bytes);
	if (percent > 100)
		percent = 100;
	if (percent != last_percent) {
		last_percent = percent;
		printf("@progress %d\n", percent);
		fflush(stdout);
	}
}

static long long tree_size(const char *path)
{
	struct stat st;
	long long sum = 0;
	DIR *d;
	struct dirent *e;

	if (lstat(path, &st) != 0)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
	d = opendir(path);
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char child[PATH_MAX];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child))
			continue;
		sum += tree_size(child);
	}
	closedir(d);
	return sum;
}

static int remove_tree(const char *path)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	int rc = 0;

	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	d = opendir(path);
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char child[PATH_MAX];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) {
			rc = -1;
			continue;
		}
		if (remove_tree(child) != 0)
			rc = -1;
	}
	closedir(d);
	if (rmdir(path) != 0)
		rc = -1;
	return rc;
}

static int copy_file(const char *src, const char *dst, mode_t mode)
{
	static char buf[CHUNK];
	int in, out;
	ssize_t n = 0;
	int rc = 0;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	snprintf(partial, sizeof(partial), "%s.part", dst);
	out = open(partial, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 0777);
	if (out < 0) {
		close(in);
		partial[0] = '\0';
		return -1;
	}
	while (!cancelled && (n = read(in, buf, sizeof(buf))) > 0) {
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));

			if (w < 0) {
				if (errno == EINTR)
					continue;
				rc = -1;
				break;
			}
			off += w;
		}
		if (rc)
			break;
		done_bytes += n;
		progress();
	}
	if (n < 0)
		rc = -1;
	if (fsync(out) != 0)
		rc = -1;
	if (close(out) != 0)
		rc = -1;
	close(in);
	if (rc == 0 && !cancelled && rename(partial, dst) == 0) {
		partial[0] = '\0';
		return 0;
	}
	unlink(partial);
	partial[0] = '\0';
	return -1;
}

static int copy_tree(const char *src, const char *dst)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	int rc = 0;

	if (cancelled || lstat(src, &st) != 0)
		return -1;
	if (S_ISREG(st.st_mode))
		return copy_file(src, dst, st.st_mode);
	if (!S_ISDIR(st.st_mode))
		return 0; /* devices, sockets, symlinks: not user content */
	if (mkdir(dst, 0755) != 0 && errno != EEXIST)
		return -1;
	d = opendir(src);
	if (!d)
		return -1;
	while (!cancelled && (e = readdir(d))) {
		char s[PATH_MAX], t[PATH_MAX];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (snprintf(s, sizeof(s), "%s/%s", src, e->d_name) >= (int)sizeof(s) ||
		    snprintf(t, sizeof(t), "%s/%s", dst, e->d_name) >= (int)sizeof(t)) {
			rc = -1;
			break;
		}
		if (copy_tree(s, t) != 0) {
			rc = -1;
			break;
		}
	}
	closedir(d);
	return cancelled ? -1 : rc;
}

/* "<dir>/<name>", or "<dir>/<stem> (n)<ext>" when that name is taken. */
static bool free_name(const char *dir, const char *name, char *out)
{
	const char *dot = strrchr(name, '.');
	size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);

	if (snprintf(out, PATH_MAX, "%s/%s", dir, name) >= PATH_MAX)
		return false;
	for (int n = 2; access(out, F_OK) == 0 && n < 1000; n++)
		if (snprintf(out, PATH_MAX, "%s/%.*s (%d)%s", dir, (int)stem, name, n,
			     dot && dot != name ? dot : "") >= PATH_MAX)
			return false;
	return access(out, F_OK) != 0;
}

static bool valid_name(const char *name)
{
	return name[0] && strlen(name) < 256 && !strchr(name, '/') && strcmp(name, ".") &&
	       strcmp(name, "..") && name[0] != '.' && !strpbrk(name, "\r\n\t");
}

static int transfer(const char *src_in, const char *dir_in, bool move)
{
	char src[PATH_MAX], dir[PATH_MAX], dst[PATH_MAX];
	const char *base;
	struct stat st, dst_st;
	struct statvfs vfs;

	if (!resolve(src_in, src, true, false) || !resolve(dir_in, dir, true, true))
		return fail("path");
	if (stat(dir, &dst_st) != 0 || !S_ISDIR(dst_st.st_mode))
		return fail("destination");
	if (under(dir, src))
		return fail("inside");
	base = strrchr(src, '/') + 1;
	if (!free_name(dir, base, dst))
		return fail("name");
	if (lstat(src, &st) != 0)
		return fail("source");
	/* Same filesystem: a move is a rename. */
	if (move && st.st_dev == dst_st.st_dev) {
		if (rename(src, dst) != 0)
			return fail("io");
		sync();
		printf(as_job ? "@result %s\n" : "OK %s\n", dst);
		return 0;
	}
	total_bytes = tree_size(src);
	if (statvfs(dir, &vfs) == 0 &&
	    (long long)vfs.f_bavail * (long long)vfs.f_frsize < total_bytes)
		return fail("space");
	progress();
	if (copy_tree(src, dst) != 0) {
		/* Nothing half-copied remains under the final name. */
		remove_tree(dst);
		return fail(cancelled ? "cancelled" : "io");
	}
	sync();
	if (move && remove_tree(src) != 0)
		return fail("source-kept");
	printf(as_job ? "@result %s\n" : "OK %s\n", dst);
	return 0;
}

static void print_root(const char *name, const char *path)
{
	struct statvfs v;

	if (statvfs(path, &v) == 0)
		printf("root=%s\t%s\t%lld\t%lld\t%d\n", name, path,
		       (long long)v.f_blocks * (long long)v.f_frsize,
		       (long long)v.f_bavail * (long long)v.f_frsize,
		       (v.f_flag & ST_RDONLY) ? 1 : 0);
}

static int cmd_roots(void)
{
	const char *dirs[] = { MEDIA_ROOT, SHARES_ROOT };

	print_root("userdata", USERDATA_ROOT);
	for (size_t i = 0; i < 2; i++) {
		DIR *d = opendir(dirs[i]);
		struct dirent *e;

		if (!d)
			continue;
		while ((e = readdir(d))) {
			char path[PATH_MAX];
			struct stat a, b;

			if (e->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s", dirs[i], e->d_name);
			/* Only mounted volumes (a different device than the parent). */
			if (stat(path, &a) != 0 || stat(dirs[i], &b) != 0 || a.st_dev == b.st_dev)
				continue;
			print_root(i == 0 ? "media" : "share", path);
		}
		closedir(d);
	}
	puts("end=1");
	return 0;
}

static int cmd_list(const char *in)
{
	char path[PATH_MAX];
	DIR *d;
	struct dirent *e;

	if (!resolve(in, path, true, true))
		return fail("path");
	d = opendir(path);
	if (!d)
		return fail("io");
	while ((e = readdir(d))) {
		char child[PATH_MAX];
		struct stat st;

		if (e->d_name[0] == '.' || internal_name(e->d_name) || strpbrk(e->d_name, "\t\n\r"))
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child) ||
		    stat(child, &st) != 0)
			continue;
		if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
			continue;
		printf("entry=%c\t%lld\t%lld\t%s\n", S_ISDIR(st.st_mode) ? 'd' : 'f',
		       S_ISDIR(st.st_mode) ? 0LL : (long long)st.st_size, (long long)st.st_mtime,
		       e->d_name);
	}
	closedir(d);
	puts("end=1");
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	char path[PATH_MAX], target[PATH_MAX];

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	as_job = getenv("NUUBOS_JOB_ID") != NULL;

	if (argc == 2 && !strcmp(argv[1], "roots"))
		return cmd_roots();
	if (argc == 3 && !strcmp(argv[1], "list"))
		return cmd_list(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "info")) {
		struct stat st;

		if (!resolve(argv[2], path, true, true) || stat(path, &st) != 0)
			return fail("path");
		printf("type=%c\nsize=%lld\nmtime=%lld\nend=1\n", S_ISDIR(st.st_mode) ? 'd' : 'f',
		       tree_size(path), (long long)st.st_mtime);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "mkdir")) {
		const char *name = strrchr(argv[2], '/');

		if (!name || !valid_name(name + 1) || !resolve(argv[2], path, false, false))
			return fail("path");
		if (mkdir(path, 0755) != 0)
			return fail(errno == EEXIST ? "exists" : "io");
		puts("OK");
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "rename")) {
		char *slash;

		if (!valid_name(argv[3]) || !resolve(argv[2], path, true, false))
			return fail("path");
		snprintf(target, sizeof(target), "%s", path);
		slash = strrchr(target, '/');
		if (!slash || (size_t)(slash - target) + strlen(argv[3]) + 2 >= sizeof(target))
			return fail("path");
		strcpy(slash + 1, argv[3]);
		if (access(target, F_OK) == 0)
			return fail("exists");
		if (rename(path, target) != 0)
			return fail("io");
		puts("OK");
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "delete")) {
		if (!resolve(argv[2], path, true, false))
			return fail("path");
		if (remove_tree(path) != 0)
			return fail("io");
		sync();
		puts("OK");
		return 0;
	}
	if (argc == 4 && (!strcmp(argv[1], "copy") || !strcmp(argv[1], "move")))
		return transfer(argv[2], argv[3], !strcmp(argv[1], "move"));
	fprintf(stderr, "Usage: nuubos-filesctl roots | list PATH | info PATH | mkdir PATH | "
			"rename PATH NAME | delete PATH | copy SRC DSTDIR | move SRC DSTDIR\n");
	return 2;
}
