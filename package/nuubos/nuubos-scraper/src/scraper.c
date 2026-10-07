/* SPDX-License-Identifier: MIT */
/*
 * nuubos-scraper — Game Metadata & ScreenScraper (EPIC-015).
 *
 *   game GAME [force]        scrape one game (force: also a matched game)
 *   bulk all|system:<id>     scrape games without metadata (a background job)
 *   accept GAME              approve a match waiting for review
 *   reject GAME              discard a match; bulk scraping skips the game
 *   clear GAME               remove scraped metadata and media of a game
 *   account status | account set NAME (password on stdin) | account clear
 *
 * Optional online enrichment: the Library never depends on it. Runs only
 * when the user asks (no automatic network use, EPIC-055). Provider data is
 * mapped into the provider-neutral store shared by every user:
 *   /userdata/library/metadata/<system>/<rom name>.txt   key=value lines
 *   /userdata/library/covers/<system>/<rom name>.png|jpg  (Library covers)
 *   /userdata/library/screenshots/<system>/<rom name>.png|jpg
 * Everything is derived data: removable and rebuildable, never the ROMs.
 *
 * Matching: CRC32 + size + file name. A game the provider identifies by
 * checksum is applied ("match=auto"); a name-only match is stored as
 * "match=review" without media until the user accepts it. A match the user
 * accepted ("match=user") is never replaced unless explicitly forced.
 *
 * ScreenScraper needs the nuubOS developer credentials, compiled in from
 * local/screenscraper-dev.conf at build time (not in the repository);
 * without them the provider reports "unavailable". The optional user
 * account (better quota) is device-wide in /state/scraper (root only).
 * Media URLs carry credentials: they are never stored.
 *
 * As a nuubos-jobd worker it reports @progress / @result / @error and stops
 * at the next game on SIGTERM, leaving no partial file.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <zlib.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef USERDATA_ROOT
#define USERDATA_ROOT "/userdata"
#endif
#ifndef STATE_ROOT
#define STATE_ROOT "/state"
#endif
#ifndef API_BASE
#define API_BASE "https://api.screenscraper.fr/api2"
#endif
#ifndef SS_DEVID
#define SS_DEVID ""
#endif
#ifndef SS_DEVPASSWORD
#define SS_DEVPASSWORD ""
#endif
#define SOFTNAME "nuubOS"

#define LIBRARY_SOCKET RUN_ROOT "/libraryd.sock"
#define ACTIVE_USER_FILE RUN_ROOT "/user/active"
#define ROMS_ROOT USERDATA_ROOT "/roms"
#define META_ROOT USERDATA_ROOT "/library/metadata"
#define COVERS_ROOT USERDATA_ROOT "/library/covers"
#define SHOTS_ROOT USERDATA_ROOT "/library/screenshots"
#define ACCOUNT_FILE STATE_ROOT "/scraper/screenscraper.conf"
/* Checksumming a large disc image costs more than the match gains. */
#define MAX_HASH_SIZE (64LL * 1024 * 1024)

static volatile sig_atomic_t cancelled;
static bool as_job;

struct game_info {
	char id[32];
	char system[32];
	char title[256];
	char rel[PATH_MAX];
	char stem[512];
	char match[16];
};

struct account {
	char user[64];
	char password[128];
};

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

static void on_term(int sig)
{
	(void)sig;
	cancelled = 1;
}

static void job_progress(int percent, const char *step)
{
	if (as_job) {
		printf("@progress %d %s\n", percent, step ? step : "");
		fflush(stdout);
	}
}

