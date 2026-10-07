/* SPDX-License-Identifier: MIT */
/*
 * nuubos-updatectl — OTA updates, network and verification side (EPIC-048).
 *
 *   status        current=, build=, available=, version=, notes=, downloaded=
 *   check         fetch and verify the release manifest
 *   download      fetch the payload (resumable), verify its SHA-256 (a job)
 *   verify        re-check the downloaded payload against the manifest
 *
 * Trust comes from the signature, never from the transport: the manifest
 * (a small key=value file) is signed with the nuubOS release key (Ed25519)
 * and carries the payload size and SHA-256. The public key is compiled in
 * from board/nuubos/common/ota/update-key.pub; a build without it reports
 * "unavailable". Nothing is installed here: nuubos-update-apply installs a
 * verified payload on the next restart and can roll it back.
 *
 * Files: /userdata/.nuubos-update/{manifest,manifest.sig,payload.part,payload}
 * (USERDATA has room; the hidden folder is not shown in Files).
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>
#include <sodium.h>

#ifndef UPDATE_URL
#define UPDATE_URL "https://github.com/baf93/nuubOS/releases/latest/download"
#endif
#ifndef UPDATE_PUBKEY_HEX
#define UPDATE_PUBKEY_HEX ""
#endif
#ifndef UPDATE_DIR
#define UPDATE_DIR "/userdata/.nuubos-update"
#endif
#ifndef OS_RELEASE
#define OS_RELEASE "/etc/os-release"
#endif
#ifndef BUILD_ID_FILE
#define BUILD_ID_FILE "/usr/share/nuubos/build-id"
#endif

static volatile sig_atomic_t cancelled;
static bool as_job;

static void on_term(int sig)
{
	(void)sig;
	cancelled = 1;
}

static int fail(const char *reason)
{
	printf(as_job ? "@error %s\n" : "ERR %s\n", reason);
	return 1;
}

static void trim(char *s)
{
	size_t n = strlen(s);

	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

/* key=value from a file into out; false when absent. */
static bool file_value(const char *path, const char *key, char *out, size_t size)
{
	char line[1024];
	size_t klen = strlen(key);
	FILE *fp = fopen(path, "r");
	bool found = false;

	out[0] = '\0';
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			snprintf(out, size, "%s", line + klen + 1);
			/* os-release values may be quoted. */
			if (out[0] == '"') {
				memmove(out, out + 1, strlen(out));
				if (out[0] && out[strlen(out) - 1] == '"')
					out[strlen(out) - 1] = '\0';
			}
			found = true;
		}
	}
	fclose(fp);
	return found;
}

static void current_version(char *version, size_t vsize, char *build, size_t bsize)
{
	FILE *fp;

	if (!file_value(OS_RELEASE, "VERSION_ID", version, vsize))
		snprintf(version, vsize, "0");
	build[0] = '\0';
	fp = fopen(BUILD_ID_FILE, "r");
	if (fp) {
		if (fgets(build, (int)bsize, fp))
			trim(build);
		fclose(fp);
	}
}

