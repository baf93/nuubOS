/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-updatectl — OTA updates, network and verification side (EPIC-048).
 *
 *   status        provider=, channel=, auto=, current=, build=, available=,
 *                 version=, notes= (release notes, escaped), downloaded=
 *   channel [NAME] show or choose the release channel (stable|rc|nightly)
 *   auto [on|off] show or set the daily automatic check (default on;
 *                 run by nuubos-update-auto)
 *   check         fetch and verify the release manifests of the channel
 *                 and of every more stable one (nightly sees nightly, rc
 *                 and stable; rc sees rc and stable; stable only stable)
 *                 and keep the newest: OK available V | OK current |
 *                 OK none (nothing published on them) | ERR network |
 *                 server | signature
 *   download      fetch the payload (resumable), verify its SHA-256 (a job)
 *   verify        re-check the downloaded payload against the manifest
 *   write DEVICE  unpack the verified system image onto DEVICE (the
 *                 inactive system slot), then read it back and check it
 *
 * Releases are GitHub Releases. Each channel has a fixed release tag
 * "channel-<name>" whose assets are the current manifest and signature
 * (replaced at every publication); the manifest names the payload by
 * "<release tag>/<file>", a versioned release of its own.
 *
 * Sources (user decision 2026-11-10): the same layout is served by
 * SourceForge (UPDATE_SOURCES, the backend for every channel) and, for
 * stable releases, by GitHub (UPDATE_URL, the emergency fallback only). A
 * verified manifest may carry "sources=" (up to MAX_SOURCES https base
 * URLs, space separated), kept in SOURCES_FILE, which then replaces
 * UPDATE_SOURCES. Every check asks all those sources and keeps the newest
 * verified manifest (a host that syncs late cannot hide an update); GitHub
 * is asked only when none of them answers at all, so consoles do not poll
 * it. The payload is fetched from them in order, GitHub last. Only a
 * signed manifest changes the list, and a host can never redirect the
 * consoles by itself.
 *
 * Trust comes from the signature, never from the transport: the manifest
 * (a small key=value file) is signed with the nuubOS release key (Ed25519)
 * and carries the payload size and SHA-256 and the SHA-256 of the system
 * image inside it. Its channel must be the device's or a more stable one
 * (the device keeps its own channel when it installs it), and only a
 * higher build serial than the running one is offered (an old signed
 * manifest can never downgrade). The public key is compiled in from
 * board/nuubos/common/ota/update-key.pub; a build without it reports
 * "unavailable". nuubos-update-apply installs into the inactive A/B slot.
 *
 * Files: /userdata/.nuubos-update/{manifest,manifest.sig,payload.part,payload}
 * (USERDATA has room; the hidden folder is not shown in Files); the
 * channel in /state/update/channel, auto-off when the daily check is off.
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
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <curl/curl.h>
#include <sodium.h>
#include <zlib.h>

#ifndef UPDATE_URL
#define UPDATE_URL "https://github.com/baf93/nuubOS/releases/download"
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
#ifndef BUILD_SERIAL_FILE
#define BUILD_SERIAL_FILE "/usr/share/nuubos/build-serial"
#endif
#ifndef CHANNEL_FILE
#define CHANNEL_FILE "/state/update/channel"
#endif
#ifndef AUTO_FILE
#define AUTO_FILE "/state/update/auto-off"
#endif
#ifndef UPDATE_LOCK
#define UPDATE_LOCK "/run/nuubos-update.lock"
#endif
#ifndef SOURCES_FILE
#define SOURCES_FILE "/state/update/sources"
#endif
#define MAX_SOURCES 4
#ifndef UPDATE_SOURCES
#define UPDATE_SOURCES "https://downloads.sourceforge.net/project/nuubos"
#endif
/* Host tests serve the sources over plain http. */
#ifndef SOURCE_SCHEME
#define SOURCE_SCHEME "https://"
#endif
#ifndef UPDATE_DEFAULT_CHANNEL
#define UPDATE_DEFAULT_CHANNEL "stable"
#endif
#define MANIFEST_FORMAT 2
/* notes= : the release notes, one line ("\n" = new line, "\\" = \). */
#define NOTES_MAX 4096