static void job_error(const char *reason)
{
	if (as_job)
		printf("@error %s\n", reason);
	else
		printf("ERR %s\n", reason);
	fflush(stdout);
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static void trim(char *s)
{
	size_t len = strlen(s);

	while (len && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
}

static bool valid_game(const char *id)
{
	if (strlen(id) != 16)
		return false;
	for (const char *p = id; *p; p++)
		if (!isxdigit((unsigned char)*p))
			return false;
	return true;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

/* One line per value: line breaks become the two characters "\n". */
static void one_line(const char *in, char *out, size_t size)
{
	size_t o = 0;

	for (const char *p = in ? in : ""; *p && o + 3 < size; p++) {
		if (*p == '\n') {
			out[o++] = '\\';
			out[o++] = 'n';
		} else if (*p == '\r' || *p == '\t') {
			out[o++] = ' ';
		} else {
			out[o++] = *p;
		}
	}
	out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Game Library (libraryd)                                             */
/* ------------------------------------------------------------------ */

static int library_request(const char *command, char *out, size_t size)
{
	struct sockaddr_un addr;
	struct timeval tv = { 5, 0 };
	size_t used = 0;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LIBRARY_SOCKET);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    write(fd, command, strlen(command)) != (ssize_t)strlen(command)) {
		close(fd);
		return -1;
	}
	while (used + 1 < size) {
		ssize_t n = read(fd, out + used, size - used - 1);

		if (n <= 0)
			break;
		used += (size_t)n;
		out[used] = '\0';
		if (strstr(out, "end=1\n") || (!strncmp(out, "ERR", 3) && strchr(out, '\n')) ||
		    (!strncmp(out, "OK", 2) && strchr(out, '\n')))
			break;
	}
	out[used] = '\0';
	close(fd);
	return used ? 0 : -1;
}

static void stem_of(const char *rel, char *out, size_t size)
{
	const char *base = strrchr(rel, '/');
	char *dot;

	copy_text(out, size, base ? base + 1 : rel);
	dot = strrchr(out, '.');
	if (dot && dot != out)
		*dot = '\0';
}

static bool game_details(const char *id, struct game_info *g)
{
	static char buf[65536];
	char cmd[64];
	char *line, *save = NULL;

	memset(g, 0, sizeof(*g));
	snprintf(cmd, sizeof(cmd), "DETAILS\t%s\n", id);
	if (library_request(cmd, buf, sizeof(buf)) != 0 || strncmp(buf, "game=", 5))
		return false;
	copy_text(g->id, sizeof(g->id), id);
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!strncmp(line, "game=", 5)) {
			char *f[3] = { NULL, NULL, NULL };
			char *p = line + 5;

			for (int i = 0; i < 3 && p; i++) {
				f[i] = p;
				p = strchr(p, '\t');
				if (p)
					*p++ = '\0';
			}
			if (f[1])
				copy_text(g->system, sizeof(g->system), f[1]);
			if (f[2])
				copy_text(g->title, sizeof(g->title), f[2]);
		} else if (!strncmp(line, "path=", 5)) {
			copy_text(g->rel, sizeof(g->rel), line + 5);
		} else if (!strncmp(line, "meta_match=", 11)) {
			copy_text(g->match, sizeof(g->match), line + 11);
		}
	}
	stem_of(g->rel, g->stem, sizeof(g->stem));
	return g->system[0] && g->rel[0];
}

/* ------------------------------------------------------------------ */
/* Provider mapping                                                    */
/* ------------------------------------------------------------------ */

/* nuubOS system id -> ScreenScraper systemeid. Systems without a verified
 * id are not scraped (their games keep local metadata). */
static int provider_system(const char *id)
{
	static const struct { const char *id; int ss; } map[] = {
		{ "megadrive", 1 }, { "mastersystem", 2 }, { "nes", 3 }, { "snes", 4 },
		{ "cps1", 6 }, { "cps2", 7 }, { "cps3", 8 }, { "gb", 9 }, { "gbc", 10 },
		{ "virtualboy", 11 }, { "gba", 12 }, { "n64", 14 }, { "nds", 15 },
		{ "sega32x", 19 }, { "segacd", 20 }, { "gamegear", 21 }, { "saturn", 22 },
		{ "dreamcast", 23 }, { "ngp", 82 }, { "atari2600", 26 }, { "lynx", 28 },
		{ "pcengine", 31 }, { "atari5200", 40 }, { "atari7800", 41 },
		{ "atarist", 42 }, { "atari800", 43 }, { "atarixe", 43 },
		{ "wonderswan", 45 }, { "coleco", 48 }, { "atomiswave", 53 },
		{ "naomi", 56 }, { "psx", 57 }, { "psp", 61 }, { "amiga", 64 },
		{ "amstradcpc", 65 }, { "c64", 66 }, { "neogeocd", 70 }, { "vic20", 73 },
		{ "arcade", 75 }, { "fbneo", 75 }, { "mame", 75 }, { "zxspectrum", 76 },
		{ "x68000", 79 }, { "channelf", 80 }, { "apple2", 86 }, { "megaduck", 90 },
		{ "vectrex", 102 }, { "odyssey2", 104 }, { "fds", 106 },
		{ "satellaview", 107 }, { "sufami", 108 }, { "sg1000", 109 },
		{ "msx", 113 }, { "pcecd", 114 }, { "intellivision", 115 },
		{ "scummvm", 123 }, { "dos", 135 }, { "neogeo", 142 },
		{ "geolith", 142 }, { "macintosh", 146 }, { "supervision", 207 },
		{ "pc98", 208 }, { "pokemini", 211 }, { "pc88", 221 }, { "tic80", 222 },
		{ "pico8", 234 },
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(map[i].id, id))
			return map[i].ss;
	return 0;
}