/* Dotted numeric comparison: >0 when a is newer than b. */
static int version_cmp(const char *a, const char *b)
{
	for (;;) {
		char *ea, *eb;
		long x = strtol(a, &ea, 10);
		long y = strtol(b, &eb, 10);

		if (x != y)
			return x > y ? 1 : -1;
		a = ea;
		b = eb;
		if (*a != '.' && *b != '.')
			return 0;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
}

static bool public_key(unsigned char pk[crypto_sign_PUBLICKEYBYTES])
{
	size_t len = 0;

	return strlen(UPDATE_PUBKEY_HEX) == 2 * crypto_sign_PUBLICKEYBYTES &&
	       sodium_hex2bin(pk, crypto_sign_PUBLICKEYBYTES, UPDATE_PUBKEY_HEX,
			      strlen(UPDATE_PUBKEY_HEX), NULL, &len, NULL) == 0 &&
	       len == crypto_sign_PUBLICKEYBYTES;
}

static long read_file(const char *path, unsigned char *buf, size_t size)
{
	FILE *fp = fopen(path, "rb");
	size_t n;

	if (!fp)
		return -1;
	n = fread(buf, 1, size, fp);
	if (ferror(fp) || !feof(fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return (long)n;
}

/* The manifest is trusted only when its Ed25519 signature verifies. */
static bool manifest_verified(void)
{
	static unsigned char manifest[16384];
	unsigned char sig[crypto_sign_BYTES + 1];
	unsigned char pk[crypto_sign_PUBLICKEYBYTES];
	long n, s;

	if (!public_key(pk))
		return false;
	n = read_file(UPDATE_DIR "/manifest", manifest, sizeof(manifest));
	s = read_file(UPDATE_DIR "/manifest.sig", sig, sizeof(sig));
	if (n <= 0 || s != crypto_sign_BYTES)
		return false;
	return crypto_sign_verify_detached(sig, manifest, (unsigned long long)n, pk) == 0;
}

/* A resumed request answered with the whole file (200 instead of 206)
 * restarts the file from the beginning instead of appending to it. */
struct sink {
	FILE *fp;
	bool resuming;
	bool restart;
};

static size_t on_header(char *buf, size_t size, size_t n, void *userdata)
{
	struct sink *s = userdata;
	size_t len = size * n;
	int code;

	if (len > 9 && !strncmp(buf, "HTTP/", 5) && sscanf(strchr(buf, ' ') ? strchr(buf, ' ') : buf, "%d", &code) == 1)
		s->restart = s->resuming && code == 200;
	return len;
}

static size_t to_file(char *ptr, size_t size, size_t n, void *userdata)
{
	struct sink *s = userdata;

	if (s->restart) {
		s->restart = false;
		s->resuming = false;
		if (ftruncate(fileno(s->fp), 0) != 0 || fseek(s->fp, 0, SEEK_SET) != 0)
			return 0;
	}
	return fwrite(ptr, size, n, s->fp);
}

static int progress_cb(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t a, curl_off_t b)
{
	static int last = -1;
	long long base = *(long long *)p;
	long long total;
	int percent;

	(void)a;
	(void)b;
	if (cancelled)
		return 1;
	total = base + dltotal;
	if (as_job && dltotal > 0 && total > 0) {
		percent = (int)((base + dlnow) * 100 / total);
		if (percent != last) {
			last = percent;
			printf("@progress %d\n", percent);
			fflush(stdout);
		}
	}
	return 0;
}

/* GET url into path; with resume, append from the current size. */
static long fetch(const char *url, const char *path, bool resume, long timeout)
{
	CURL *curl = curl_easy_init();
	struct stat st;
	long long have = 0;
	long status = -1;
	FILE *fp;
	CURLcode res;
	struct sink sink;
	char range[32];

	if (!curl)
		return -1;
	if (resume && stat(path, &st) == 0)
		have = (long long)st.st_size;
	fp = fopen(path, resume && have ? "ab" : "wb");
	if (!fp) {
		curl_easy_cleanup(curl);
		return -1;
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "nuubOS-update");
	sink = (struct sink){ .fp = fp, .resuming = have > 0, .restart = false };
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_file);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &sink);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	if (timeout)
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
	/* A stalled connection fails instead of hanging. */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
	/* An explicit range (not RESUME_FROM): a server that ignores it answers
	 * 200 with the whole file, which the sink restarts from zero. */
	if (have) {
		snprintf(range, sizeof(range), "%lld-", have);
		curl_easy_setopt(curl, CURLOPT_RANGE, range);
	}
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &have);
	res = curl_easy_perform(curl);
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	else if (res == CURLE_HTTP_RETURNED_ERROR)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	if (fclose(fp) != 0)
		return -1;
	return status;
}

static bool sha256_matches(const char *path, const char *expected_hex, long long expected_size)
{
	crypto_hash_sha256_state st;
	unsigned char digest[crypto_hash_sha256_BYTES];
	char hex[2 * crypto_hash_sha256_BYTES + 1];
	static unsigned char buf[1 << 20];
	long long total = 0;
	size_t n;
	FILE *fp = fopen(path, "rb");

	if (!fp)
		return false;
	crypto_hash_sha256_init(&st);
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0 && !cancelled) {
		crypto_hash_sha256_update(&st, buf, n);
		total += (long long)n;
	}
	fclose(fp);
	if (cancelled || total != expected_size)
		return false;
	crypto_hash_sha256_final(&st, digest);
	sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
	return !strcasecmp(hex, expected_hex);
}

static bool safe_name(const char *s)
{
	if (!s[0] || strlen(s) > 128)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' && *s != '_')
			return false;
	return true;
}

static int cmd_check(void)
{
	unsigned char pk[crypto_sign_PUBLICKEYBYTES];
	char url[1024], cur[64], build[128], version[64], nbuild[128], notes[512];

	if (!public_key(pk))
		return fail("unavailable");
	if (mkdir(UPDATE_DIR, 0700) != 0 && errno != EEXIST)
		return fail("storage");
	snprintf(url, sizeof(url), "%s/nuubos-update.manifest", UPDATE_URL);
	if (fetch(url, UPDATE_DIR "/manifest.new", false, 30) != 200)
		return fail("network");
	snprintf(url, sizeof(url), "%s/nuubos-update.manifest.sig", UPDATE_URL);
	if (fetch(url, UPDATE_DIR "/manifest.sig.new", false, 30) != 200)
		return fail("network");
	/* Keep a previous good manifest until the new one verifies. */
	rename(UPDATE_DIR "/manifest", UPDATE_DIR "/manifest.old");
	rename(UPDATE_DIR "/manifest.sig", UPDATE_DIR "/manifest.sig.old");
	rename(UPDATE_DIR "/manifest.new", UPDATE_DIR "/manifest");
	rename(UPDATE_DIR "/manifest.sig.new", UPDATE_DIR "/manifest.sig");
	if (!manifest_verified()) {
		rename(UPDATE_DIR "/manifest.old", UPDATE_DIR "/manifest");
		rename(UPDATE_DIR "/manifest.sig.old", UPDATE_DIR "/manifest.sig");
		return fail("signature");
	}
	unlink(UPDATE_DIR "/manifest.old");
	unlink(UPDATE_DIR "/manifest.sig.old");
	current_version(cur, sizeof(cur), build, sizeof(build));
	file_value(UPDATE_DIR "/manifest", "version", version, sizeof(version));
	file_value(UPDATE_DIR "/manifest", "build", nbuild, sizeof(nbuild));
	file_value(UPDATE_DIR "/manifest", "notes", notes, sizeof(notes));
	if (version_cmp(version, cur) > 0 || (version_cmp(version, cur) == 0 && strcmp(nbuild, build)))
		printf("OK available %s\n", version);
	else
		printf("OK current\n");
	return 0;
}

