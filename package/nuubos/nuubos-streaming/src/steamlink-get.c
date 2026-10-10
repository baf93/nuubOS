/* SPDX-License-Identifier: MIT */
/*
 * nuubos-steamlink-get — downloads the Valve Steam Link application
 * (EPIC-026) for nuubos-streamd.
 *
 * Steam Link is proprietary and is not part of the nuubOS image: the user
 * asks for it, and this helper fetches Valve's current arm64 build from
 * Valve's public server, exactly what Valve's own launcher does on
 * Raspberry Pi OS.
 *
 *   nuubos-steamlink-get check
 *       @latest <version>
 *   nuubos-steamlink-get install <root>
 *       @progress <percent>... then @done <version>
 *   nuubos-steamlink-get rollback <root>
 *       @done <version>   (swaps app/ and previous/: rolling back twice
 *                          returns to the newer build)
 *   nuubos-steamlink-get remove <root>
 *       @done
 *
 * Failures print "@error network|space|archive|signature|write" and exit
 * non-zero.
 *
 * The archive is extracted while it downloads (gzip + tar, no temporary
 * tarball) into <root>/.staging, then swapped in as <root>/app; the build
 * it replaces stays as <root>/previous for a rollback. USERDATA is
 * exFAT: symbolic and hard links become copies of their targets. SIGTERM
 * stops the download and removes the partial tree.
 *
 * Valve signs every archive (OpenPGP detached signature "<archive>.sig",
 * checked by Valve's own launcher with gpg). The archive bytes are hashed
 * while they download and the signature is verified with Valve's release
 * key before the new tree replaces the installed one.
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
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <zlib.h>

#define BUILD_URL "https://media.steampowered.com/steamlink/rpi/trixie/arm64/public_build.txt"
#define ARCHIVE_PREFIX "https://media.steampowered.com/steamlink/rpi/"
#define ARCHIVE_ROOT "steamlink"
/* ~33 MB download, ~110 MB once links are copies; keep a margin. */
#define MIN_FREE_BYTES (256ULL * 1024 * 1024)
#define MAX_LINKS 64

/*
 * Valve Steam Link Release Key <steamlink@steampowered.com>, RSA 2048,
 * fingerprint BA23 DAE6 4102 FBE0 BB14 0CE5 387C 648A 24C0 E740: the public
 * key Valve's Raspberry Pi launcher (/usr/bin/steamlink, package steamlink
 * 1.0.16) imports to verify these archives. Modulus and exponent of its
 * OpenPGP public key packet.
 */