static const char *const channels[] = { "stable", "rc", "nightly" };
/* Names before 2026-10-14, still found in CHANNEL_FILE: old -> new. */
static const char *const renamed_channels[][2] = { { "beta", "rc" }, { "alpha", "nightly" } };

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
	/* Release notes are one escaped line of up to NOTES_MAX bytes. */
	static char line[NOTES_MAX + 64];
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

/* Build serial (seconds since the epoch at image build time): releases are
 * ordered by it, never by the version text. */
static long long current_serial(void)
{
	char text[32] = "";
	FILE *fp = fopen(BUILD_SERIAL_FILE, "r");

	if (fp) {
		if (!fgets(text, sizeof(text), fp))
			text[0] = '\0';
		fclose(fp);
	}
	return atoll(text);
}

/* Position in channels[] (most stable first), -1 when unknown. */
static int channel_rank(const char *name)
{
	for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++)
		if (!strcmp(name, channels[i]))
			return (int)i;
	return -1;
}

static bool valid_channel(const char *name)
{
	return channel_rank(name) >= 0;
}

static void current_channel(char *out, size_t size)
{
	FILE *fp = fopen(CHANNEL_FILE, "r");

	out[0] = '\0';
	if (fp) {
		if (fgets(out, (int)size, fp))
			trim(out);
		fclose(fp);
	}
	for (size_t i = 0; i < sizeof(renamed_channels) / sizeof(renamed_channels[0]); i++)
		if (!strcmp(out, renamed_channels[i][0]))
			snprintf(out, size, "%s", renamed_channels[i][1]);
	if (!valid_channel(out))
		snprintf(out, size, "%s", UPDATE_DEFAULT_CHANNEL);
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
static bool manifest_file_verified(const char *path, const char *sig_path)
{
	static unsigned char manifest[16384];
	unsigned char sig[crypto_sign_BYTES + 1];
	unsigned char pk[crypto_sign_PUBLICKEYBYTES];
	long n, s;

	if (!public_key(pk))
		return false;
	n = read_file(path, manifest, sizeof(manifest));
	s = read_file(sig_path, sig, sizeof(sig));
	if (n <= 0 || s != crypto_sign_BYTES)
		return false;
	return crypto_sign_verify_detached(sig, manifest, (unsigned long long)n, pk) == 0;
}

static bool manifest_verified(void)
{
	return manifest_file_verified(UPDATE_DIR "/manifest", UPDATE_DIR "/manifest.sig");
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

/* GET url into path; with resume, append from the current size. The HTTP
 * status, -1 without one (no connection), -2 when the file may not be on
 * the card. */
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
	bool synced;

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
	synced = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	else if (res == CURLE_HTTP_RETURNED_ERROR)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	if (fclose(fp) != 0 || !synced)
		return -2;
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

/* "<tag>/<file>": two plain names. */
static bool safe_payload(const char *s)
{
	char tag[160];
	const char *slash = strchr(s, '/');

	if (!slash || strchr(slash + 1, '/') || (size_t)(slash - s) >= sizeof(tag))
		return false;
	snprintf(tag, sizeof(tag), "%.*s", (int)(slash - s), s);
	return safe_name(tag) && safe_name(slash + 1) && strcmp(tag, "..") && strcmp(slash + 1, "..");
}

/* Whether releases of CHANNEL are offered to a device on the channel in
 * use: its own and the more stable ones. */
static bool channel_visible(const char *channel)
{
	char wanted[32];
	int rank = channel_rank(channel);

	current_channel(wanted, sizeof(wanted));
	return rank >= 0 && rank <= channel_rank(wanted);
}

/* A verified manifest of a format this build understands, for the channel
 * in use or a more stable one, newer than the running build. */
static bool manifest_offers_update(void)
{
	char format[16], channel[32], serial[32];

	file_value(UPDATE_DIR "/manifest", "format", format, sizeof(format));
	file_value(UPDATE_DIR "/manifest", "channel", channel, sizeof(channel));
	file_value(UPDATE_DIR "/manifest", "serial", serial, sizeof(serial));
	return atoi(format) == MANIFEST_FORMAT && channel_visible(channel) &&
	       atoll(serial) > current_serial();
}

/* The download sources in order: SOURCES_FILE (else UPDATE_SOURCES), then
 * UPDATE_URL, the fallback. Returns how many come before the fallback. */
static int sources(char list[MAX_SOURCES + 1][512])
{
	char line[512], compiled[MAX_SOURCES * 512], *save = NULL, *url;
	int n = 0;
	FILE *fp = fopen(SOURCES_FILE, "r");

	while (fp && n < MAX_SOURCES && fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, SOURCE_SCHEME, strlen(SOURCE_SCHEME)) && strcmp(line, UPDATE_URL))
			snprintf(list[n++], sizeof(list[0]), "%s", line);
	}
	if (fp)
		fclose(fp);
	if (!n) {
		snprintf(compiled, sizeof(compiled), "%s", UPDATE_SOURCES);
		for (url = strtok_r(compiled, " ", &save); url && n < MAX_SOURCES; url = strtok_r(NULL, " ", &save))
			snprintf(list[n++], sizeof(list[0]), "%s", url);
	}
	snprintf(list[n], sizeof(list[0]), "%s", UPDATE_URL);
	return n;
}

