/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-tailscale-get — downloads Tailscale's daemon for nuubos-tsd.
 *
 * Tailscale is not part of the nuubOS image: the user asks for it, and this
 * helper fetches the current stable arm64 build from Tailscale's own package
 * server (the static binaries Tailscale publishes for any Linux), keeping
 * only tailscaled (nuubos-tsd talks to it through its local API; the
 * tailscale command line is not needed).
 *
 *   nuubos-tailscale-get check
 *       @latest <version>
 *   nuubos-tailscale-get install <root>
 *       @progress <percent>... then @done <version>
 *   nuubos-tailscale-get remove <root>
 *       @done
 *
 * Failures print "@error network|space|archive|checksum|write" and exit
 * non-zero.
 *
 * The archive is hashed (SHA-256) and unpacked while it downloads (gzip +
 * tar, no temporary tarball); tailscaled goes to <root>/.staging and
 * replaces <root>/app only when the hash matches the one Tailscale
 * publishes next to the archive ("<archive>.sha256"). SIGTERM stops the
 * download and removes the partial file.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <zlib.h>

#ifndef PKGS_BASE
#define PKGS_BASE "https://pkgs.tailscale.com/stable/"
#endif
#ifndef PKGS_PROTOCOLS
#define PKGS_PROTOCOLS "https"
#endif
#define ARCH "arm64"
#define MEMBER "tailscaled"
/* tailscaled is ~40 MB; keep a margin. */
#define MIN_FREE_BYTES (128ULL * 1024 * 1024)

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

struct text {
	char data[16384];
	size_t used;
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
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, PKGS_PROTOCOLS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, PKGS_PROTOCOLS);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	/* Abort a stalled transfer (< 1 KB/s for 30 s). */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	return curl;
}

static bool fetch_text(const char *url, struct text *t)
{
	CURL *curl = new_request(url);
	CURLcode rc;

	t->used = 0;
	t->data[0] = '\0';
	if (!curl)
		return false;
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, text_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, t);
	rc = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return rc == CURLE_OK;
}

/* The package index (?mode=json): "TarballsVersion" and the arm64 archive
 * name ("tailscale_1.104.2_arm64.tgz"), checked to be a plain file name. */
static bool latest_build(char *file, size_t file_size, char *version, size_t version_size)
{
	struct text t;
	cJSON *j, *v, *f;
	bool ok = false;

	if (!fetch_text(PKGS_BASE "?mode=json", &t))
		return false;
	j = cJSON_Parse(t.data);
	v = cJSON_GetObjectItemCaseSensitive(j, "TarballsVersion");
	f = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "Tarballs"), ARCH);
	if (cJSON_IsString(v) && cJSON_IsString(f) && v->valuestring[0] && strlen(v->valuestring) < 32 &&
	    !strchr(f->valuestring, '/') && !strstr(f->valuestring, "..") && strlen(f->valuestring) < 96) {
		snprintf(version, version_size, "%s", v->valuestring);
		snprintf(file, file_size, "%s", f->valuestring);
		ok = true;
	}
	cJSON_Delete(j);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Download: SHA-256 + gzip + tar, one member kept                     */
/* ------------------------------------------------------------------ */

struct extract {
	EVP_MD_CTX *md;
	z_stream z;
	bool z_ready, finished;
	const char *error;
	/* tar */
	unsigned char header[512];
	size_t header_used;
	unsigned long long left, pad;
	bool in_body, keep, found;
	int out;
	const char *out_path;
	/* progress */
	unsigned long long total, received;
	int percent;
};

static unsigned long long octal(const unsigned char *p, size_t len)
{
	unsigned long long v = 0;

	for (size_t i = 0; i < len && p[i]; i++) {
		if (p[i] == ' ')
			continue;
		if (p[i] < '0' || p[i] > '7')
			break;
		v = v * 8 + (p[i] - '0');
	}
	return v;
}

/* "tailscale_1.104.2_arm64/tailscaled": a regular file named MEMBER one
 * directory deep (the archive's own layout). */
static bool wanted(const unsigned char *h)
{
	char name[101];
	const char *slash;

	memcpy(name, h, 100);
	name[100] = '\0';
	slash = strchr(name, '/');
	return (h[156] == '0' || h[156] == '\0') && slash && strchr(slash + 1, '/') == NULL &&
	       !strcmp(slash + 1, MEMBER);
}