static const unsigned char VALVE_KEY_N[] = {
	0xb8, 0x8d, 0x88, 0x7f, 0xc3, 0x22, 0xd6, 0x16, 0x01, 0x3c, 0xc9, 0x59,
	0xfb, 0x85, 0x00, 0xda, 0xe3, 0x23, 0xff, 0x38, 0x83, 0x2c, 0x0f, 0x60,
	0xb8, 0xf4, 0xe0, 0x10, 0x21, 0xea, 0x2f, 0x64, 0x86, 0xd5, 0x74, 0xb4,
	0xb8, 0x66, 0x55, 0x20, 0x9d, 0x01, 0x9b, 0x2d, 0xa7, 0xba, 0x6a, 0x60,
	0xd5, 0x59, 0xa2, 0x53, 0x07, 0x3a, 0x0a, 0x2e, 0xe2, 0x0a, 0xde, 0xa7,
	0xa9, 0xca, 0x87, 0x48, 0xcd, 0x18, 0x90, 0x41, 0xd0, 0x49, 0x67, 0x98,
	0x32, 0x57, 0x56, 0xf7, 0x3e, 0x8e, 0xef, 0xf3, 0xe5, 0x0f, 0x3e, 0x6b,
	0x13, 0x83, 0x10, 0x5d, 0x1e, 0x66, 0x95, 0x57, 0x8a, 0x5b, 0xc5, 0x22,
	0xa6, 0x85, 0x1c, 0x1d, 0x49, 0x28, 0x20, 0x76, 0xf4, 0x51, 0x50, 0x4c,
	0xfa, 0x0d, 0xb6, 0x2b, 0xd6, 0xbe, 0x59, 0x4f, 0x95, 0xb9, 0x01, 0x80,
	0x19, 0x07, 0x7d, 0x2a, 0x23, 0x6b, 0x3c, 0x36, 0xef, 0x46, 0x2f, 0x99,
	0x6a, 0xad, 0x9b, 0x6d, 0x3c, 0x9e, 0x34, 0x88, 0x11, 0x2d, 0x3e, 0x77,
	0x9c, 0xda, 0x01, 0xc3, 0x69, 0x85, 0x9e, 0x38, 0xd1, 0xfd, 0xd3, 0x47,
	0xef, 0x50, 0xbb, 0x64, 0xa3, 0xa4, 0x62, 0x86, 0x50, 0x01, 0x3b, 0x27,
	0xdd, 0x4a, 0xb9, 0x9a, 0xa5, 0x27, 0x57, 0x91, 0x52, 0x9a, 0xd2, 0x79,
	0xe2, 0x97, 0xb8, 0xfd, 0x40, 0x18, 0x74, 0x54, 0x34, 0xe9, 0xf6, 0x63,
	0x57, 0xa9, 0xb5, 0x92, 0xf8, 0x72, 0xc2, 0x70, 0x89, 0x94, 0xc0, 0x87,
	0x91, 0xc9, 0x59, 0x85, 0x9c, 0xb6, 0x2d, 0x89, 0x97, 0x26, 0xfc, 0x21,
	0xff, 0xde, 0x6c, 0xda, 0x41, 0xa0, 0x32, 0xd1, 0x68, 0xce, 0x76, 0x35,
	0x43, 0x1c, 0x2c, 0x45, 0xf5, 0x4a, 0x21, 0x58, 0x37, 0x77, 0x51, 0xec,
	0x54, 0x77, 0x7e, 0xb4, 0x84, 0x6b, 0xe1, 0x54, 0xc9, 0x0b, 0xbc, 0xc7,
	0xe4, 0x45, 0xf3, 0x2d,
};
static const unsigned char VALVE_KEY_E[] = { 0x01, 0x00, 0x01 };

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
/* Latest build                                                        */
/* ------------------------------------------------------------------ */