/* The active user's nuubOS language (metadata language preference). */
static void user_language(char *out, size_t size)
{
	char user[64] = "";
	char path[PATH_MAX];
	char line[128];
	FILE *fp;

	copy_text(out, size, "en");
	fp = fopen(ACTIVE_USER_FILE, "r");
	if (!fp)
		return;
	if (fgets(user, sizeof(user), fp))
		trim(user);
	fclose(fp);
	if (!user[0] || strchr(user, '/'))
		return;
	snprintf(path, sizeof(path), STATE_ROOT "/users/%s/localization.conf", user);
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (!strncmp(line, "LANGUAGE=", 9) && strlen(line + 9) == 2)
			copy_text(out, size, line + 9);
	}
	fclose(fp);
}

/* ------------------------------------------------------------------ */
/* Account                                                             */
/* ------------------------------------------------------------------ */

static void load_account(struct account *a)
{
	char line[256];
	FILE *fp = fopen(ACCOUNT_FILE, "r");

	memset(a, 0, sizeof(*a));
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		line[strcspn(line, "\r\n")] = '\0';
		if (!(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "USER"))
			copy_text(a->user, sizeof(a->user), eq);
		else if (!strcmp(line, "PASSWORD"))
			copy_text(a->password, sizeof(a->password), eq);
	}
	fclose(fp);
}

static int save_account(const struct account *a)
{
	char tmp[PATH_MAX];
	FILE *fp;
	int rc = 0;

	if (mkdir_p(STATE_ROOT "/scraper") != 0)
		return -1;
	chmod(STATE_ROOT "/scraper", 0700);
	snprintf(tmp, sizeof(tmp), "%s.tmp", ACCOUNT_FILE);
	umask(077);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "USER=%s\nPASSWORD=%s\n", a->user, a->password);
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0)
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, ACCOUNT_FILE) != 0)
		rc = -1;
	if (rc)
		unlink(tmp);
	return rc;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

struct buffer {
	char *data;
	size_t len;
	size_t cap;
	size_t limit;
};

static size_t to_buffer(char *ptr, size_t size, size_t n, void *userdata)
{
	struct buffer *b = userdata;
	size_t len = size * n;

	if (b->len + len + 1 > b->limit)
		return 0; /* aborts the transfer */
	if (b->len + len + 1 > b->cap) {
		size_t cap = (b->len + len + 1) * 2;
		char *p = realloc(b->data, cap);

		if (!p)
			return 0;
		b->data = p;
		b->cap = cap;
	}
	memcpy(b->data + b->len, ptr, len);
	b->len += len;
	b->data[b->len] = '\0';
	return len;
}

static int progress_cb(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)p; (void)a; (void)b; (void)c; (void)d;
	return cancelled ? 1 : 0;
}

/* GET url into b; returns the HTTP status, or -1 on transport failure. */
static long http_get(const char *url, struct buffer *b)
{
	CURL *curl = curl_easy_init();
	long status = -1;
	CURLcode res;

	if (!curl)
		return -1;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, SOFTNAME);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_buffer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, b);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	res = curl_easy_perform(curl);
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return status;
}

static void append_param(char *url, size_t size, CURL *esc, const char *key, const char *value)
{
	char *e = curl_easy_escape(esc, value, 0);
	size_t len = strlen(url);

	snprintf(url + len, size - len, "%s%s=%s", strchr(url, '?') ? "&" : "?", key, e ? e : "");
	curl_free(e);
}

static void credential_params(char *url, size_t size, CURL *esc, const struct account *a)
{
	append_param(url, size, esc, "devid", SS_DEVID);
	append_param(url, size, esc, "devpassword", SS_DEVPASSWORD);
	append_param(url, size, esc, "softname", SOFTNAME);
	if (a->user[0]) {
		append_param(url, size, esc, "ssid", a->user);
		append_param(url, size, esc, "sspassword", a->password);
	}
}

/* Media URLs from the provider embed the credentials of the request:
 * keep only the media parameters before storing one. */