/* sources= of the verified manifest becomes the list (none: removed). */
static void save_sources(const char *manifest)
{
	char value[MAX_SOURCES * 512], *save = NULL, *url;
	FILE *fp;
	int n = 0;

	if (!file_value(manifest, "sources", value, sizeof(value)) || !value[0]) {
		unlink(SOURCES_FILE);
		return;
	}
	if (!(fp = fopen(SOURCES_FILE ".tmp", "w")))
		return;
	for (url = strtok_r(value, " ", &save); url && n < MAX_SOURCES; url = strtok_r(NULL, " ", &save))
		if (!strncmp(url, SOURCE_SCHEME, strlen(SOURCE_SCHEME)) && strlen(url) < 500 && !strpbrk(url, "\"'\\")) {
			fprintf(fp, "%s\n", url);
			n++;
		}
	if (fflush(fp) == 0 && fsync(fileno(fp)) == 0 && fclose(fp) == 0)
		rename(SOURCES_FILE ".tmp", SOURCES_FILE);
	else
		unlink(SOURCES_FILE ".tmp");
}

/* No answer at all (DNS, connection, timeout) = the network; an HTTP error
 * (GitHub rate limit, 5xx) = the update service. */
static const char *http_failure(long status)
{
	return status > 0 ? "server" : "network";
}

/*
 * Fetch and verify the manifest of one channel into manifest.cand: 0 =
 * a verified manifest of a format this build understands, published for
 * that channel; 1 = nothing published there (404); else the failure.
 */
/* One host's manifest of the channel into manifest.cand: NULL = verified,
 * "" = nothing published there (404 or another channel/format), else the
 * failure. */
static const char *fetch_from(const char *base, const char *channel, long long *serial)
{
	char url[1024], format[16], name[32], text[32];
	long status;

	snprintf(url, sizeof(url), "%s/channel-%s/nuubos-update.manifest", base, channel);
	status = fetch(url, UPDATE_DIR "/manifest.cand", false, 30);
	if (status == 404)
		return "";
	if (status != 200)
		return http_failure(status);
	snprintf(url, sizeof(url), "%s/channel-%s/nuubos-update.manifest.sig", base, channel);
	status = fetch(url, UPDATE_DIR "/manifest.cand.sig", false, 30);
	if (status != 200)
		return status == 404 ? "" : http_failure(status);
	if (!manifest_file_verified(UPDATE_DIR "/manifest.cand", UPDATE_DIR "/manifest.cand.sig"))
		return "signature";
	file_value(UPDATE_DIR "/manifest.cand", "format", format, sizeof(format));
	file_value(UPDATE_DIR "/manifest.cand", "channel", name, sizeof(name));
	file_value(UPDATE_DIR "/manifest.cand", "serial", text, sizeof(text));
	/* Signed but for another channel or format: nothing usable there. */
	if (atoi(format) != MANIFEST_FORMAT || strcmp(name, channel))
		return "";
	*serial = atoll(text);
	return NULL;
}

