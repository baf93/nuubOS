/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-portmaster-get — installs and updates PortMaster (EPIC-058) for
 * nuubos-portsd.
 *
 * PortMaster (MIT) is not part of the image: on the user's request this
 * helper fetches the release of PortMaster's stable channel from its GitHub
 * releases, as PortMaster's own installer does on other systems.
 *
 *   nuubos-portmaster-get check
 *       @latest <version>
 *   nuubos-portmaster-get install <root>
 *       @progress <percent>... then @done <version>
 *   nuubos-portmaster-get rollback <root>
 *       @done <version>   (swaps the files of the last update back; rolling
 *                          back twice returns to the newer release)
 *   nuubos-portmaster-get remove <root>
 *       @done
 *
 * Failures print "@error network|space|archive|checksum|write" and exit
 * non-zero.
 *
 * <root> is the ports folder (/userdata/roms/ports): PortMaster lives in
 * <root>/PortMaster, where ports look for it; nuubOS keeps its own files in
 * <root>/.nuubos. The release zip is downloaded to <root>/.nuubos, checked
 * against the MD5 PortMaster publishes with it and extracted into a staging
 * tree (Python's zipfile, which keeps every member inside the tree). Its
 * files then replace those of the installed tree one by one: what they
 * replace goes to <root>/.nuubos/previous for a rollback, and everything
 * PortMaster downloaded itself (runtimes, themes, its configuration) stays
 * where it is. SIGTERM stops the download and removes the partial files.
 */

#include <ctype.h>
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
#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/evp.h>

#define VERSION_URL "https://github.com/PortsMaster/PortMaster-GUI/releases/latest/download/version.json"
#define RELEASE_PREFIX "https://github.com/PortsMaster/PortMaster-GUI/releases/download/"
#define CHANNEL "stable"
/* nuubOS's PortMaster module (the "mod_<CFW_NAME>.txt" ports source), used
 * until PortMaster ships its own. */
#define MOD_FILE "mod_nuubOS.txt"
#define MOD_SOURCE "/usr/share/nuubos/ports/" MOD_FILE
#define PYTHON "/usr/bin/python3"
/* ~25 MB download, ~45 MB extracted next to the installed tree. */
#define MIN_FREE_BYTES (256ULL * 1024 * 1024)

static volatile sig_atomic_t stop_requested;

static void handle_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void event(const char *fmt, const char *value)
{
	printf(fmt, value);
	putchar('\n');
	fflush(stdout);
}

static int fail(const char *reason)
{
	event("@error %s", reason);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Release of the stable channel                                       */
/* ------------------------------------------------------------------ */

struct text {
	char data[8192];
	size_t used;
};

struct release {
	char version[32];
	char url[512];
	char md5[33];
};

static size_t text_write(char *ptr, size_t size, size_t nmemb, void *user)
{
	struct text *t = user;
	size_t len = size * nmemb;

	if (t->used + len >= sizeof(t->data))
		return 0;
	memcpy(t->data + t->used, ptr, len);
	t->used += len;
	t->data[t->used] = '\0';
	return len;
}

static CURL *new_request(const char *url)
{
	CURL *curl = curl_easy_init();

	if (!curl)
		return NULL;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	/* GitHub release assets redirect to its download host. */
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	/* Abort a stalled transfer (< 1 KB/s for 30 s). */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	return curl;
}

/* The string value of "key" inside obj (a flat JSON object, no escapes in
 * the values PortMaster writes there). */
static bool json_string(const char *obj, const char *end, const char *key, char *out, size_t size)
{
	char pattern[32];
	const char *p;
	const char *q;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(obj, pattern);
	if (!p || p >= end)
		return false;
	p += strlen(pattern);
	while (p < end && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (p >= end || *p != '"')
		return false;
	q = strchr(++p, '"');
	if (!q || q >= end || (size_t)(q - p) >= size)
		return false;
	memcpy(out, p, (size_t)(q - p));
	out[q - p] = '\0';
	return true;
}

/* version.json: {"alpha": {...}, "beta": {...}, "stable": {"version",
 * "url", "md5"}}. */
static bool latest_release(struct release *r)
{
	struct text t = { .used = 0 };
	CURL *curl = new_request(VERSION_URL);
	const char *obj;
	const char *end;
	CURLcode rc;

	if (!curl)
		return false;
	t.data[0] = '\0';
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, text_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	rc = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK)
		return false;
	obj = strstr(t.data, "\"" CHANNEL "\"");
	if (!obj || !(obj = strchr(obj, '{')) || !(end = strchr(obj, '}')))
		return false;
	if (!json_string(obj, end, "version", r->version, sizeof(r->version)) ||
	    !json_string(obj, end, "url", r->url, sizeof(r->url)) ||
	    !json_string(obj, end, "md5", r->md5, sizeof(r->md5)))
		return false;
	if (strncmp(r->url, RELEASE_PREFIX, strlen(RELEASE_PREFIX)) != 0 || strlen(r->md5) != 32 ||
	    !r->version[0])
		return false;
	for (const char *p = r->version; *p; p++)
		if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-')
			return false;
	for (char *p = r->md5; *p; p++) {
		if (!isxdigit((unsigned char)*p))
			return false;
		*p = (char)tolower((unsigned char)*p);
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Download                                                            */
/* ------------------------------------------------------------------ */

struct download {
	int fd;
	EVP_MD_CTX *md5;
	unsigned long long total;    /* Content-Length, 0 if unknown */
	unsigned long long received;
	int percent;
	const char *error;
};

static size_t download_write(char *ptr, size_t size, size_t nmemb, void *user)
{
	struct download *d = user;
	size_t len = size * nmemb;
	const char *p = ptr;
	size_t left = len;

	if (stop_requested || d->error)
		return 0;
	if (EVP_DigestUpdate(d->md5, ptr, len) != 1) {
		d->error = "checksum";
		return 0;
	}
	while (left > 0) {
		ssize_t w = write(d->fd, p, left);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0) {
			d->error = errno == ENOSPC ? "space" : "write";
			return 0;
		}
		p += w;
		left -= (size_t)w;
	}
	d->received += len;
	if (d->total > 0) {
		/* The extraction after the download takes the last percent. */
		int percent = (int)(d->received * 100 / d->total);

		if (percent > 99)
			percent = 99;
		if (percent != d->percent) {
			char text[8];

			d->percent = percent;
			snprintf(text, sizeof(text), "%d", percent);
			event("@progress %s", text);
		}
	}
	return len;
}

static size_t header_line(char *ptr, size_t size, size_t nmemb, void *user)
{
	struct download *d = user;
	size_t len = size * nmemb;

	/* Each redirect sends its own headers: the last length wins. */
	if (len > 15 && !strncasecmp(ptr, "content-length:", 15))
		d->total = strtoull(ptr + 15, NULL, 10);
	return len;
}

static const char *download(const struct release *r, const char *path)
{
	struct download d = { .fd = -1, .percent = -1 };
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_len = 0;
	char hex[33];
	CURL *curl;
	CURLcode rc;

	d.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (d.fd < 0)
		return "write";
	d.md5 = EVP_MD_CTX_new();
	curl = new_request(r->url);
	if (!d.md5 || !curl || EVP_DigestInit_ex(d.md5, EVP_md5(), NULL) != 1) {
		EVP_MD_CTX_free(d.md5);
		if (curl)
			curl_easy_cleanup(curl);
		close(d.fd);
		return "write";
	}
	event("@progress %s", "0");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &d);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_line);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &d);
	rc = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (close(d.fd) != 0 && !d.error)
		d.error = "write";
	if (!d.error && EVP_DigestFinal_ex(d.md5, digest, &digest_len) != 1)
		d.error = "checksum";
	EVP_MD_CTX_free(d.md5);
	if (stop_requested)
		return "cancelled";
	if (d.error)
		return d.error;
	if (rc != CURLE_OK)
		return "network";
	for (unsigned int i = 0; i < digest_len && i < 16; i++)
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	if (digest_len != 16 || strcmp(hex, r->md5) != 0)
		return "checksum";
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Tree helpers                                                        */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int make_parent(const char *path)
{
	char tmp[PATH_MAX];
	char *slash;

	snprintf(tmp, sizeof(tmp), "%s", path);
	slash = strrchr(tmp, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	return mkdir_p(tmp);
}

static int remove_tree(const char *path)
{
	struct stat st;
	DIR *dir;
	struct dirent *e;
	int rc = 0;

	if (lstat(path, &st) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	dir = opendir(path);
	if (!dir)
		return -1;
	while ((e = readdir(dir)) != NULL) {
		char child[PATH_MAX];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
		if (remove_tree(child) != 0)
			rc = -1;
	}
	closedir(dir);
	if (rmdir(path) != 0)
		rc = -1;
	return rc;
}

static int copy_file(const char *from, const char *to)
{
	char buf[65536];
	int in = open(from, O_RDONLY | O_CLOEXEC);
	int out;
	ssize_t n;
	int rc = 0;

	if (in < 0)
		return -1;
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			rc = -1;
			break;
		}
	}
	if (n < 0)
		rc = -1;
	close(in);
	if (close(out) != 0)
		rc = -1;
	return rc;
}

static void read_version(const char *tree, char *out, size_t size)
{
	char path[PATH_MAX];
	FILE *fp;

	out[0] = '\0';
	snprintf(path, sizeof(path), "%s/version", tree);
	fp = fopen(path, "r");
	if (!fp)
		return;
	if (!fgets(out, (int)size, fp))
		out[0] = '\0';
	fclose(fp);
	out[strcspn(out, "\r\n")] = '\0';
}

/* The tree is a PortMaster release. */
static bool valid_tree(const char *dir)
{
	static const char *const files[] = { "version", "PortMaster.sh", "control.txt", "pugwash" };
	char path[PATH_MAX];

	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
		if (access(path, R_OK) != 0)
			return false;
	}
	return true;
}