static void strip_credentials(const char *in, char *out, size_t size)
{
	const char *q = strchr(in, '?');
	size_t o;

	if (!q) {
		copy_text(out, size, in);
		return;
	}
	o = (size_t)snprintf(out, size, "%.*s", (int)(q - in), in);
	for (const char *p = q + 1; *p && o + 1 < size;) {
		const char *amp = strchr(p, '&');
		size_t len = amp ? (size_t)(amp - p) : strlen(p);

		if (strncmp(p, "devid=", 6) && strncmp(p, "devpassword=", 12) &&
		    strncmp(p, "ssid=", 5) && strncmp(p, "sspassword=", 11) &&
		    strncmp(p, "softname=", 9))
			o += (size_t)snprintf(out + o, size - o, "%c%.*s",
					      strchr(out, '?') ? '&' : '?', (int)len, p);
		p += len + (amp ? 1 : 0);
	}
}

/* Download url to path atomically (".part" then rename). */
static bool download_media(const char *url, const char *path)
{
	struct buffer b = { .limit = 8 * 1024 * 1024 };
	char part[PATH_MAX + 8];
	char dir[PATH_MAX];
	char *slash;
	long status = http_get(url, &b);
	FILE *fp;
	bool ok = false;

	if (status == 200 && b.len > 64) {
		copy_text(dir, sizeof(dir), path);
		slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			mkdir_p(dir);
		}
		snprintf(part, sizeof(part), "%s.part", path);
		fp = fopen(part, "wb");
		if (fp) {
			ok = fwrite(b.data, 1, b.len, fp) == b.len;
			ok = (fflush(fp) == 0) && ok;
			ok = (fclose(fp) == 0) && ok;
			if (ok && !cancelled)
				ok = rename(part, path) == 0;
			else
				ok = false;
			if (!ok)
				unlink(part);
		}
	}
	free(b.data);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Response mapping                                                    */
/* ------------------------------------------------------------------ */

static const char *json_text(const cJSON *o, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	if (cJSON_IsString(v))
		return v->valuestring;
	if (cJSON_IsObject(v)) {
		const cJSON *t = cJSON_GetObjectItemCaseSensitive(v, "text");

		if (cJSON_IsString(t))
			return t->valuestring;
	}
	return NULL;
}

/* Pick the "text" of the array entry whose key matches the first
 * preference present ("region" or "langue"). */
static const char *pick(const cJSON *arr, const char *key, const char *const *prefs)
{
	const cJSON *it;

	if (!cJSON_IsArray(arr))
		return NULL;
	for (int p = 0; prefs[p]; p++)
		cJSON_ArrayForEach(it, arr) {
			const char *k = json_text(it, key);

			if (k && !strcmp(k, prefs[p]))
				return json_text(it, "text");
		}
	it = cJSON_GetArrayItem(arr, 0);
	return it ? json_text(it, "text") : NULL;
}

static const char *pick_media(const cJSON *medias, const char *type, const char *const *regions,
			      const char **format)
{
	const cJSON *it;

	if (!cJSON_IsArray(medias))
		return NULL;
	for (int p = 0; regions[p]; p++)
		cJSON_ArrayForEach(it, medias) {
			const char *t = json_text(it, "type");
			const char *r = json_text(it, "region");

			if (t && !strcmp(t, type) && (!r || !strcmp(r, regions[p]))) {
				*format = json_text(it, "format");
				return json_text(it, "url");
			}
		}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Metadata store                                                      */
/* ------------------------------------------------------------------ */

static void meta_path(const struct game_info *g, const char *ext, char *out, size_t size)
{
	snprintf(out, size, META_ROOT "/%s/%s.%s", g->system, g->stem, ext);
}

static bool cover_exists(const struct game_info *g)
{
	static const char *const exts[] = { "png", "jpg", "jpeg" };
	char path[PATH_MAX];

	for (size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), COVERS_ROOT "/%s/%s.%s", g->system, g->stem, exts[i]);
		if (access(path, F_OK) == 0)
			return true;
	}
	return false;
}

struct metadata {
	char provider_id[32];
	char match[16];
	char title[256];
	char description[4096];
	char release[32];
	char developer[128];
	char publisher[128];
	char genre[128];
	char players[32];
	char rating[16];
	char cover_url[1024];
	char cover_ext[8];
	char shot_url[1024];
	char shot_ext[8];
	char screenshot[PATH_MAX];
};