struct text {
	char data[1024];
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

/* public_build.txt holds the archive URL; the version is in its name
 * (".../steamlink-rpi-trixie-arm64-1.3.32.316.tar.gz"). */
static bool latest_build(char *url, size_t url_size, char *version, size_t version_size)
{
	struct text t = { .used = 0 };
	CURL *curl = new_request(BUILD_URL);
	const char *name;
	const char *dash;
	size_t len;
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
	len = strcspn(t.data, "\r\n \t");
	t.data[len] = '\0';
	if (len >= url_size || strncmp(t.data, ARCHIVE_PREFIX, strlen(ARCHIVE_PREFIX)) != 0 ||
	    len < 8 || strcmp(t.data + len - 7, ".tar.gz") != 0 || strstr(t.data, "/arm64/") == NULL)
		return false;
	snprintf(url, url_size, "%s", t.data);
	name = strrchr(t.data, '/') + 1;
	t.data[len - 7] = '\0';
	dash = strrchr(name, '-');
	if (!dash || !dash[1] || strlen(dash + 1) >= version_size)
		return false;
	for (const char *p = dash + 1; *p; p++)
		if (!isdigit((unsigned char)*p) && *p != '.')
			return false;
	snprintf(version, version_size, "%s", dash + 1);
	return true;
}

/* ------------------------------------------------------------------ */
/* Valve's OpenPGP signature                                           */
/* ------------------------------------------------------------------ */

struct signature {
	unsigned char hashed[512]; /* version .. hashed subpackets */
	size_t hashed_len;
	int hash_algo;             /* OpenPGP: 8 SHA-256, 10 SHA-512 */
	unsigned char value[256];  /* RSA signature, left-padded to the modulus */
};

/* A v4 binary-document RSA signature packet (old or new packet format). */
static bool parse_signature(const unsigned char *data, size_t len, struct signature *sig)
{
	const unsigned char *p;
	size_t body, pos, hlen, ulen, bits, bytes;

	if (len < 2 || !(data[0] & 0x80))
		return false;
	if (data[0] & 0x40) {
		if ((data[0] & 0x3f) != 2)
			return false;
		if (data[1] < 192) {
			body = data[1];
			p = data + 2;
		} else if (data[1] < 224 && len >= 3) {
			body = ((size_t)(data[1] - 192) << 8) + data[2] + 192;
			p = data + 3;
		} else if (data[1] == 255 && len >= 6) {
			body = ((size_t)data[2] << 24) | ((size_t)data[3] << 16) | ((size_t)data[4] << 8) | data[5];
			p = data + 6;
		} else {
			return false;
		}
	} else {
		static const size_t sizes[] = { 1, 2, 4 };
		size_t n;

		if (((data[0] >> 2) & 0x0f) != 2 || (data[0] & 3) == 3)
			return false;
		n = sizes[data[0] & 3];
		if (len < 1 + n)
			return false;
		body = 0;
		for (size_t i = 0; i < n; i++)
			body = (body << 8) | data[1 + i];
		p = data + 1 + n;
	}
	if ((size_t)(p - data) + body > len || body < 10)
		return false;
	/* version 4, signature type 0x00 (binary document), RSA */
	if (p[0] != 4 || p[1] != 0x00 || p[2] != 1 || (p[3] != 8 && p[3] != 10))
		return false;
	sig->hash_algo = p[3];
	hlen = ((size_t)p[4] << 8) | p[5];
	pos = 6 + hlen;
	if (pos + 2 > body || pos > sizeof(sig->hashed))
		return false;
	memcpy(sig->hashed, p, pos);
	sig->hashed_len = pos;
	ulen = ((size_t)p[pos] << 8) | p[pos + 1];
	pos += 2 + ulen + 2; /* unhashed subpackets, left 16 bits of the hash */
	if (pos + 2 > body)
		return false;
	bits = ((size_t)p[pos] << 8) | p[pos + 1];
	bytes = (bits + 7) / 8;
	pos += 2;
	if (bytes == 0 || bytes > sizeof(sig->value) || pos + bytes > body)
		return false;
	memset(sig->value, 0, sizeof(sig->value));
	memcpy(sig->value + sizeof(sig->value) - bytes, p + pos, bytes);
	return true;
}

static EVP_PKEY *valve_key(void)
{
	OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
	BIGNUM *n = BN_bin2bn(VALVE_KEY_N, sizeof(VALVE_KEY_N), NULL);
	BIGNUM *e = BN_bin2bn(VALVE_KEY_E, sizeof(VALVE_KEY_E), NULL);
	OSSL_PARAM *params = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *key = NULL;

	if (bld && n && e && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) &&
	    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e) &&
	    (params = OSSL_PARAM_BLD_to_param(bld)) != NULL &&
	    (ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL)) != NULL &&
	    EVP_PKEY_fromdata_init(ctx) == 1)
		(void)EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params);
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(bld);
	BN_free(n);
	BN_free(e);
	return key;
}

/* Hash state for the archive; the signed trailer is added at the end. */
static EVP_MD_CTX *verify_begin(const struct signature *sig, EVP_PKEY *key)
{
	EVP_MD_CTX *md = EVP_MD_CTX_new();

	if (md && EVP_DigestVerifyInit(md, NULL, sig->hash_algo == 10 ? EVP_sha512() : EVP_sha256(),
				       NULL, key) == 1)
		return md;
	EVP_MD_CTX_free(md);
	return NULL;
}

static bool verify_end(EVP_MD_CTX *md, const struct signature *sig)
{
	unsigned char trailer[6] = { 4, 0xff };

	trailer[2] = (unsigned char)(sig->hashed_len >> 24);
	trailer[3] = (unsigned char)(sig->hashed_len >> 16);
	trailer[4] = (unsigned char)(sig->hashed_len >> 8);
	trailer[5] = (unsigned char)sig->hashed_len;
	return EVP_DigestVerifyUpdate(md, sig->hashed, sig->hashed_len) == 1 &&
	       EVP_DigestVerifyUpdate(md, trailer, sizeof(trailer)) == 1 &&
	       EVP_DigestVerifyFinal(md, sig->value, sizeof(sig->value)) == 1;
}