/*
 * The newest verified manifest of one channel into manifest.cand: NULL =
 * found, "" = nothing published there, else the failure. GitHub (the
 * fallback) only when no other source answered at all.
 */
static const char *fetch_channel(const char *channel, long long *serial)
{
	char list[MAX_SOURCES + 1][512];
	const char *failure = NULL;
	bool answered = false;
	int n = sources(list);

	*serial = -1;
	for (int i = 0; i <= n; i++) {
		long long s = -1;
		const char *rc;

		if (i == n && (answered || *serial >= 0))
			break;
		rc = fetch_from(list[i], channel, &s);
		/* A 404 or a bad signature is an answer: only silence or HTTP
		 * errors from every source bring in the fallback. */
		if (!rc || !*rc || !strcmp(rc, "signature"))
			answered = true;
		if (rc) {
			if (*rc && (!failure || !strcmp(rc, "signature")))
				failure = rc;
			continue;
		}
		if (s > *serial) {
			*serial = s;
			rename(UPDATE_DIR "/manifest.cand", UPDATE_DIR "/manifest.best");
			rename(UPDATE_DIR "/manifest.cand.sig", UPDATE_DIR "/manifest.best.sig");
		}
	}
	unlink(UPDATE_DIR "/manifest.cand");
	unlink(UPDATE_DIR "/manifest.cand.sig");
	if (*serial < 0)
		return failure ? failure : "";
	rename(UPDATE_DIR "/manifest.best", UPDATE_DIR "/manifest.cand");
	rename(UPDATE_DIR "/manifest.best.sig", UPDATE_DIR "/manifest.cand.sig");
	return NULL;
}

/*
 * The device's channel and every more stable one are checked (user request
 * 2026-10-22: a nightly console also sees RC and stable releases, an RC
 * one stable releases), and the newest verified manifest is kept: an
 * update from a more stable channel is offered and installed like one of
 * the device's own, which keeps its channel. A failure on one channel does
 * not hide an update found on another.
 */
static int cmd_check(void)
{
	unsigned char pk[crypto_sign_PUBLICKEYBYTES];
	char channel[32], version[64];
	const char *failure = NULL;
	long long best = -1;
	int rank;

	if (!public_key(pk))
		return fail("unavailable");
	if (mkdir(UPDATE_DIR, 0700) != 0 && errno != EEXIST)
		return fail("storage");
	current_channel(channel, sizeof(channel));
	for (rank = channel_rank(channel); rank >= 0; rank--) {
		long long serial = -1;
		const char *rc = fetch_channel(channels[rank], &serial);

		if (rc) {
			if (*rc && !failure)
				failure = rc;
			continue;
		}
		if (serial <= best)
			continue;
		best = serial;
		rename(UPDATE_DIR "/manifest.cand", UPDATE_DIR "/manifest.new");
		rename(UPDATE_DIR "/manifest.cand.sig", UPDATE_DIR "/manifest.sig.new");
	}
	unlink(UPDATE_DIR "/manifest.cand");
	unlink(UPDATE_DIR "/manifest.cand.sig");
	if (best < 0) {
		if (failure)
			return fail(failure);
		/* Nothing published on any of them yet: nothing to offer. */
		puts("OK none");
		return 0;
	}
	rename(UPDATE_DIR "/manifest.new", UPDATE_DIR "/manifest");
	rename(UPDATE_DIR "/manifest.sig.new", UPDATE_DIR "/manifest.sig");
	save_sources(UPDATE_DIR "/manifest");
	file_value(UPDATE_DIR "/manifest", "version", version, sizeof(version));
	if (manifest_offers_update())
		printf("OK available %s\n", version);
	else if (failure && best <= current_serial())
		return fail(failure);
	else
		printf("OK current\n");
	return 0;
}