static int write_metadata(const struct game_info *g, const struct metadata *m)
{
	char path[PATH_MAX];
	char tmp[PATH_MAX + 8];
	char dir[PATH_MAX];
	FILE *fp;
	int rc = 0;

	snprintf(dir, sizeof(dir), META_ROOT "/%s", g->system);
	if (mkdir_p(dir) != 0)
		return -1;
	meta_path(g, "txt", path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "# nuubOS game metadata (provider-neutral)\nprovider=screenscraper\n");
	fprintf(fp, "provider_id=%s\nmatch=%s\n", m->provider_id, m->match);
#define FIELD(k, v) do { if ((v)[0]) fprintf(fp, k "=%s\n", (v)); } while (0)
	FIELD("title", m->title);
	FIELD("description", m->description);
	FIELD("release", m->release);
	FIELD("developer", m->developer);
	FIELD("publisher", m->publisher);
	FIELD("genre", m->genre);
	FIELD("players", m->players);
	FIELD("rating", m->rating);
	FIELD("screenshot", m->screenshot);
	/* Credential-free media references, for Accept. */
	FIELD("cover_url", m->cover_url);
	FIELD("cover_ext", m->cover_ext);
	FIELD("shot_url", m->shot_url);
	FIELD("shot_ext", m->shot_ext);
#undef FIELD
	if (ferror(fp))
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc)
		unlink(tmp);
	return rc;
}

static bool read_meta_value(const struct game_info *g, const char *key, char *out, size_t size)
{
	char path[PATH_MAX];
	char line[8192];
	size_t klen = strlen(key);
	FILE *fp;
	bool found = false;

	meta_path(g, "txt", path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			copy_text(out, size, line + klen + 1);
			found = true;
		}
	}
	fclose(fp);
	return found;
}

/* Fetch media (stored references + credentials at download time). */
static void fetch_media(const struct game_info *g, struct metadata *m, bool replace_cover)
{
	struct account a;
	CURL *esc = curl_easy_init();
	char url[2048];
	char path[PATH_MAX];

	if (!esc)
		return;
	load_account(&a);
	if (m->cover_url[0] && (replace_cover || !cover_exists(g))) {
		copy_text(url, sizeof(url), m->cover_url);
		credential_params(url, sizeof(url), esc, &a);
		snprintf(path, sizeof(path), COVERS_ROOT "/%s/%s.%s", g->system, g->stem,
			 m->cover_ext[0] ? m->cover_ext : "png");
		(void)download_media(url, path);
	}
	if (m->shot_url[0]) {
		copy_text(url, sizeof(url), m->shot_url);
		credential_params(url, sizeof(url), esc, &a);
		snprintf(path, sizeof(path), SHOTS_ROOT "/%s/%s.%s", g->system, g->stem,
			 m->shot_ext[0] ? m->shot_ext : "png");
		if (download_media(url, path))
			copy_text(m->screenshot, sizeof(m->screenshot), path);
	}
	curl_easy_cleanup(esc);
}

/* ------------------------------------------------------------------ */
/* Scraping                                                            */
/* ------------------------------------------------------------------ */

static bool file_crc(const char *path, unsigned long *crc, long long *size)
{
	struct stat st;
	unsigned char buf[65536];
	int fd;
	ssize_t n;

	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return false;
	*size = (long long)st.st_size;
	*crc = 0;
	if (st.st_size > MAX_HASH_SIZE)
		return true;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	*crc = crc32(0L, Z_NULL, 0);
	while ((n = read(fd, buf, sizeof(buf))) > 0 && !cancelled)
		*crc = crc32(*crc, buf, (uInt)n);
	close(fd);
	return n == 0;
}

enum outcome { SCRAPED, REVIEW, NOT_FOUND, SKIPPED, FAILED, STOP };
static const char *stop_reason = "";

static enum outcome scrape(const struct game_info *g, bool force)
{
	static const char *const regions[] = { "wor", "us", "eu", "ss", "jp", NULL };
	char langs_buf[3][4];
	const char *langs[4];
	char path[PATH_MAX];
	char url[4096];
	char crc_text[16] = "";
	unsigned long crc = 0;
	long long size = 0;
	struct account a;
	struct buffer b = { .limit = 4 * 1024 * 1024 };
	struct metadata m;
	cJSON *root, *jeu, *rom;
	CURL *esc;
	long status;
	int ss = provider_system(g->system);
	const char *base, *v, *fmt = NULL;
	enum outcome result;

	if (!ss)
		return SKIPPED;
	if (!force && (!strcmp(g->match, "user") || !strcmp(g->match, "auto") ||
		       !strcmp(g->match, "review")))
		return SKIPPED;
	meta_path(g, "rejected", path, sizeof(path));
	if (!force && access(path, F_OK) == 0)
		return SKIPPED;
	if (!SS_DEVID[0] || !SS_DEVPASSWORD[0]) {
		stop_reason = "unavailable";
		return STOP;
	}
	snprintf(path, sizeof(path), ROMS_ROOT "/%s", g->rel);
	if (!file_crc(path, &crc, &size))
		return FAILED;
	if (cancelled)
		return STOP;
	if (crc)
		snprintf(crc_text, sizeof(crc_text), "%08lX", crc);

