/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-openh264-get — downloads Cisco's OpenH264 binary on the user's
 * request (Miracast needs H.264).
 *
 *   nuubos-openh264-get install DIR | remove DIR
 *
 * Cisco distributes this binary under its own licence and pays the MPEG LA
 * AVC royalties for it, on the condition that every device downloads it
 * from Cisco itself: nuubOS never ships or mirrors it. The version is
 * pinned and both the archive and the library are checked against SHA-256
 * digests recorded when it was added (Cisco also publishes a signed MD5).
 *
 * Output lines: @progress N, @error network|space|verify|write, @done.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/evp.h>

#define VERSION "2.6.0"
#define URL "http://ciscobinary.openh264.org/libopenh264-" VERSION "-linux-arm64.8.so.bz2"
#define SHA256_BZ2 "a78aea7970150f46bcd3bb7994c9e6dd90bd7a9ea785920f5a73f6964e3fcda7"
#define SHA256_SO "12e7b33623667cdab0e575170c147b1b36eadb77d0d2aa7ceb5afd3e58902140"
#define LIB_NAME "libopenh264.so"
#define NEEDED_BYTES (4 * 1024 * 1024)

static int last_percent = -1;

static void say(const char *line)
{
	printf("%s\n", line);
	fflush(stdout);
}

static int progress(void *user, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un)
{
	(void)user; (void)ut; (void)un;
	if (total > 0) {
		int percent = (int)(now * 100 / total);

		if (percent != last_percent) {
			last_percent = percent;
			printf("@progress %d\n", percent);
			fflush(stdout);
		}
	}
	return 0;
}

static int sha256_file(const char *path, char out[65])
{
	unsigned char buf[65536], digest[32];
	unsigned int len = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	FILE *fp = fopen(path, "rb");
	size_t n;

	if (!ctx || !fp || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		if (fp)
			fclose(fp);
		EVP_MD_CTX_free(ctx);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		EVP_DigestUpdate(ctx, buf, n);
	fclose(fp);
	EVP_DigestFinal_ex(ctx, digest, &len);
	EVP_MD_CTX_free(ctx);
	for (unsigned int i = 0; i < len; i++)
		snprintf(out + 2 * i, 3, "%02x", digest[i]);
	return 0;
}

static int run_bzcat(const char *in, const char *out)
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int ifd = open(in, O_RDONLY), ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (ifd < 0 || ofd < 0)
			_exit(1);
		dup2(ifd, STDIN_FILENO);
		dup2(ofd, STDOUT_FILENO);
		execlp("bzcat", "bzcat", (char *)NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* The folder and its parents (/userdata/cast does not exist on a new card). */
static int mkdir_parents(const char *dir)
{
	char path[512];

	if (snprintf(path, sizeof(path), "%s", dir) >= (int)sizeof(path))
		return -1;
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(path, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

static int install(const char *dir)
{
	char staging[512], bz2[600], so[600], final[600], digest[65];
	struct statvfs vfs;
	CURL *curl;
	FILE *fp;
	CURLcode rc;
	long http = 0;

	snprintf(staging, sizeof(staging), "%s/.staging", dir);
	if (mkdir_parents(dir) != 0 || (mkdir(staging, 0755) != 0 && errno != EEXIST)) {
		say("@error write");
		return 1;
	}
	if (statvfs(dir, &vfs) == 0 && (unsigned long long)vfs.f_bavail * vfs.f_frsize < NEEDED_BYTES) {
		say("@error space");
		return 1;
	}
	snprintf(bz2, sizeof(bz2), "%s/lib.so.bz2", staging);
	snprintf(so, sizeof(so), "%s/" LIB_NAME, staging);
	snprintf(final, sizeof(final), "%s/" LIB_NAME, dir);
	fp = fopen(bz2, "wb");
	if (!fp) {
		say("@error write");
		return 1;
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();
	if (!curl) {
		fclose(fp);
		say("@error network");
		return 1;
	}
	say("@progress 0");
	curl_easy_setopt(curl, CURLOPT_URL, URL);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "nuubOS");
	rc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
	curl_easy_cleanup(curl);
	if (fclose(fp) != 0 || rc != CURLE_OK || http != 200) {
		fprintf(stderr, "download failed: %s, HTTP %ld\n", curl_easy_strerror(rc), http);
		unlink(bz2);
		say(rc == CURLE_WRITE_ERROR ? "@error space" : "@error network");
		return 1;
	}
	/* Integrity: the pinned archive, then the pinned library. */
	if (sha256_file(bz2, digest) != 0 || strcmp(digest, SHA256_BZ2)) {
		fprintf(stderr, "archive digest %s\n", digest);
		unlink(bz2);
		say("@error verify");
		return 1;
	}
	if (run_bzcat(bz2, so) != 0 || sha256_file(so, digest) != 0 || strcmp(digest, SHA256_SO)) {
		unlink(bz2);
		unlink(so);
		say("@error verify");
		return 1;
	}
	unlink(bz2);
	if (rename(so, final) != 0) {
		unlink(so);
		say("@error write");
		return 1;
	}
	rmdir(staging);
	sync();
	say("@progress 100");
	say("@done");
	return 0;
}

static int remove_lib(const char *dir)
{
	char path[600];

	snprintf(path, sizeof(path), "%s/" LIB_NAME, dir);
	if (unlink(path) != 0 && errno != ENOENT) {
		say("@error write");
		return 1;
	}
	say("@done");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "install"))
		return install(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "remove"))
		return remove_lib(argv[2]);
	fprintf(stderr, "Usage: nuubos-openh264-get install|remove DIR\n");
	return 2;
}