/* Free space on USERDATA for what is still missing of the payload. */
static bool room_for(long long size)
{
	struct statvfs vfs;
	struct stat st;
	long long have = 0;

	if (stat(UPDATE_DIR "/payload.part", &st) == 0)
		have = (long long)st.st_size;
	if (statvfs(UPDATE_DIR, &vfs) != 0)
		return true;
	return (long long)vfs.f_bavail * (long long)vfs.f_frsize > size - have + (64LL << 20);
}

/*
 * payload.part.clean holds the part's size after a download stopped
 * cleanly (done, failed or cancelled, the data synced). A part without it,
 * or of another size, survived a crash or a power cut: its end may be a
 * size that reached the card without the data (2026-10-23, RG35XX Pro SD
 * hang), so the resume starts PART_TAIL earlier. Only then: on a Wi-Fi
 * that drops every few MB a clean stop must keep everything.
 */
#define PART_CLEAN UPDATE_DIR "/payload.part.clean"
#define PART_TAIL (8LL << 20)

static void trim_part(void)
{
	unsigned char text[32] = "";
	struct stat st;
	long long keep;

	if (stat(UPDATE_DIR "/payload.part", &st) != 0)
		return;
	if (read_file(PART_CLEAN, text, sizeof(text) - 1) > 0 && atoll((char *)text) == (long long)st.st_size)
		return;
	keep = (long long)st.st_size > PART_TAIL ? ((long long)st.st_size - PART_TAIL) & ~((1LL << 20) - 1) : 0;
	if (truncate(UPDATE_DIR "/payload.part", keep) != 0)
		unlink(UPDATE_DIR "/payload.part");
}

static void mark_part_clean(void)
{
	struct stat st;
	FILE *fp;

	if (stat(UPDATE_DIR "/payload.part", &st) != 0 || !(fp = fopen(PART_CLEAN ".tmp", "w")))
		return;
	fprintf(fp, "%lld\n", (long long)st.st_size);
	if (fflush(fp) == 0 && fsync(fileno(fp)) == 0 && fclose(fp) == 0)
		rename(PART_CLEAN ".tmp", PART_CLEAN);
	else
		unlink(PART_CLEAN ".tmp");
}