/*
 * Moves every file of src (relative path rel) to the same place in dst; the
 * file it replaces there goes to the same place in keep. Directories are
 * merged; files of dst that src does not have stay.
 */
static int move_over(const char *src, const char *dst, const char *keep, const char *rel)
{
	char from[PATH_MAX], to[PATH_MAX], saved[PATH_MAX];
	struct stat st, old;
	DIR *dir;
	struct dirent *e;
	int rc = 0;

	snprintf(from, sizeof(from), "%s%s%s", src, rel[0] ? "/" : "", rel);
	snprintf(to, sizeof(to), "%s%s%s", dst, rel[0] ? "/" : "", rel);
	snprintf(saved, sizeof(saved), "%s%s%s", keep, rel[0] ? "/" : "", rel);
	if (lstat(from, &st) != 0)
		return -1;
	if (S_ISDIR(st.st_mode)) {
		if (lstat(to, &old) == 0 && !S_ISDIR(old.st_mode)) {
			if (make_parent(saved) != 0 || rename(to, saved) != 0)
				return -1;
		}
		if (mkdir_p(to) != 0)
			return -1;
		dir = opendir(from);
		if (!dir)
			return -1;
		while ((e = readdir(dir)) != NULL) {
			char child[PATH_MAX];

			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
			if (move_over(src, dst, keep, child) != 0)
				rc = -1;
		}
		closedir(dir);
		return rc;
	}
	if (lstat(to, &old) == 0) {
		if (make_parent(saved) != 0 || rename(to, saved) != 0)
			return -1;
	}
	if (make_parent(to) != 0 || rename(from, to) != 0)
		return -1;
	return 0;
}