static int cmd_download(void)
{
	char url[1024], payload[160], sha[80], size_text[32];
	long long size;
	long status;

	if (!manifest_verified())
		return fail("signature");
	if (!file_value(UPDATE_DIR "/manifest", "payload", payload, sizeof(payload)) || !safe_name(payload) ||
	    !file_value(UPDATE_DIR "/manifest", "sha256", sha, sizeof(sha)) ||
	    !file_value(UPDATE_DIR "/manifest", "size", size_text, sizeof(size_text)))
		return fail("manifest");
	size = atoll(size_text);
	if (size <= 0)
		return fail("manifest");
	if (access(UPDATE_DIR "/payload", F_OK) == 0 && sha256_matches(UPDATE_DIR "/payload", sha, size)) {
		printf(as_job ? "@result downloaded\n" : "OK downloaded\n");
		return 0;
	}
	unlink(UPDATE_DIR "/payload");
	snprintf(url, sizeof(url), "%s/%s", UPDATE_URL, payload);
	status = fetch(url, UPDATE_DIR "/payload.part", true, 0);
	if (cancelled)
		return fail("cancelled");
	/* 206 partial content or 200 for a fresh download; 416 means the
	 * part already holds everything. */
	if (status != 200 && status != 206 && status != 416)
		return fail("network");
	if (!sha256_matches(UPDATE_DIR "/payload.part", sha, size)) {
		/* A corrupt or tampered part is never kept for resume. */
		unlink(UPDATE_DIR "/payload.part");
		return fail(cancelled ? "cancelled" : "checksum");
	}
	if (rename(UPDATE_DIR "/payload.part", UPDATE_DIR "/payload") != 0)
		return fail("storage");
	sync();
	printf(as_job ? "@result downloaded\n" : "OK downloaded\n");
	return 0;
}

static int cmd_verify(void)
{
	char sha[80], size_text[32];

	if (!manifest_verified())
		return fail("signature");
	if (!file_value(UPDATE_DIR "/manifest", "sha256", sha, sizeof(sha)) ||
	    !file_value(UPDATE_DIR "/manifest", "size", size_text, sizeof(size_text)))
		return fail("manifest");
	if (!sha256_matches(UPDATE_DIR "/payload", sha, atoll(size_text)))
		return fail("checksum");
	puts("OK");
	return 0;
}

static int cmd_status(void)
{
	unsigned char pk[crypto_sign_PUBLICKEYBYTES];
	char cur[64], build[128], version[64] = "", nbuild[128] = "", notes[512] = "";
	bool verified = manifest_verified();
	int available = 0;

	current_version(cur, sizeof(cur), build, sizeof(build));
	if (verified) {
		file_value(UPDATE_DIR "/manifest", "version", version, sizeof(version));
		file_value(UPDATE_DIR "/manifest", "build", nbuild, sizeof(nbuild));
		file_value(UPDATE_DIR "/manifest", "notes", notes, sizeof(notes));
		available = version_cmp(version, cur) > 0 ||
			    (version_cmp(version, cur) == 0 && strcmp(nbuild, build) != 0);
	}
	printf("provider=%s\ncurrent=%s\nbuild=%s\navailable=%d\nversion=%s\nnotes=%s\ndownloaded=%d\nend=1\n",
	       public_key(pk) ? "available" : "unavailable", cur, build, available,
	       available ? version : "", available ? notes : "",
	       available && access(UPDATE_DIR "/payload", F_OK) == 0);
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int rc = 2;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	as_job = getenv("NUUBOS_JOB_ID") != NULL;
	if (sodium_init() < 0)
		return fail("crypto");
	curl_global_init(CURL_GLOBAL_DEFAULT);
	if (argc == 2 && !strcmp(argv[1], "status"))
		rc = cmd_status();
	else if (argc == 2 && !strcmp(argv[1], "check"))
		rc = cmd_check();
	else if (argc == 2 && !strcmp(argv[1], "download"))
		rc = cmd_download();
	else if (argc == 2 && !strcmp(argv[1], "verify"))
		rc = cmd_verify();
	else
		fprintf(stderr, "Usage: nuubos-updatectl status | check | download | verify\n");
	curl_global_cleanup();
	return rc;
}