static int cmd_download(void)
{
	char url[1024], payload[160], sha[80], size_text[32];
	long long size;
	long status;

	if (!manifest_verified())
		return fail("signature");
	if (!manifest_offers_update())
		return fail("current");
	if (!file_value(UPDATE_DIR "/manifest", "payload", payload, sizeof(payload)) || !safe_payload(payload) ||
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
	if (!room_for(size))
		return fail("space");
	trim_part();
	unlink(PART_CLEAN);
	/* Source after source; the part resumes on the next one (the checksum
	 * below covers the whole file whichever host gave each piece). */
	{
		char list[MAX_SOURCES + 1][512];
		int n = sources(list);

		status = -1;
		for (int i = 0; i <= n && !cancelled; i++) {
			snprintf(url, sizeof(url), "%s/%s", list[i], payload);
			status = fetch(url, UPDATE_DIR "/payload.part", true, 0);
			if (status == 200 || status == 206 || status == 416 || status == -2)
				break;
		}
	}
	if (status != -2)
		mark_part_clean();
	if (cancelled)
		return fail("cancelled");
	/* 206 partial content or 200 for a fresh download; 416 means the
	 * part already holds everything. */
	if (status != 200 && status != 206 && status != 416)
		return fail(http_failure(status));
	if (!sha256_matches(UPDATE_DIR "/payload.part", sha, size)) {
		/* A corrupt or tampered part is never kept for resume. */
		unlink(UPDATE_DIR "/payload.part");
		return fail(cancelled ? "cancelled" : "checksum");
	}
	if (rename(UPDATE_DIR "/payload.part", UPDATE_DIR "/payload") != 0)
		return fail("storage");
	unlink(PART_CLEAN);
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
	static char notes[NOTES_MAX + 1];
	char cur[64], build[128], channel[32], version[64] = "";
	bool available = manifest_verified() && manifest_offers_update();

	current_version(cur, sizeof(cur), build, sizeof(build));
	current_channel(channel, sizeof(channel));
	if (available) {
		file_value(UPDATE_DIR "/manifest", "version", version, sizeof(version));
		file_value(UPDATE_DIR "/manifest", "notes", notes, sizeof(notes));
	}
	printf("provider=%s\nchannel=%s\nauto=%d\ncurrent=%s\nbuild=%s\navailable=%d\nversion=%s\nnotes=%s\ndownloaded=%d\nend=1\n",
	       public_key(pk) ? "available" : "unavailable", channel, access(AUTO_FILE, F_OK) != 0, cur, build, available,
	       version, notes, available && access(UPDATE_DIR "/payload", F_OK) == 0);
	return 0;
}

/* A new channel forgets what the old one offered. */
static int cmd_channel(const char *name)
{
	char current[32];
	FILE *fp;

	current_channel(current, sizeof(current));
	if (!name) {
		puts(current);
		return 0;
	}
	if (!valid_channel(name))
		return fail("channel");
	if (!strcmp(name, current)) {
		puts("OK");
		return 0;
	}
	fp = fopen(CHANNEL_FILE ".tmp", "w");
	if (!fp)
		return fail("storage");
	if (fprintf(fp, "%s\n", name) < 0 || fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
		fclose(fp);
		unlink(CHANNEL_FILE ".tmp");
		return fail("storage");
	}
	fclose(fp);
	if (rename(CHANNEL_FILE ".tmp", CHANNEL_FILE) != 0)
		return fail("storage");
	unlink(UPDATE_DIR "/manifest");
	unlink(UPDATE_DIR "/manifest.sig");
	unlink(UPDATE_DIR "/payload");
	unlink(UPDATE_DIR "/payload.part");
	unlink(PART_CLEAN);
	puts("OK");
	return 0;
}

/* The daily check is on unless AUTO_FILE exists. */
static int cmd_auto(const char *value)
{
	if (!value) {
		puts(access(AUTO_FILE, F_OK) != 0 ? "on" : "off");
		return 0;
	}
	if (!strcmp(value, "on")) {
		if (unlink(AUTO_FILE) != 0 && errno != ENOENT)
			return fail("storage");
	} else if (!strcmp(value, "off")) {
		int fd = open(AUTO_FILE, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

		if (fd < 0)
			return fail("storage");
		close(fd);
	} else {
		return fail("value");
	}
	puts("OK");
	return 0;
}

static void progress(int percent)
{
	static int last = -1;

	if (as_job && percent != last) {
		last = percent;
		printf("@progress %d\n", percent);
		fflush(stdout);
	}
}

/* Hash the first size bytes of fd: the image as it landed on the card. */
static bool read_back(int fd, long long size, const char *expected_hex)
{
	crypto_hash_sha256_state st;
	unsigned char digest[crypto_hash_sha256_BYTES];
	char hex[2 * crypto_hash_sha256_BYTES + 1];
	static unsigned char buf[1 << 20];
	long long done = 0;

	crypto_hash_sha256_init(&st);
	while (done < size && !cancelled) {
		size_t want = size - done < (long long)sizeof(buf) ? (size_t)(size - done) : sizeof(buf);
		ssize_t n = read(fd, buf, want);

		if (n <= 0)
			return false;
		crypto_hash_sha256_update(&st, buf, (size_t)n);
		done += n;
		progress(80 + (int)(done * 20 / size));
	}
	if (cancelled)
		return false;
	crypto_hash_sha256_final(&st, digest);
	sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
	return !strcasecmp(hex, expected_hex);
}

static int cmd_write(const char *device)
{
	char sha[80], size_text[32], image_sha[80], image_text[32];
	crypto_hash_sha256_state st;
	unsigned char digest[crypto_hash_sha256_BYTES];
	char hex[2 * crypto_hash_sha256_BYTES + 1];
	static unsigned char buf[1 << 20];
	long long image_size, done = 0;
	struct stat dst;
	gzFile gz;
	int fd, n;

	if (!manifest_verified())
		return fail("signature");
	if (!file_value(UPDATE_DIR "/manifest", "sha256", sha, sizeof(sha)) ||
	    !file_value(UPDATE_DIR "/manifest", "size", size_text, sizeof(size_text)) ||
	    !file_value(UPDATE_DIR "/manifest", "image_sha256", image_sha, sizeof(image_sha)) ||
	    !file_value(UPDATE_DIR "/manifest", "image_size", image_text, sizeof(image_text)))
		return fail("manifest");
	image_size = atoll(image_text);
	if (image_size <= 0)
		return fail("manifest");
	progress(-1);
	if (!sha256_matches(UPDATE_DIR "/payload", sha, atoll(size_text)))
		return fail(cancelled ? "cancelled" : "checksum");
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &dst) != 0)
		return fail("slot");
	if (S_ISBLK(dst.st_mode)) {
		unsigned long long bytes = 0;

		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0 || (long long)bytes < image_size) {
			close(fd);
			return fail("slot");
		}
	}
	gz = gzopen(UPDATE_DIR "/payload", "rb");
	if (!gz) {
		close(fd);
		return fail("storage");
	}
	gzbuffer(gz, 1 << 17);
	crypto_hash_sha256_init(&st);
	progress(0);
	while (!cancelled && (n = gzread(gz, buf, sizeof(buf))) > 0) {
		if (done + n > image_size)
			break;
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(fd, buf + off, (size_t)(n - off));

			if (w <= 0) {
				gzclose(gz);
				close(fd);
				return fail("write");
			}
			off += w;
		}
		crypto_hash_sha256_update(&st, buf, (size_t)n);
		done += n;
		progress((int)(done * 80 / image_size));
	}
	gzclose(gz);
	crypto_hash_sha256_final(&st, digest);
	sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
	if (cancelled || done != image_size || strcasecmp(hex, image_sha)) {
		close(fd);
		return fail(cancelled ? "cancelled" : "checksum");
	}
	/* Read back from the card, not from the page cache. */
	if (fsync(fd) != 0) {
		close(fd);
		return fail("write");
	}
	if (S_ISBLK(dst.st_mode))
		ioctl(fd, BLKFLSBUF, 0);
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	if (lseek(fd, 0, SEEK_SET) != 0 || !read_back(fd, image_size, image_sha)) {
		close(fd);
		return fail(cancelled ? "cancelled" : "verify");
	}
	close(fd);
	printf(as_job ? "@result written\n" : "OK written\n");
	return 0;
}