	user_language(langs_buf[0], sizeof(langs_buf[0]));
	copy_text(langs_buf[1], sizeof(langs_buf[1]), "en");
	langs[0] = langs_buf[0];
	langs[1] = langs_buf[1];
	langs[2] = NULL;

	esc = curl_easy_init();
	if (!esc)
		return FAILED;
	load_account(&a);
	base = strrchr(g->rel, '/');
	snprintf(url, sizeof(url), API_BASE "/jeuInfos.php?output=json&romtype=rom");
	credential_params(url, sizeof(url), esc, &a);
	snprintf(url + strlen(url), sizeof(url) - strlen(url), "&systemeid=%d&romtaille=%lld", ss, size);
	append_param(url, sizeof(url), esc, "romnom", base ? base + 1 : g->rel);
	if (crc_text[0])
		append_param(url, sizeof(url), esc, "crc", crc_text);
	curl_easy_cleanup(esc);

	status = http_get(url, &b);
	if (cancelled) {
		free(b.data);
		return STOP;
	}
	if (status == 404) {
		free(b.data);
		return NOT_FOUND;
	}
	if (status == 401 || status == 403) {
		free(b.data);
		stop_reason = "auth";
		return STOP;
	}
	if (status == 429 || status == 430 || status == 431) {
		/* Quota or rate limit: stop, never retry in a loop. */
		free(b.data);
		stop_reason = "quota";
		return STOP;
	}
	if (status < 0 || status >= 500 || status == 423) {
		free(b.data);
		stop_reason = status < 0 ? "network" : "service";
		return STOP;
	}
	root = status == 200 && b.data ? cJSON_Parse(b.data) : NULL;
	free(b.data);
	jeu = root ? cJSON_GetObjectItemCaseSensitive(
			     cJSON_GetObjectItemCaseSensitive(root, "response"), "jeu") : NULL;
	if (!jeu) {
		cJSON_Delete(root);
		return NOT_FOUND;
	}

	memset(&m, 0, sizeof(m));
	copy_text(m.provider_id, sizeof(m.provider_id), json_text(jeu, "id"));
	/* Identified by checksum -> apply; name only -> review. */
	rom = cJSON_GetObjectItemCaseSensitive(jeu, "rom");
	v = rom ? json_text(rom, "romcrc") : NULL;
	copy_text(m.match, sizeof(m.match),
		  crc_text[0] && v && !strcasecmp(v, crc_text) ? "auto" : "review");
	one_line(pick(cJSON_GetObjectItemCaseSensitive(jeu, "noms"), "region", regions),
		 m.title, sizeof(m.title));
	one_line(pick(cJSON_GetObjectItemCaseSensitive(jeu, "synopsis"), "langue", langs),
		 m.description, sizeof(m.description));
	one_line(pick(cJSON_GetObjectItemCaseSensitive(jeu, "dates"), "region", regions),
		 m.release, sizeof(m.release));
	one_line(json_text(jeu, "developpeur"), m.developer, sizeof(m.developer));
	one_line(json_text(jeu, "editeur"), m.publisher, sizeof(m.publisher));
	one_line(json_text(jeu, "joueurs"), m.players, sizeof(m.players));
	one_line(json_text(jeu, "note"), m.rating, sizeof(m.rating));
	{
		const cJSON *genres = cJSON_GetObjectItemCaseSensitive(jeu, "genres");
		const cJSON *first = cJSON_IsArray(genres) ? cJSON_GetArrayItem(genres, 0) : NULL;

		if (first)
			one_line(pick(cJSON_GetObjectItemCaseSensitive(first, "noms"), "langue", langs),
				 m.genre, sizeof(m.genre));
	}
	{
		const cJSON *medias = cJSON_GetObjectItemCaseSensitive(jeu, "medias");
		const char *u = pick_media(medias, "box-2D", regions, &fmt);

		if (u) {
			strip_credentials(u, m.cover_url, sizeof(m.cover_url));
			copy_text(m.cover_ext, sizeof(m.cover_ext),
				  fmt && !strcmp(fmt, "jpg") ? "jpg" : "png");
		}
		fmt = NULL;
		u = pick_media(medias, "ss", regions, &fmt);
		if (u) {
			strip_credentials(u, m.shot_url, sizeof(m.shot_url));
			copy_text(m.shot_ext, sizeof(m.shot_ext),
				  fmt && !strcmp(fmt, "jpg") ? "jpg" : "png");
		}
	}
	cJSON_Delete(root);