static void tar_feed(struct extract *x, const unsigned char *data, size_t len)
{
	while (len > 0 && !x->error && !x->finished) {
		if (x->in_body) {
			size_t n = x->left < len ? (size_t)x->left : len;

			if (x->keep && n > 0) {
				for (size_t off = 0; off < n;) {
					ssize_t w = write(x->out, data + off, n - off);

					if (w < 0) {
						if (errno == EINTR)
							continue;
						x->error = errno == ENOSPC ? "space" : "write";
						return;
					}
					off += (size_t)w;
				}
			}
			x->left -= n;
			data += n;
			len -= n;
			if (x->left == 0) {
				x->in_body = false;
				if (x->keep) {
					x->keep = false;
					x->found = true;
				}
			}
			continue;
		}
		if (x->pad > 0) {
			size_t n = x->pad < len ? (size_t)x->pad : len;

			x->pad -= n;
			data += n;
			len -= n;
			continue;
		}
		{
			size_t n = sizeof(x->header) - x->header_used;

			if (n > len)
				n = len;
			memcpy(x->header + x->header_used, data, n);
			x->header_used += n;
			data += n;
			len -= n;
		}
		if (x->header_used < sizeof(x->header))
			continue;
		x->header_used = 0;
		if (x->header[0] == '\0') {
			/* End of archive. */
			x->finished = true;
			return;
		}
		if (memcmp(x->header + 257, "ustar", 5)) {
			x->error = "archive";
			return;
		}
		x->left = octal(x->header + 124, 12);
		x->pad = (512 - x->left % 512) % 512;
		x->in_body = x->left > 0;
		x->keep = !x->found && wanted(x->header);
		if (x->keep && x->left == 0) {
			x->error = "archive";
			return;
		}
	}
}

static size_t download_write(char *ptr, size_t size, size_t nmemb, void *user)
{
	struct extract *x = user;
	size_t len = size * nmemb;
	unsigned char out[65536];

	if (stop_requested || x->error)
		return 0;
	if (EVP_DigestUpdate(x->md, ptr, len) != 1) {
		x->error = "checksum";
		return 0;
	}
	x->received += len;
	x->z.next_in = (unsigned char *)ptr;
	x->z.avail_in = (unsigned int)len;
	while (x->z.avail_in > 0 && !x->finished) {
		int rc;

		x->z.next_out = out;
		x->z.avail_out = sizeof(out);
		rc = inflate(&x->z, Z_NO_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END) {
			x->error = "archive";
			return 0;
		}
		tar_feed(x, out, sizeof(out) - x->z.avail_out);
		if (x->error)
			return 0;
		if (rc == Z_STREAM_END)
			break;
	}
	if (x->total > 0) {
		int percent = (int)(x->received * 100 / x->total);

		if (percent > 99)
			percent = 99;
		if (percent != x->percent) {
			char text[8];

			x->percent = percent;
			snprintf(text, sizeof(text), "%d", percent);
			event("@progress %s", text);
		}
	}
	return len;
}

static size_t header_line(char *ptr, size_t size, size_t nmemb, void *user)
{
	struct extract *x = user;
	size_t len = size * nmemb;

	if (len > 15 && !strncasecmp(ptr, "content-length:", 15))
		x->total = strtoull(ptr + 15, NULL, 10);
	return len;
}

static int remove_tree(const char *path)
{
	char cmd[PATH_MAX + 16];

	/* Only paths built here, under <root>: no shell metacharacters. */
	if (strpbrk(path, "'\"`$\\;&|<>*?\n") || access(path, F_OK))
		return 0;
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
	return system(cmd);
}

static bool write_file(const char *path, const char *text)
{
	FILE *fp = fopen(path, "w");
	bool ok;

	if (!fp)
		return false;
	ok = fputs(text, fp) >= 0;
	ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0 && ok;
	return fclose(fp) == 0 && ok;
}