/*
 * check, download and write change UPDATE_DIR: one at a time. Two downloads
 * appending to the same payload.part (2026-10-25: a manual one next to the
 * Settings job) made a part longer than the payload, which failed its
 * checksum at 100 %; a check during a download replaces the manifest the
 * download verifies against. The lock goes with the process.
 */
static bool take_lock(void)
{
	int fd = open(UPDATE_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

	return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
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
	if (argc >= 2 && (!strcmp(argv[1], "check") || !strcmp(argv[1], "download") || !strcmp(argv[1], "write")) &&
	    !take_lock())
		return fail("busy");
	curl_global_init(CURL_GLOBAL_DEFAULT);
	if (argc == 2 && !strcmp(argv[1], "status"))
		rc = cmd_status();
	else if (argc == 2 && !strcmp(argv[1], "check"))
		rc = cmd_check();
	else if (argc == 2 && !strcmp(argv[1], "download"))
		rc = cmd_download();
	else if (argc == 2 && !strcmp(argv[1], "verify"))
		rc = cmd_verify();
	else if ((argc == 2 || argc == 3) && !strcmp(argv[1], "channel"))
		rc = cmd_channel(argc == 3 ? argv[2] : NULL);
	else if ((argc == 2 || argc == 3) && !strcmp(argv[1], "auto"))
		rc = cmd_auto(argc == 3 ? argv[2] : NULL);
	else if (argc == 3 && !strcmp(argv[1], "write"))
		rc = cmd_write(argv[2]);
	else
		fprintf(stderr, "Usage: nuubos-updatectl status | channel [stable|rc|nightly] | auto [on|off] | check | download | verify | write DEVICE\n");
	curl_global_cleanup();
	return rc;
}