	if (!strcmp(m.match, "auto"))
		fetch_media(g, &m, force);
	if (cancelled)
		return STOP;
	result = write_metadata(g, &m) == 0 ? (!strcmp(m.match, "auto") ? SCRAPED : REVIEW) : FAILED;
	if (result != FAILED) {
		meta_path(g, "rejected", path, sizeof(path));
		unlink(path);
	}
	return result;
}

static void library_rescan(void)
{
	char buf[256];

	(void)library_request("SCAN\n", buf, sizeof(buf));
}

static int cmd_game(const char *id, bool force)
{
	struct game_info g;
	enum outcome o;

	if (!valid_game(id) || !game_details(id, &g)) {
		job_error("game");
		return 1;
	}
	job_progress(-1, g.title);
	o = scrape(&g, force);
	library_rescan();
	switch (o) {
	case SCRAPED:
		printf(as_job ? "@result scraped\n" : "OK scraped\n");
		return 0;
	case REVIEW:
		printf(as_job ? "@result review\n" : "OK review\n");
		return 0;
	case NOT_FOUND:
		job_error("not-found");
		return 1;
	case SKIPPED:
		job_error(provider_system(g.system) ? "matched" : "system");
		return 1;
	case STOP:
		job_error(cancelled ? "cancelled" : stop_reason);
		return 1;
	default:
		job_error("storage");
		return 1;
	}
}

static int cmd_bulk(const char *scope)
{
	static char status[262144];
	static char list[262144];
	char ids[4096][17];
	size_t count = 0, done = 0, scraped = 0, review = 0;
	char *line, *save = NULL;

	if (strcmp(scope, "all") && strncmp(scope, "system:", 7)) {
		job_error("scope");
		return 1;
	}
	if (library_request("STATUS\n", status, sizeof(status)) != 0) {
		job_error("library");
		return 1;
	}
	for (line = strtok_r(status, "\n", &save); line && count < 4096;
	     line = strtok_r(NULL, "\n", &save)) {
		char system[64], cmd[96];
		char *tab, *l2, *save2 = NULL;

		if (strncmp(line, "system=", 7))
			continue;
		copy_text(system, sizeof(system), line + 7);
		if ((tab = strchr(system, '\t')))
			*tab = '\0';
		if (!strncmp(scope, "system:", 7) && strcmp(scope + 7, system))
			continue;
		if (!provider_system(system))
			continue;
		snprintf(cmd, sizeof(cmd), "GAMES\tsystem:%s\n", system);
		if (library_request(cmd, list, sizeof(list)) != 0)
			continue;
		for (l2 = strtok_r(list, "\n", &save2); l2 && count < 4096;
		     l2 = strtok_r(NULL, "\n", &save2)) {
			if (strncmp(l2, "game=", 5) || strlen(l2) < 21)
				continue;
			snprintf(ids[count], 17, "%.16s", l2 + 5);
			/* Unavailable games cannot be hashed: skip them. */
			{
				char *f = l2;
				int field = 0;

				for (char *p = l2; *p; p++)
					if (*p == '\t' && ++field == 7) {
						f = p + 1;
						break;
					}
				if (field == 7 && f[0] == '0')
					continue;
			}
			count++;
		}
	}
	for (size_t i = 0; i < count && !cancelled; i++) {
		struct game_info g;
		enum outcome o;

		job_progress((int)(done * 100 / (count ? count : 1)), NULL);
		done++;
		if (!game_details(ids[i], &g))
			continue;
		o = scrape(&g, false);
		if (o == SCRAPED)
			scraped++;
		else if (o == REVIEW)
			review++;
		else if (o == STOP)
			break;
	}
	library_rescan();
	if (cancelled || stop_reason[0]) {
		job_error(cancelled ? "cancelled" : stop_reason);
		return 1;
	}
	printf(as_job ? "@result %zu %zu %zu\n" : "OK %zu %zu %zu\n", scraped, review, count);
	return 0;
}