/* ------------------------------------------------------------------ */
/* Streaming tar extraction                                            */
/* ------------------------------------------------------------------ */

struct link {
	char path[PATH_MAX];   /* relative to the staging tree */
	char target[PATH_MAX]; /* symlink: relative to the link; hard link: to the tree */
	bool hard;
};

struct extract {
	const char *dir;     /* staging tree */
	z_stream z;
	bool z_ready;
	unsigned char header[512];
	size_t header_used;
	unsigned long long remaining;  /* data bytes left in the current entry */
	unsigned long long padding;    /* bytes to skip after the data */
	int fd;                        /* regular file being written, or -1 */
	char long_name[PATH_MAX];      /* GNU 'L' / pax path for the next entry */
	char long_link[PATH_MAX];      /* GNU 'K' / pax linkpath */
	char meta[8192];               /* 'L', 'K' and 'x' payloads */
	size_t meta_used;
	char meta_type;
	bool finished;
	const char *error;
	struct link links[MAX_LINKS];
	int link_count;
	unsigned long long total;      /* Content-Length, 0 if unknown */
	unsigned long long received;
	int percent;
	EVP_MD_CTX *verify;            /* archive hash for Valve's signature */
};

static unsigned long long octal(const unsigned char *p, size_t len)
{
	unsigned long long v = 0;

	/* GNU base-256 for sizes >= 8 GB: never in this archive. */
	if (p[0] & 0x80)
		return ~0ULL;
	for (size_t i = 0; i < len && p[i]; i++) {
		if (p[i] == ' ')
			continue;
		if (p[i] < '0' || p[i] > '7')
			break;
		v = v * 8 + (unsigned long long)(p[i] - '0');
	}
	return v;
}

/* Archive member → path inside the tree, or NULL if it must be refused:
 * everything sits under "steamlink/", nothing may escape the tree. */
static const char *member_path(const char *name)
{
	size_t root = strlen(ARCHIVE_ROOT);
	const char *rel;

	while (name[0] == '.' && name[1] == '/')
		name += 2;
	if (strncmp(name, ARCHIVE_ROOT, root) != 0 || (name[root] != '/' && name[root] != '\0'))
		return NULL;
	rel = name + root;
	while (*rel == '/')
		rel++;
	for (const char *p = rel; *p; ) {
		const char *end = strchr(p, '/');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		if (len == 2 && p[0] == '.' && p[1] == '.')
			return NULL;
		p += len;
		while (*p == '/')
			p++;
	}
	return rel;
}

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

/* pax extended header: "<len> key=value\n" records. */
static void parse_pax(struct extract *x)
{
	size_t pos = 0;

	while (pos < x->meta_used) {
		char *rec = x->meta + pos;
		char *space;
		char *eq;
		unsigned long len = strtoul(rec, &space, 10);

		if (len == 0 || *space != ' ' || pos + len > x->meta_used)
			break;
		rec[len - 1] = '\0';
		eq = strchr(space + 1, '=');
		if (eq) {
			*eq = '\0';
			if (!strcmp(space + 1, "path"))
				snprintf(x->long_name, sizeof(x->long_name), "%s", eq + 1);
			else if (!strcmp(space + 1, "linkpath"))
				snprintf(x->long_link, sizeof(x->long_link), "%s", eq + 1);
		}
		pos += len;
	}
}