/* python3 -I -m zipfile -e <zip> <dir> */
static bool extract(const char *zip, const char *dir)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		return false;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
		}
		execl(PYTHON, PYTHON, "-I", "-m", "zipfile", "-e", zip, dir, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return false;
		if (stop_requested)
			kill(pid, SIGTERM);
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int install(const char *root)
{
	struct release r;
	char state[PATH_MAX], zip[PATH_MAX], staging[PATH_MAX], tree[PATH_MAX];
	char live[PATH_MAX], previous[PATH_MAX], old[PATH_MAX], path[PATH_MAX];
	char version[32];
	struct statvfs vfs;
	bool shipped_mod;
	bool updating;
	const char *error;

	if (!latest_release(&r))
		return fail("network");
	snprintf(state, sizeof(state), "%s/.nuubos", root);
	snprintf(zip, sizeof(zip), "%s/PortMaster.zip.part", state);
	snprintf(staging, sizeof(staging), "%s/.staging", state);
	snprintf(tree, sizeof(tree), "%s/PortMaster", staging);
	snprintf(live, sizeof(live), "%s/PortMaster", root);
	snprintf(previous, sizeof(previous), "%s/previous", state);
	snprintf(old, sizeof(old), "%s/.old", state);
	if (mkdir_p(state) != 0)
		return fail("write");
	if (statvfs(root, &vfs) == 0 && (unsigned long long)vfs.f_bavail * vfs.f_frsize < MIN_FREE_BYTES)
		return fail("space");
	remove_tree(staging);
	remove_tree(old);

	error = download(&r, zip);
	if (!error && (mkdir_p(staging) != 0 || !extract(zip, staging)))
		error = stop_requested ? "cancelled" : "archive";
	unlink(zip);
	if (!error && !valid_tree(tree))
		error = "archive";
	if (error) {
		remove_tree(staging);
		return fail(error);
	}
	snprintf(path, sizeof(path), "%s/" MOD_FILE, tree);
	shipped_mod = access(path, F_OK) == 0;

	/* The files of the release replace the installed ones; those they
	 * replace become the rollback, the one before it goes away. */
	updating = access(live, F_OK) == 0;
	if (mkdir_p(old) != 0 || move_over(tree, live, old, "") != 0) {
		/* Put back what was moved: the installed release stays. */
		if (updating)
			(void)move_over(old, live, staging, "");
		remove_tree(staging);
		remove_tree(old);
		return fail("write");
	}
	remove_tree(staging);
	snprintf(path, sizeof(path), "%s/version", old);
	if (updating && access(path, F_OK) == 0) {
		remove_tree(previous);
		if (rename(old, previous) != 0)
			remove_tree(old);
	} else {
		remove_tree(old);
	}
	if (!shipped_mod) {
		snprintf(path, sizeof(path), "%s/" MOD_FILE, live);
		(void)copy_file(MOD_SOURCE, path);
	}
	sync();
	read_version(live, version, sizeof(version));
	event("@done %s", version[0] ? version : r.version);
	return 0;
}

/* Back to the files of the release before the last update (and forth). */
static int rollback(const char *root)
{
	char live[PATH_MAX], previous[PATH_MAX], swap[PATH_MAX];
	char version[32];

	snprintf(live, sizeof(live), "%s/PortMaster", root);
	snprintf(previous, sizeof(previous), "%s/.nuubos/previous", root);
	snprintf(swap, sizeof(swap), "%s/.nuubos/.swap", root);
	read_version(previous, version, sizeof(version));
	if (!version[0] || access(live, F_OK) != 0)
		return fail("archive");
	remove_tree(swap);
	if (mkdir_p(swap) != 0 || move_over(previous, live, swap, "") != 0) {
		(void)move_over(swap, live, previous, "");
		remove_tree(swap);
		return fail("write");
	}
	remove_tree(previous);
	if (rename(swap, previous) != 0)
		remove_tree(swap);
	sync();
	read_version(live, version, sizeof(version));
	event("@done %s", version);
	return 0;
}

/* PortMaster and nuubOS's copies; the ports and their data stay. */
static int remove_app(const char *root)
{
	static const char *const dirs[] = {
		".nuubos/.staging", ".nuubos/.old", ".nuubos/.swap", ".nuubos/previous",
		".nuubos/PortMaster.zip.part", "PortMaster",
	};
	int rc = 0;

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char path[PATH_MAX];

		snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
		if (remove_tree(path) != 0)
			rc = -1;
	}
	sync();
	if (rc != 0)
		return fail("write");
	event("@done%s", "");
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int rc;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_stop;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (argc == 2 && !strcmp(argv[1], "check")) {
		struct release r;

		curl_global_init(CURL_GLOBAL_DEFAULT);
		rc = latest_release(&r) ? 0 : 1;
		if (rc == 0)
			event("@latest %s", r.version);
		else
			fail("network");
		curl_global_cleanup();
		return rc;
	}
	if (argc == 3 && !strcmp(argv[1], "install") && argv[2][0] == '/') {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		rc = install(argv[2]);
		curl_global_cleanup();
		return rc;
	}
	if (argc == 3 && !strcmp(argv[1], "remove") && argv[2][0] == '/')
		return remove_app(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "rollback") && argv[2][0] == '/')
		return rollback(argv[2]);
	fprintf(stderr, "usage: nuubos-portmaster-get check | install <root> | rollback <root> | remove <root>\n");
	return 2;
}