static int cmd_accept(const char *id)
{
	struct game_info g;
	struct metadata m;
	char path[PATH_MAX];
	char line[8192];
	FILE *fp;

	if (!valid_game(id) || !game_details(id, &g)) {
		puts("ERR game");
		return 1;
	}
	memset(&m, 0, sizeof(m));
	meta_path(&g, "txt", path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp) {
		puts("ERR no-match");
		return 1;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		line[strcspn(line, "\r\n")] = '\0';
		if (!(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
#define READ(k, f) else if (!strcmp(line, k)) copy_text(m.f, sizeof(m.f), eq)
		if (0) {}
		READ("provider_id", provider_id); READ("title", title);
		READ("description", description); READ("release", release);
		READ("developer", developer); READ("publisher", publisher);
		READ("genre", genre); READ("players", players); READ("rating", rating);
		READ("screenshot", screenshot); READ("cover_url", cover_url);
		READ("cover_ext", cover_ext); READ("shot_url", shot_url); READ("shot_ext", shot_ext);
#undef READ
	}
	fclose(fp);
	copy_text(m.match, sizeof(m.match), "user");
	if (SS_DEVID[0])
		fetch_media(&g, &m, false);
	if (write_metadata(&g, &m) != 0) {
		puts("ERR storage");
		return 1;
	}
	library_rescan();
	puts("OK");
	return 0;
}

static int cmd_reject_or_clear(const char *id, bool reject)
{
	struct game_info g;
	char path[PATH_MAX];
	char value[PATH_MAX];
	static const char *const exts[] = { "png", "jpg", "jpeg" };

	if (!valid_game(id) || !game_details(id, &g)) {
		puts("ERR game");
		return 1;
	}
	/* Scraped media goes with the metadata; user-supplied covers next
	 * to the ROMs (media/covers) are never touched. */
	if (read_meta_value(&g, "screenshot", value, sizeof(value)) &&
	    !strncmp(value, SHOTS_ROOT "/", strlen(SHOTS_ROOT) + 1))
		unlink(value);
	if (!reject && read_meta_value(&g, "provider", value, sizeof(value)))
		for (size_t i = 0; i < 3; i++) {
			snprintf(path, sizeof(path), COVERS_ROOT "/%s/%s.%s", g.system, g.stem, exts[i]);
			unlink(path);
		}
	meta_path(&g, "txt", path, sizeof(path));
	unlink(path);
	if (reject) {
		FILE *fp;

		snprintf(value, sizeof(value), META_ROOT "/%s", g.system);
		mkdir_p(value);
		meta_path(&g, "rejected", path, sizeof(path));
		fp = fopen(path, "w");
		if (fp)
			fclose(fp);
	}
	library_rescan();
	puts("OK");
	return 0;
}

static int cmd_account(int argc, char **argv)
{
	struct account a;

	if (argc == 3 && !strcmp(argv[2], "status")) {
		load_account(&a);
		printf("user=%s\nprovider=%s\nend=1\n", a.user,
		       SS_DEVID[0] ? "available" : "unavailable");
		return 0;
	}
	if (argc == 3 && !strcmp(argv[2], "clear")) {
		if (unlink(ACCOUNT_FILE) != 0 && errno != ENOENT) {
			puts("ERR storage");
			return 1;
		}
		puts("OK");
		return 0;
	}
	if (argc == 4 && !strcmp(argv[2], "set")) {
		memset(&a, 0, sizeof(a));
		if (strlen(argv[3]) >= sizeof(a.user) || strpbrk(argv[3], "=\n\r")) {
			puts("ERR name");
			return 1;
		}
		copy_text(a.user, sizeof(a.user), argv[3]);
		if (!fgets(a.password, sizeof(a.password), stdin)) {
			puts("ERR password");
			return 1;
		}
		a.password[strcspn(a.password, "\r\n")] = '\0';
		if (!a.password[0] || save_account(&a) != 0) {
			puts("ERR storage");
			return 1;
		}
		puts("OK");
		return 0;
	}
	fprintf(stderr, "Usage: nuubos-scraper account status|clear|set NAME\n");
	return 2;
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
	curl_global_init(CURL_GLOBAL_DEFAULT);

	if (argc >= 3 && !strcmp(argv[1], "game"))
		rc = cmd_game(argv[2], argc >= 4 && !strcmp(argv[3], "force"));
	else if (argc == 3 && !strcmp(argv[1], "bulk"))
		rc = cmd_bulk(argv[2]);
	else if (argc == 3 && !strcmp(argv[1], "accept"))
		rc = cmd_accept(argv[2]);
	else if (argc == 3 && !strcmp(argv[1], "reject"))
		rc = cmd_reject_or_clear(argv[2], true);
	else if (argc == 3 && !strcmp(argv[1], "clear"))
		rc = cmd_reject_or_clear(argv[2], false);
	else if (argc >= 3 && !strcmp(argv[1], "account"))
		rc = cmd_account(argc, argv);
	else
		fprintf(stderr, "Usage: nuubos-scraper game GAME [force] | bulk all|system:ID | "
				"accept|reject|clear GAME | account status|clear|set NAME\n");
	curl_global_cleanup();
	return rc;
}