static void start_entry(struct extract *x)
{
	const unsigned char *h = x->header;
	char name[PATH_MAX];
	char link[PATH_MAX];
	char path[PATH_MAX];
	const char *rel;
	char type = (char)h[156];
	unsigned long long size = octal(h + 124, 12);

	if (size == ~0ULL) {
		x->error = "archive";
		return;
	}
	x->remaining = size;
	x->padding = (512 - size % 512) % 512;
	x->fd = -1;

	if (type == 'L' || type == 'K' || type == 'x') {
		if (size >= sizeof(x->meta)) {
			x->error = "archive";
			return;
		}
		x->meta_type = type;
		x->meta_used = 0;
		return;
	}
	x->meta_type = 0;

	if (x->long_name[0]) {
		snprintf(name, sizeof(name), "%s", x->long_name);
	} else if (!memcmp(h + 257, "ustar", 5) && h[345]) {
		snprintf(name, sizeof(name), "%.155s/%.100s", (const char *)h + 345, (const char *)h);
	} else {
		snprintf(name, sizeof(name), "%.100s", (const char *)h);
	}
	if (x->long_link[0])
		snprintf(link, sizeof(link), "%s", x->long_link);
	else
		snprintf(link, sizeof(link), "%.100s", (const char *)h + 157);
	x->long_name[0] = '\0';
	x->long_link[0] = '\0';

	if (type == 'g')
		return;
	rel = member_path(name);
	if (!rel) {
		x->error = "archive";
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", x->dir, rel);

	switch (type) {
	case '0':
	case '\0':
	case '7':
		if (make_parent(path) != 0) {
			x->error = "write";
			return;
		}
		x->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
		if (x->fd < 0)
			x->error = "write";
		break;
	case '5':
		if (rel[0] && mkdir_p(path) != 0)
			x->error = "write";
		break;
	case '1':
	case '2': {
		const char *target = link;

		if (x->link_count >= MAX_LINKS) {
			x->error = "archive";
			return;
		}
		/* A hard link names another member of the archive. */
		if (type == '1' && !(target = member_path(link))) {
			x->error = "archive";
			return;
		}
		if (type == '2' && (target[0] == '/' || strstr(target, "..") != NULL)) {
			x->error = "archive";
			return;
		}
		snprintf(x->links[x->link_count].path, sizeof(x->links[0].path), "%s", rel);
		snprintf(x->links[x->link_count].target, sizeof(x->links[0].target), "%s", target);
		x->links[x->link_count].hard = type == '1';
		x->link_count++;
		break;
	}
	default:
		/* Devices, FIFOs...: not part of an application archive. */
		break;
	}
}

static void end_entry(struct extract *x)
{
	if (x->fd >= 0) {
		if (close(x->fd) != 0 && !x->error)
			x->error = "write";
		x->fd = -1;
	}
	if (x->meta_type == 'L' || x->meta_type == 'K') {
		char *dst = x->meta_type == 'L' ? x->long_name : x->long_link;

		x->meta[x->meta_used < sizeof(x->meta) ? x->meta_used : sizeof(x->meta) - 1] = '\0';
		snprintf(dst, PATH_MAX, "%s", x->meta);
	} else if (x->meta_type == 'x') {
		parse_pax(x);
	}
	x->meta_type = 0;
}

/* Feeds decompressed tar bytes. */
static void tar_feed(struct extract *x, const unsigned char *data, size_t len)
{
	while (len > 0 && !x->error && !x->finished) {
		if (x->remaining > 0) {
			size_t n = len < x->remaining ? len : (size_t)x->remaining;

			if (x->fd >= 0) {
				const unsigned char *p = data;
				size_t left = n;

				while (left > 0) {
					ssize_t w = write(x->fd, p, left);

					if (w < 0 && errno == EINTR)
						continue;
					if (w <= 0) {
						x->error = errno == ENOSPC ? "space" : "write";
						return;
					}
					p += w;
					left -= (size_t)w;
				}
			} else if (x->meta_type) {
				memcpy(x->meta + x->meta_used, data, n);
				x->meta_used += n;
			}
			x->remaining -= n;
			data += n;
			len -= n;
			if (x->remaining == 0 && x->padding == 0)
				end_entry(x);
			continue;
		}
		if (x->padding > 0) {
			size_t n = len < x->padding ? len : (size_t)x->padding;

			x->padding -= n;
			data += n;
			len -= n;
			if (x->padding == 0)
				end_entry(x);
			continue;
		}
		{
			size_t n = 512 - x->header_used;

			if (n > len)
				n = len;
			memcpy(x->header + x->header_used, data, n);
			x->header_used += n;
			data += n;
			len -= n;
		}
		if (x->header_used < 512)
			continue;
		x->header_used = 0;
		/* Two zero blocks end the archive; one is enough to stop. */
		{
			bool zero = true;

			for (int i = 0; i < 512 && zero; i++)
				zero = x->header[i] == 0;
			if (zero) {
				x->finished = true;
				return;
			}
		}
		start_entry(x);
		if (!x->error && x->remaining == 0 && x->padding == 0)
			end_entry(x);
	}
}

static size_t download_write(char *ptr, size_t size, size_t nmemb, void *user)
{
	struct extract *x = user;
	size_t len = size * nmemb;
	unsigned char out[65536];

	if (stop_requested || x->error)
		return 0;
	if (EVP_DigestVerifyUpdate(x->verify, ptr, len) != 1) {
		x->error = "signature";
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

/* ------------------------------------------------------------------ */
/* Tree helpers                                                        */
/* ------------------------------------------------------------------ */

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
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
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

/* Tree-relative path a link resolves to (following chains of links). */
static bool resolve_link(const struct extract *x, int index, char *out, size_t size)
{
	char current[PATH_MAX];
	int i = index;

	for (int depth = 0; depth < 8; depth++) {
		const struct link *l = &x->links[i];
		int next = -1;

		if (l->hard) {
			snprintf(current, sizeof(current), "%s", l->target);
		} else {
			const char *slash = strrchr(l->path, '/');

			if (slash)
				snprintf(current, sizeof(current), "%.*s/%s", (int)(slash - l->path), l->path, l->target);
			else
				snprintf(current, sizeof(current), "%s", l->target);
		}
		for (int k = 0; k < x->link_count; k++)
			if (!strcmp(x->links[k].path, current))
				next = k;
		if (next < 0) {
			snprintf(out, size, "%s", current);
			return true;
		}
		i = next;
	}
	return false;
}

static const char *materialize_links(const struct extract *x)
{
	for (int i = 0; i < x->link_count; i++) {
		char target[PATH_MAX];
		char from[PATH_MAX];
		char to[PATH_MAX];
		struct stat st;

		if (!resolve_link(x, i, target, sizeof(target)))
			return "archive";
		snprintf(from, sizeof(from), "%s/%s", x->dir, target);
		snprintf(to, sizeof(to), "%s/%s", x->dir, x->links[i].path);
		if (stat(from, &st) != 0 || !S_ISREG(st.st_mode))
			return "archive";
		if (make_parent(to) != 0 || copy_file(from, to) != 0)
			return errno == ENOSPC ? "space" : "write";
	}
	return NULL;
}

/* The tree is a Steam Link build for this machine. */
static bool valid_tree(const char *dir)
{
	unsigned char elf[20];
	char path[PATH_MAX];
	int fd;
	bool ok;

	snprintf(path, sizeof(path), "%s/version.txt", dir);
	if (access(path, R_OK) != 0)
		return false;
	snprintf(path, sizeof(path), "%s/bin/shell", dir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	ok = read(fd, elf, sizeof(elf)) == (ssize_t)sizeof(elf) && !memcmp(elf, "\x7f" "ELF", 4) &&
	     elf[4] == 2 /* 64-bit */ && elf[18] == 183 /* EM_AARCH64 */;
	close(fd);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int install(const char *root)
{
	char url[512];
	char sig_url[520];
	char version[32];
	char staging[PATH_MAX];
	char app[PATH_MAX];
	char old[PATH_MAX];
	struct statvfs vfs;
	struct signature sig;
	struct text sig_data = { .used = 0 };
	struct extract *x;
	EVP_PKEY *key;
	CURL *curl;
	CURLcode rc;
	const char *error = NULL;

	if (!latest_build(url, sizeof(url), version, sizeof(version)))
		return fail("network");
	snprintf(sig_url, sizeof(sig_url), "%s.sig", url);
	curl = new_request(sig_url);
	if (!curl)
		return fail("network");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, text_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sig_data);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	rc = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK)
		return fail("network");
	if (!parse_signature((const unsigned char *)sig_data.data, sig_data.used, &sig))
		return fail("signature");
	key = valve_key();
	if (!key)
		return fail("signature");
	if (mkdir_p(root) != 0)
		return fail("write");
	if (statvfs(root, &vfs) == 0 && (unsigned long long)vfs.f_bavail * vfs.f_frsize < MIN_FREE_BYTES)
		return fail("space");

	snprintf(staging, sizeof(staging), "%s/.staging", root);
	snprintf(app, sizeof(app), "%s/app", root);
	snprintf(old, sizeof(old), "%s/.old", root);
	remove_tree(staging);
	remove_tree(old);
	if (mkdir_p(staging) != 0)
		return fail("write");

	x = calloc(1, sizeof(*x));
	curl = new_request(url);
	if (!x || !curl || inflateInit2(&x->z, 16 + MAX_WBITS) != Z_OK ||
	    (x->verify = verify_begin(&sig, key)) == NULL) {
		if (x)
			EVP_MD_CTX_free(x->verify);
		free(x);
		if (curl)
			curl_easy_cleanup(curl);
		EVP_PKEY_free(key);
		remove_tree(staging);
		return fail("write");
	}
	x->z_ready = true;
	x->dir = staging;
	x->fd = -1;
	x->percent = -1;
	event("@progress %s", "0");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, x);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_line);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, x);
	rc = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	if (x->fd >= 0)
		close(x->fd);
	inflateEnd(&x->z);

	if (stop_requested)
		error = "cancelled";
	else if (x->error)
		error = x->error;
	else if (rc != CURLE_OK)
		error = "network";
	else if (!x->finished)
		error = "archive";
	/* Nothing from the archive is used unless Valve signed it. */
	if (!error && !verify_end(x->verify, &sig))
		error = "signature";
	EVP_MD_CTX_free(x->verify);
	EVP_PKEY_free(key);
	if (!error)
		error = materialize_links(x);
	if (!error && !valid_tree(staging))
		error = "archive";
	free(x);
	if (error) {
		remove_tree(staging);
		return fail(error);
	}

	/* Swap: the installed build becomes <root>/previous (rollback); the
	 * build before it goes away once the new one is in place. */
	if (access(app, F_OK) == 0 && rename(app, old) != 0) {
		remove_tree(staging);
		return fail("write");
	}
	if (rename(staging, app) != 0) {
		rename(old, app);
		remove_tree(staging);
		return fail("write");
	}
	{
		char previous[PATH_MAX];

		snprintf(previous, sizeof(previous), "%s/previous", root);
		if (access(old, F_OK) == 0) {
			remove_tree(previous);
			if (rename(old, previous) != 0)
				remove_tree(old);
		}
	}
	sync();
	event("@done %s", version);
	return 0;
}