static int install(const char *root)
{
	char file[128], version[32], url[256], staging[PATH_MAX], app[PATH_MAX], old[PATH_MAX], path[PATH_MAX];
	unsigned char digest[EVP_MAX_MD_SIZE];
	char hex[2 * EVP_MAX_MD_SIZE + 1], want[96] = "";
	unsigned int dlen = 0;
	struct text sum;
	struct statvfs vfs;
	struct extract x;
	CURL *curl;
	CURLcode rc;

	if (mkdir(root, 0755) && errno != EEXIST)
		return fail("write");
	if (statvfs(root, &vfs) == 0 && (unsigned long long)vfs.f_bavail * vfs.f_frsize < MIN_FREE_BYTES)
		return fail("space");
	if (!latest_build(file, sizeof(file), version, sizeof(version)))
		return fail("network");
	snprintf(url, sizeof(url), PKGS_BASE "%s.sha256", file);
	if (!fetch_text(url, &sum))
		return fail("network");
	sscanf(sum.data, "%64[0-9a-fA-F]", want);
	if (strlen(want) != 64)
		return fail("checksum");

	snprintf(staging, sizeof(staging), "%s/.staging", root);
	snprintf(app, sizeof(app), "%s/app", root);
	snprintf(old, sizeof(old), "%s/.old", root);
	remove_tree(staging);
	remove_tree(old);
	if (mkdir(staging, 0755))
		return fail("write");
	snprintf(path, sizeof(path), "%s/" MEMBER, staging);

	memset(&x, 0, sizeof(x));
	x.percent = -1;
	x.out_path = path;
	x.out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	x.md = EVP_MD_CTX_new();
	if (x.out < 0 || !x.md || EVP_DigestInit_ex(x.md, EVP_sha256(), NULL) != 1 ||
	    inflateInit2(&x.z, 15 + 16) != Z_OK) {
		if (x.out >= 0)
			close(x.out);
		EVP_MD_CTX_free(x.md);
		remove_tree(staging);
		return fail("write");
	}
	snprintf(url, sizeof(url), PKGS_BASE "%s", file);
	curl = new_request(url);
	if (!curl) {
		rc = CURLE_FAILED_INIT;
	} else {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &x);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_line);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &x);
		rc = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
	}
	inflateEnd(&x.z);
	if (EVP_DigestFinal_ex(x.md, digest, &dlen) != 1)
		dlen = 0;
	EVP_MD_CTX_free(x.md);
	if (fsync(x.out) != 0 && !x.error)
		x.error = "write";
	close(x.out);
	if (stop_requested || rc != CURLE_OK || x.error || !x.found) {
		remove_tree(staging);
		if (stop_requested)
			return 1;
		return fail(x.error ? x.error : rc != CURLE_OK ? "network" : "archive");
	}
	for (unsigned int i = 0; i < dlen; i++)
		snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	if (dlen != 32 || strcasecmp(hex, want)) {
		remove_tree(staging);
		return fail("checksum");
	}
	chmod(path, 0755);
	snprintf(path, sizeof(path), "%s/VERSION", staging);
	if (!write_file(path, version)) {
		remove_tree(staging);
		return fail("write");
	}
	/* Swap: the old build goes away only once the new one is in place. */
	if (access(app, F_OK) == 0 && rename(app, old)) {
		remove_tree(staging);
		return fail("write");
	}
	if (rename(staging, app)) {
		rename(old, app);
		remove_tree(staging);
		return fail("write");
	}
	remove_tree(old);
	sync();
	event("@done %s", version);
	return 0;
}

static int remove_app(const char *root)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/.staging", root);
	remove_tree(path);
	snprintf(path, sizeof(path), "%s/app", root);
	if (remove_tree(path))
		return fail("write");
	sync();
	event("@done%s", "");
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int rc;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_stop;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	if (argc == 2 && !strcmp(argv[1], "check")) {
		char file[128], version[32];

		rc = latest_build(file, sizeof(file), version, sizeof(version)) ? (event("@latest %s", version), 0)
										   : fail("network");
	} else if (argc == 3 && !strcmp(argv[1], "install") && argv[2][0] == '/') {
		rc = install(argv[2]);
	} else if (argc == 3 && !strcmp(argv[1], "remove") && argv[2][0] == '/') {
		rc = remove_app(argv[2]);
	} else {
		fprintf(stderr, "Usage: nuubos-tailscale-get check | install ROOT | remove ROOT\n");
		rc = 2;
	}
	curl_global_cleanup();
	return rc;
}