/* Back to the build before the last update (and forth again). */
static int rollback(const char *root)
{
	char app[PATH_MAX], previous[PATH_MAX], swap[PATH_MAX], path[PATH_MAX];
	char version[32] = "";
	FILE *fp;

	snprintf(app, sizeof(app), "%s/app", root);
	snprintf(previous, sizeof(previous), "%s/previous", root);
	snprintf(swap, sizeof(swap), "%s/.swap", root);
	if (!valid_tree(previous))
		return fail("archive");
	remove_tree(swap);
	if (access(app, F_OK) == 0 && rename(app, swap) != 0)
		return fail("write");
	if (rename(previous, app) != 0) {
		rename(swap, app);
		return fail("write");
	}
	if (access(swap, F_OK) == 0 && rename(swap, previous) != 0)
		remove_tree(swap);
	sync();
	snprintf(path, sizeof(path), "%s/version.txt", app);
	fp = fopen(path, "r");
	if (fp) {
		if (!fgets(version, sizeof(version), fp))
			version[0] = '\0';
		fclose(fp);
	}
	version[strcspn(version, "\r\n")] = '\0';
	event("@done %s", version);
	return 0;
}

static int remove_app(const char *root)
{
	static const char *const dirs[] = { ".staging", ".old", ".swap", "previous", "app" };
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
		char url[512];
		char version[32];

		curl_global_init(CURL_GLOBAL_DEFAULT);
		rc = latest_build(url, sizeof(url), version, sizeof(version)) ? 0 : 1;
		if (rc == 0)
			event("@latest %s", version);
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
	fprintf(stderr, "usage: nuubos-steamlink-get check | install <root> | rollback <root> | remove <root>\n");
	return 2;
}
