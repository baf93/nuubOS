/* SPDX-License-Identifier: MIT */
/*
 * nuubos-libraryd — Game Library and ROM Discovery service
 * (EPIC-011 / EPIC-012 / EPIC-020 baseline).
 *
 * Owns:
 *   - the system registry loaded from /usr/share/nuubos/systems.conf;
 *   - the shared logical-game catalog (/userdata/library/catalog.tsv),
 *     reconciled by a background scan of /userdata/roms/<system>: new
 *     content is added, missing content becomes UNAVAILABLE, nothing is
 *     deleted;
 *   - the active user's library state under /userdata/users/<id>/library:
 *     play history (Last Played, Time Played, sessions), favorites and
 *     custom collections;
 *   - safe Delete Game (EPIC-011/012): the ROM and the files its .m3u/.cue
 *     owns, never content another game references, never user saves;
 *   - the Game Details view (EPIC-016), aggregating the catalog, the user's
 *     history and the provider-neutral metadata store
 *     /userdata/library/metadata/<system>/<rom name>.txt (EPIC-015);
 *   - the installed application registry (*.app manifests).
 *
 * Clients send one command per line on /run/nuubos/libraryd.sock. STATUS
 * returns the Home snapshot, SUBSCRIBE returns it and pushes a new one on
 * every change. The service sleeps in poll() without timeout when idle.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Roots are overridable at build time for host tests. */
#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef USERDATA_ROOT
#define USERDATA_ROOT "/userdata"
#endif
#ifndef SHARE_ROOT
#define SHARE_ROOT "/usr/share/nuubos"
#endif
#define SOCKET_PATH RUN_ROOT "/libraryd.sock"
#define PIDFILE RUN_ROOT "/libraryd.pid"
#define REGISTRY_PATH SHARE_ROOT "/systems.conf"
#define ROMS_ROOT USERDATA_ROOT "/roms"
#define CATALOG_DIR USERDATA_ROOT "/library"
#define CATALOG_PATH CATALOG_DIR "/catalog.tsv"
#define USERDATA_USERS USERDATA_ROOT "/users"
#define ACTIVE_DIR RUN_ROOT "/user"
#define ACTIVE_FILE ACTIVE_DIR "/active"
#define SYSTEM_ICONS_DIR SHARE_ROOT "/systems"
#define COVERS_ROOT CATALOG_DIR "/covers"
#define METADATA_ROOT CATALOG_DIR "/metadata"
#define SYSTEM_APPS_DIR SHARE_ROOT "/applications"
#define USER_APPS_DIR USERDATA_ROOT "/applications"

/* v2: paths relative to roms/, identity = system + file name + size,
 * absolute cover paths. Older catalogs are rebuilt by the first scan. */
#define CATALOG_HEADER "# nuubOS library catalog v2"
#define MAX_CLIENTS 16
#define MAX_LINE 1024
#define MAX_SYSTEMS 128
#define MAX_APPS 64
#define MAX_SCAN_DEPTH 6
#define RECENT_LIMIT 10
/* A session longer than this is an emulator left running or a lost END:
 * count the cap, never unbounded phantom playtime (EPIC-020). */
#define MAX_SESSION_SEC (12LL * 3600LL)

struct system_def {
	char id[24];
	char name[64];
	char dir[32];
	char exts[192];
	int aspect;
	char color[8];
	/* ",key,key," folder-name keys: id, folder, name and aliases. */
	char keys[256];
	char icon[160];
};

struct game {
	char id[17];
	int sys;
	char *rel;
	char *title;
	char *cover;
	long long size;
	long long mtime;
	long long added;
	bool available;
	bool seen; /* found by the scan being reconciled */
};

struct history_entry {
	char id[17];
	long long last;
	long long time;
	int sessions;
};

struct collection {
	char id[16];
	char name[72];
	char (*games)[17];
	size_t count;
};

struct app_def {
	char id[48];
	char name[64];
	char icon[256];
	int order;
};

struct scan_item {
	int sys;
	char *rel;
	char *cover;
	long long size;
	long long mtime;
};

struct scan_result {
	struct scan_item *items;
	size_t count;
	size_t cap;
	bool dir_present[MAX_SYSTEMS];
};

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

struct strbuf {
	char *data;
	size_t len;
	size_t cap;
};

static volatile sig_atomic_t running = 1;

static struct system_def systems[MAX_SYSTEMS];
static int system_count;

static struct game *games;
static size_t game_count;
static size_t game_cap;

static char active_user[40];
static struct history_entry *history;
static size_t history_count;
static size_t history_cap;
static char (*favorites)[17];
static size_t favorite_count;
static struct collection *collections;
static size_t collection_count;

static struct app_def apps[MAX_APPS];
static int app_count;

static struct client clients[MAX_CLIENTS];

static int scan_pipe[2] = { -1, -1 };
static pthread_t scan_thread;
static bool scanning;
static bool rescan_pending;
static struct scan_result *scan_output;

static char session_game[17];
static struct timespec session_start;

/* ------------------------------------------------------------------ */
/* Utilities                                                          */
/* ------------------------------------------------------------------ */

static void log_msg(const char *fmt, ...)
{
	va_list ap;
	time_t now = time(NULL);
	struct tm tm;
	char stamp[32];

	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s libraryd: ", stamp);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void on_signal(int signo)
{
	(void)signo;
	running = 0;
}

static void *xrealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);

	if (!p) {
		log_msg("out of memory");
		abort();
	}
	return p;
}

static char *xstrdup(const char *s)
{
	char *p = strdup(s ? s : "");

	if (!p) {
		log_msg("out of memory");
		abort();
	}
	return p;
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static long long wall_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec;
}

static void trim(char *s)
{
	size_t len = strlen(s);
	size_t start = 0;

	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
	while (s[start] && isspace((unsigned char)s[start]))
		start++;
	if (start)
		memmove(s, s + start, len - start + 1);
}

static uint64_t fnv1a64(const char *a, const char *b)
{
	uint64_t h = 1469598103934665603ULL;

	for (const char *s = a; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	h ^= '/';
	h *= 1099511628211ULL;
	for (const char *s = b; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

/*
 * Fast discovery identity (EPIC-012): system + file name + size. Moving a
 * ROM to another folder keeps its identity (and the users' history);
 * a renamed or changed file is a new game, never a false merge.
 */
static void game_id_for(int sys, const char *rel, long long size, char out[17])
{
	const char *base = strrchr(rel, '/');
	char key[600];

	snprintf(key, sizeof(key), "%s\x1f%lld", base ? base + 1 : rel, size);
	snprintf(out, 17, "%016llx",
		 (unsigned long long)fnv1a64(systems[sys].id, key));
}

static bool valid_game_id(const char *id)
{
	if (strlen(id) != 16)
		return false;
	for (int i = 0; i < 16; i++)
		if (!isxdigit((unsigned char)id[i]))
			return false;
	return true;
}

static bool valid_user_id(const char *id)
{
	size_t len = strlen(id);

	if (len == 0 || len > 36)
		return false;
	for (size_t i = 0; i < len; i++)
		if (!isxdigit((unsigned char)id[i]) && id[i] != '-')
			return false;
	return true;
}

/* Text stored in TSV files and sent to clients never contains tabs or
 * line breaks. */
static bool tsv_safe(const char *s)
{
	return strpbrk(s, "\t\r\n") == NULL;
}

/* Bounded path formatting: false when the result would not fit, so an
 * over-long name is skipped instead of silently truncated. */
static bool pathf(char *out, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static bool pathf(char *out, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(out, size, fmt, ap);
	va_end(ap);
	return n >= 0 && (size_t)n < size;
}

static void mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void sb_append(struct strbuf *b, const char *fmt, ...)
{
	va_list ap;
	int need;

	for (;;) {
		size_t room = b->cap - b->len;

		va_start(ap, fmt);
		need = vsnprintf(b->data ? b->data + b->len : NULL, room, fmt, ap);
		va_end(ap);
		if (need < 0)
			return;
		if ((size_t)need < room) {
			b->len += (size_t)need;
			return;
		}
		b->cap = (b->cap + (size_t)need + 1) * 2;
		b->data = xrealloc(b->data, b->cap);
	}
}

/*
 * Write a whole file atomically: temporary file in the same directory,
 * fsync, rename. USERDATA is exFAT; rename over an existing file is
 * supported there and never leaves a half-written catalog.
 */
static int write_atomic(const char *path, const char *data, size_t len)
{
	char tmp[PATH_MAX];
	int fd;
	size_t off = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	while (off < len) {
		ssize_t n = write(fd, data + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp);
			return -1;
		}
		off += (size_t)n;
	}
	if (fsync(fd) != 0 || close(fd) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* System registry                                                    */
/* ------------------------------------------------------------------ */

/* Folder names are compared without case, spaces or punctuation:
 * "Super Nintendo", "super-nintendo" and "SuperNintendo" are one key. */
static void folder_key(const char *in, char *out, size_t size)
{
	size_t o = 0;

	for (; *in && o + 1 < size; in++)
		if (isalnum((unsigned char)*in))
			out[o++] = (char)tolower((unsigned char)*in);
	out[o] = '\0';
}

static void add_key(struct system_def *s, const char *text)
{
	char key[48];
	size_t len;

	folder_key(text, key, sizeof(key));
	len = strlen(s->keys);
	if (key[0] && len + strlen(key) + 2 < sizeof(s->keys))
		snprintf(s->keys + len, sizeof(s->keys) - len, "%s,", key);
}

static void load_registry(void)
{
	FILE *fp = fopen(REGISTRY_PATH, "r");
	char line[768];

	system_count = 0;
	if (!fp) {
		log_msg("registry %s missing: %s", REGISTRY_PATH, strerror(errno));
		return;
	}
	while (fgets(line, sizeof(line), fp) && system_count < MAX_SYSTEMS) {
		char *field[7] = { NULL };
		char *p = line;
		int n = 0;
		struct system_def *s;
		struct stat st;

		trim(line);
		if (line[0] == '#' || line[0] == '\0')
			continue;
		while (n < 7) {
			field[n++] = p;
			p = strchr(p, '|');
			if (!p)
				break;
			*p++ = '\0';
		}
		if (n < 6)
			continue;
		s = &systems[system_count];
		memset(s, 0, sizeof(*s));
		copy_text(s->id, sizeof(s->id), field[0]);
		copy_text(s->name, sizeof(s->name), field[1]);
		copy_text(s->dir, sizeof(s->dir), field[2]);
		snprintf(s->exts, sizeof(s->exts), ",%s,", field[3]);
		s->aspect = atoi(field[4]);
		if (s->aspect < 400 || s->aspect > 2000)
			s->aspect = 1000;
		copy_text(s->color, sizeof(s->color), field[5]);
		if (!s->id[0] || !s->dir[0] || strchr(s->dir, '/') || !tsv_safe(s->name))
			continue;
		s->keys[0] = ',';
		add_key(s, s->id);
		add_key(s, s->dir);
		add_key(s, s->name);
		if (n == 7) {
			char *save = NULL;

			for (char *a = strtok_r(field[6], ",", &save); a; a = strtok_r(NULL, ",", &save))
				add_key(s, a);
		}
		if (pathf(s->icon, sizeof(s->icon), SYSTEM_ICONS_DIR "/%s.png", s->id) &&
		    (stat(s->icon, &st) != 0 || !S_ISREG(st.st_mode)))
			s->icon[0] = '\0';
		system_count++;
	}
	fclose(fp);
	log_msg("registry: %d systems", system_count);
}

/* One folder per registered system, so users see a tidy layout; ROMs may
 * also be dropped anywhere under roms/ (EPIC-012 storage layout). */
static void ensure_rom_dirs(void)
{
	char path[PATH_MAX];

	if (mkdir(ROMS_ROOT, 0755) != 0 && errno != EEXIST)
		return;
	for (int i = 0; i < system_count; i++)
		if (pathf(path, sizeof(path), ROMS_ROOT "/%s", systems[i].dir))
			mkdir(path, 0755);
}

static int system_by_id(const char *id)
{
	for (int i = 0; i < system_count; i++)
		if (!strcmp(systems[i].id, id))
			return i;
	return -1;
}

/* System whose id, folder, name or alias matches a folder name. */
static int system_by_folder(const char *name)
{
	char key[48];
	char needle[52];

	folder_key(name, key, sizeof(key));
	if (!key[0])
		return -1;
	snprintf(needle, sizeof(needle), ",%s,", key);
	for (int i = 0; i < system_count; i++)
		if (strstr(systems[i].keys, needle))
			return i;
	return -1;
}

static const char *extension_of(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && dot != name ? dot + 1 : NULL;
}

static bool has_suffix(const char *name, const char *ext, size_t ext_len)
{
	size_t len = strlen(name);

	return len > ext_len + 1 && name[len - ext_len - 1] == '.' &&
	       !strncasecmp(name + len - ext_len, ext, ext_len);
}

/* Registry extensions may have several dots ("p8.png"). */
static bool system_accepts(int sys, const char *name)
{
	const char *p = systems[sys].exts + 1;

	while (*p) {
		const char *end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		if (len && has_suffix(name, p, len))
			return true;
		if (!end)
			break;
		p = end + 1;
	}
	return false;
}

/* The only system accepting this file name, -1 if none, -2 if several. */
static int system_by_extension(const char *name)
{
	int found = -1;

	for (int i = 0; i < system_count; i++) {
		if (!system_accepts(i, name))
			continue;
		if (found >= 0)
			return -2;
		found = i;
	}
	return found;
}

/* ------------------------------------------------------------------ */
/* Titles                                                             */
/* ------------------------------------------------------------------ */

/* "Super Mario World (USA) [!].sfc" -> "Super Mario World". Locally
 * derived identity only; metadata providers enrich it later (EPIC-015). */
static char *title_from_rel(const char *rel)
{
	const char *base = strrchr(rel, '/');
	char stem[512];
	char out[512];
	size_t o = 0;
	int depth = 0;
	char *dot;

	copy_text(stem, sizeof(stem), base ? base + 1 : rel);
	dot = strrchr(stem, '.');
	if (dot && dot != stem)
		*dot = '\0';
	for (const char *p = stem; *p && o + 1 < sizeof(out); p++) {
		if (*p == '(' || *p == '[') {
			depth++;
			continue;
		}
		if ((*p == ')' || *p == ']') && depth > 0) {
			depth--;
			continue;
		}
		if (depth > 0)
			continue;
		if (*p == '_')
			out[o++] = ' ';
		else
			out[o++] = *p;
	}
	out[o] = '\0';
	/* Collapse runs of spaces left behind by removed tags. */
	{
		char *w = out;
		bool space = false;

		for (char *r = out; *r; r++) {
			if (*r == ' ') {
				if (space)
					continue;
				space = true;
			} else {
				space = false;
			}
			*w++ = *r;
		}
		*w = '\0';
	}
	trim(out);
	if (!out[0])
		copy_text(out, sizeof(out), stem);
	return xstrdup(out);
}

/* ------------------------------------------------------------------ */
/* Background scan (runs on its own thread, reads only the registry)  */
/* ------------------------------------------------------------------ */

struct ref_list {
	char **items;
	size_t count;
	size_t cap;
};

struct scan_ctx {
	struct scan_result *out;
	struct ref_list refs;
	struct ref_list unclassified;
};

static void ref_add(struct ref_list *refs, const char *rel)
{
	if (refs->count == refs->cap) {
		refs->cap = refs->cap ? refs->cap * 2 : 32;
		refs->items = xrealloc(refs->items, refs->cap * sizeof(char *));
	}
	refs->items[refs->count++] = xstrdup(rel);
}

static int ref_compare(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* Resolve "name" relative to the descriptor directory, collapsing "./"
 * and refusing anything that escapes roms/. */
static bool join_relative(const char *desc_rel, const char *name, char *out, size_t size)
{
	char dir[PATH_MAX];
	const char *slash = strrchr(desc_rel, '/');

	if (name[0] == '/' || strstr(name, ".."))
		return false;
	while (name[0] == '.' && name[1] == '/')
		name += 2;
	if (slash) {
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - desc_rel), desc_rel);
		return pathf(out, size, "%s/%s", dir, name);
	}
	return pathf(out, size, "%s", name) && out[0] != '\0';
}

/* Entries of a .m3u (discs) or .cue (FILE tracks), relative to roms/. */
static size_t descriptor_entries(const char *rel, char entries[][PATH_MAX], size_t max)
{
	char path[PATH_MAX];
	char line[1024];
	bool cue = has_suffix(rel, "cue", 3);
	bool m3u = has_suffix(rel, "m3u", 3);
	size_t n = 0;
	FILE *fp;

	if ((!cue && !m3u) || !pathf(path, sizeof(path), ROMS_ROOT "/%s", rel))
		return 0;
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (n < max && fgets(line, sizeof(line), fp)) {
		char name[1024];

		trim(line);
		if (!line[0])
			continue;
		if (m3u) {
			if (line[0] == '#')
				continue;
			copy_text(name, sizeof(name), line);
		} else {
			char *p;

			if (strncasecmp(line, "FILE ", 5))
				continue;
			p = line + 5;
			while (*p == ' ')
				p++;
			if (*p == '"') {
				char *end = strchr(p + 1, '"');

				if (!end)
					continue;
				snprintf(name, sizeof(name), "%.*s", (int)(end - p - 1), p + 1);
			} else {
				char *end = strchr(p, ' ');

				snprintf(name, sizeof(name), "%.*s",
					 end ? (int)(end - p) : (int)strlen(p), p);
			}
		}
		if (join_relative(rel, name, entries[n], PATH_MAX))
			n++;
	}
	fclose(fp);
	return n;
}

static int sniff_depth;

/* Identify a loose disc or cartridge image whose extension several
 * systems share, from well-known header strings. Unknown stays unknown:
 * such files need their system folder. */
static int sniff_system(const char *rel)
{
	static const struct { const char *magic; const char *system; } discs[] = {
		{ "PLAYSTATION", "psx" },
		{ "SEGASATURN", "saturn" },
		{ "SEGADISCSYSTEM", "segacd" },
		{ "SEGAKATANA", "dreamcast" },
	};
	char path[PATH_MAX];
	unsigned char buf[40960];
	ssize_t n;
	int fd;

	if (has_suffix(rel, "cue", 3) || has_suffix(rel, "m3u", 3)) {
		char first[1][PATH_MAX];
		int sys = -1;

		/* .m3u -> first disc, .cue -> first track. */
		if (sniff_depth < 2 && descriptor_entries(rel, first, 1) == 1) {
			sniff_depth++;
			sys = sniff_system(first[0]);
			sniff_depth--;
		}
		return sys;
	}
	if (!pathf(path, sizeof(path), ROMS_ROOT "/%s", rel))
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n <= 0)
		return -1;
	for (size_t i = 0; i < sizeof(discs) / sizeof(discs[0]); i++)
		if (memmem(buf, (size_t)n, discs[i].magic, strlen(discs[i].magic)))
			return system_by_id(discs[i].system);
	/* Mega Drive / Genesis cartridge header at 0x100. */
	if (has_suffix(rel, "bin", 3) && n > 0x104 && !memcmp(buf + 0x100, "SEGA", 4))
		return system_by_id("megadrive");
	return -1;
}

static bool skip_directory(const char *name)
{
	static const char *const skipped[] = {
		"media", "images", "videos", "manuals", "covers", "boxart",
		"snap", "snaps", "screenshots", "bios", NULL
	};

	if (name[0] == '.')
		return true;
	for (int i = 0; skipped[i]; i++)
		if (!strcasecmp(name, skipped[i]))
			return true;
	return false;
}

static bool regular_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * Shared cover art: the canonical store /userdata/library/covers/<system>/
 * (also the future scraper target), then a media/covers folder next to the
 * ROM. Named like the ROM without its extension.
 */
static char *find_cover(int sys, const char *rel)
{
	static const char *const exts[] = { "png", "jpg", "jpeg", NULL };
	const char *base = strrchr(rel, '/');
	char stem[512];
	char dir[PATH_MAX];
	char path[PATH_MAX];
	char *dot;

	copy_text(stem, sizeof(stem), base ? base + 1 : rel);
	dot = strrchr(stem, '.');
	if (dot && dot != stem)
		*dot = '\0';
	snprintf(dir, sizeof(dir), "%.*s", base ? (int)(base - rel) : 0, rel);
	for (int i = 0; exts[i]; i++)
		if (pathf(path, sizeof(path), COVERS_ROOT "/%s/%s.%s", systems[sys].id, stem, exts[i]) &&
		    regular_file(path))
			return xstrdup(path);
	for (int i = 0; exts[i]; i++)
		if (pathf(path, sizeof(path), ROMS_ROOT "/%s%smedia/covers/%s.%s", dir,
			  dir[0] ? "/" : "", stem, exts[i]) &&
		    regular_file(path))
			return xstrdup(path);
	return NULL;
}

/*
 * The system of a file: its nearest folder named after a system when that
 * system takes the file, else the only system taking its extension, else a
 * header sniff for shared disc/cartridge formats.
 */
static int classify(const char *rel, const char *name, int folder_sys)
{
	int sys;

	if (folder_sys >= 0 && system_accepts(folder_sys, name))
		return folder_sys;
	sys = system_by_extension(name);
	if (sys >= 0)
		return sys;
	if (sys == -2)
		return sniff_system(rel);
	return -1;
}

static void walk(struct scan_ctx *ctx, const char *sub, int depth, int folder_sys)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *de;

	if (!pathf(path, sizeof(path), ROMS_ROOT "%s%s", sub[0] ? "/" : "", sub))
		return;
	dir = opendir(path);
	if (!dir)
		return;
	while ((de = readdir(dir)) != NULL) {
		char rel[PATH_MAX];
		char full[PATH_MAX];
		struct stat st;
		int sys;

		if (de->d_name[0] == '.' || !tsv_safe(de->d_name))
			continue;
		if (!pathf(rel, sizeof(rel), "%s%s%s", sub, sub[0] ? "/" : "", de->d_name) ||
		    !pathf(full, sizeof(full), ROMS_ROOT "/%s", rel) ||
		    stat(full, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			int here;

			if (depth >= MAX_SCAN_DEPTH || skip_directory(de->d_name))
				continue;
			here = system_by_folder(de->d_name);
			walk(ctx, rel, depth + 1, here >= 0 ? here : folder_sys);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;
		if (has_suffix(de->d_name, "m3u", 3) || has_suffix(de->d_name, "cue", 3)) {
			static char entries[64][PATH_MAX];
			size_t n = descriptor_entries(rel, entries, 64);

			for (size_t i = 0; i < n; i++)
				ref_add(&ctx->refs, entries[i]);
		}
		if (system_by_extension(de->d_name) == -1)
			continue; /* not a game format at all: ignore quietly */
		sys = classify(rel, de->d_name, folder_sys);
		if (sys < 0) {
			ref_add(&ctx->unclassified, rel);
			continue;
		}
		if (ctx->out->count == ctx->out->cap) {
			ctx->out->cap = ctx->out->cap ? ctx->out->cap * 2 : 256;
			ctx->out->items = xrealloc(ctx->out->items, ctx->out->cap * sizeof(*ctx->out->items));
		}
		ctx->out->items[ctx->out->count++] = (struct scan_item){
			.sys = sys,
			.rel = xstrdup(rel),
			.cover = NULL,
			.size = (long long)st.st_size,
			.mtime = (long long)st.st_mtime,
		};
	}
	closedir(dir);
}

static void *scan_main(void *arg)
{
	struct scan_ctx ctx = { 0 };
	size_t keep = 0, skipped = 0;
	char byte = 1;

	(void)arg;
	ctx.out = calloc(1, sizeof(*ctx.out));
	if (!ctx.out)
		abort();
	walk(&ctx, "", 0, -1);

	/* Files referenced by a .m3u/.cue belong to that logical game. */
	qsort(ctx.refs.items, ctx.refs.count, sizeof(char *), ref_compare);
	for (size_t i = 0; i < ctx.out->count; i++) {
		struct scan_item *it = &ctx.out->items[i];
		const char *key = it->rel;

		if (ctx.refs.count &&
		    bsearch(&key, ctx.refs.items, ctx.refs.count, sizeof(char *), ref_compare)) {
			free(it->rel);
			continue;
		}
		it->cover = find_cover(it->sys, it->rel);
		ctx.out->items[keep++] = *it;
	}
	ctx.out->count = keep;
	/* Report shared-format files nobody could place (track files of a
	 * descriptor are not missing anything). */
	for (size_t i = 0; i < ctx.unclassified.count; i++) {
		const char *key = ctx.unclassified.items[i];

		if (!ctx.refs.count ||
		    !bsearch(&key, ctx.refs.items, ctx.refs.count, sizeof(char *), ref_compare))
			skipped++;
		free(ctx.unclassified.items[i]);
	}
	free(ctx.unclassified.items);
	for (size_t i = 0; i < ctx.refs.count; i++)
		free(ctx.refs.items[i]);
	free(ctx.refs.items);
	if (skipped)
		log_msg("scan: %zu files with a shared format outside a system folder were skipped",
			skipped);
	scan_output = ctx.out;
	while (write(scan_pipe[1], &byte, 1) < 0 && errno == EINTR)
		;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Shared catalog                                                     */
/* ------------------------------------------------------------------ */

static long find_game(const char *id)
{
	for (size_t i = 0; i < game_count; i++)
		if (!strcmp(games[i].id, id))
			return (long)i;
	return -1;
}

static struct game *add_game(void)
{
	if (game_count == game_cap) {
		game_cap = game_cap ? game_cap * 2 : 256;
		games = xrealloc(games, game_cap * sizeof(*games));
	}
	memset(&games[game_count], 0, sizeof(games[game_count]));
	return &games[game_count++];
}

static void save_catalog(void)
{
	struct strbuf b = { 0 };

	mkdir_p(CATALOG_DIR);
	sb_append(&b, CATALOG_HEADER "\n");
	for (size_t i = 0; i < game_count; i++) {
		const struct game *g = &games[i];

		sb_append(&b, "%s\t%s\t%s\t%s\t%s\t%lld\t%lld\t%lld\t%d\n",
			  g->id, systems[g->sys].id, g->rel, g->title,
			  g->cover ? g->cover : "", g->size, g->mtime, g->added,
			  g->available ? 1 : 0);
	}
	if (write_atomic(CATALOG_PATH, b.data ? b.data : "", b.len) != 0)
		log_msg("catalog save failed: %s", strerror(errno));
	free(b.data);
}

static void load_catalog(void)
{
	FILE *fp = fopen(CATALOG_PATH, "r");
	char line[2048];

	if (!fp)
		return;
	if (!fgets(line, sizeof(line), fp) || strncmp(line, CATALOG_HEADER, strlen(CATALOG_HEADER))) {
		log_msg("catalog: old format, rebuilding from scan");
		fclose(fp);
		return;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *f[9];
		char *p = line;
		int n = 0;
		int sys;
		struct game *g;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || !line[0])
			continue;
		while (n < 9) {
			f[n++] = p;
			p = strchr(p, '\t');
			if (!p)
				break;
			*p++ = '\0';
		}
		if (n != 9 || !valid_game_id(f[0]))
			continue;
		sys = system_by_id(f[1]);
		/* A system removed from the registry keeps no visible games. */
		if (sys < 0)
			continue;
		g = add_game();
		copy_text(g->id, sizeof(g->id), f[0]);
		g->sys = sys;
		g->rel = xstrdup(f[2]);
		g->title = xstrdup(f[3]);
		g->cover = f[4][0] ? xstrdup(f[4]) : NULL;
		g->size = atoll(f[5]);
		g->mtime = atoll(f[6]);
		g->added = atoll(f[7]);
		g->available = atoi(f[8]) != 0;
	}
	fclose(fp);
	log_msg("catalog: %zu games", game_count);
}

/*
 * Reconcile the known catalog with one scan: UNCHANGED / ADDED / CHANGED /
 * AVAILABLE_AGAIN / UNAVAILABLE. Entries are never deleted (EPIC-012).
 */
static bool reconcile(struct scan_result *scan)
{
	size_t added = 0, back = 0, gone = 0, changed = 0, duplicates = 0;
	long long now = wall_now();

	for (size_t i = 0; i < game_count; i++)
		games[i].seen = false;
	for (size_t i = 0; i < scan->count; i++) {
		struct scan_item *it = &scan->items[i];
		char id[17];
		long idx;
		struct game *g;

		game_id_for(it->sys, it->rel, it->size, id);
		idx = find_game(id);
		if (idx < 0) {
			g = add_game();
			copy_text(g->id, sizeof(g->id), id);
			g->sys = it->sys;
			g->rel = it->rel;
			g->title = title_from_rel(it->rel);
			g->cover = it->cover;
			g->size = it->size;
			g->mtime = it->mtime;
			g->added = now;
			g->available = true;
			g->seen = true;
			it->rel = NULL;
			it->cover = NULL;
			added++;
			continue;
		}
		g = &games[idx];
		if (g->seen) {
			/* Same file name and size twice: one logical game. */
			duplicates++;
			continue;
		}
		g->seen = true;
		if (strcmp(g->rel, it->rel)) {
			/* Moved to another folder: same game, new location. */
			free(g->rel);
			g->rel = it->rel;
			it->rel = NULL;
			changed++;
		}
		if (!g->available) {
			g->available = true;
			back++;
		}
		if (g->size != it->size || g->mtime != it->mtime) {
			g->size = it->size;
			g->mtime = it->mtime;
			changed++;
		}
		if ((g->cover == NULL) != (it->cover == NULL) ||
		    (g->cover && strcmp(g->cover, it->cover))) {
			free(g->cover);
			g->cover = it->cover;
			it->cover = NULL;
			changed++;
		}
	}
	for (size_t i = 0; i < game_count; i++) {
		if (!games[i].seen && games[i].available) {
			games[i].available = false;
			gone++;
		}
	}
	log_msg("scan: %zu found, %zu added, %zu changed, %zu available again, %zu unavailable, %zu duplicates",
		scan->count, added, changed, back, gone, duplicates);
	return added || changed || back || gone;
}

static void free_scan(struct scan_result *scan)
{
	for (size_t i = 0; i < scan->count; i++) {
		free(scan->items[i].rel);
		free(scan->items[i].cover);
	}
	free(scan->items);
	free(scan);
}

static void start_scan(void)
{
	if (scanning) {
		rescan_pending = true;
		return;
	}
	if (pthread_create(&scan_thread, NULL, scan_main, NULL) != 0) {
		log_msg("scan thread failed: %s", strerror(errno));
		return;
	}
	scanning = true;
	log_msg("scan started");
}

/* ------------------------------------------------------------------ */
/* Applications                                                       */
/* ------------------------------------------------------------------ */

static void load_app_dir(const char *dir_path)
{
	DIR *dir = opendir(dir_path);
	struct dirent *de;

	if (!dir)
		return;
	while ((de = readdir(dir)) != NULL && app_count < MAX_APPS) {
		const char *ext = extension_of(de->d_name);
		char path[PATH_MAX];
		char line[512];
		struct app_def app = { .order = 100 };
		FILE *fp;

		if (!ext || strcmp(ext, "app") || de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		while (fgets(line, sizeof(line), fp)) {
			char *eq;

			trim(line);
			if (line[0] == '#' || !(eq = strchr(line, '=')))
				continue;
			*eq++ = '\0';
			if (!strcmp(line, "ID"))
				copy_text(app.id, sizeof(app.id), eq);
			else if (!strcmp(line, "NAME"))
				copy_text(app.name, sizeof(app.name), eq);
			else if (!strcmp(line, "ICON"))
				snprintf(app.icon, sizeof(app.icon), "%s%s%s",
					 eq[0] == '/' ? "" : dir_path,
					 eq[0] == '/' ? "" : "/", eq);
			else if (!strcmp(line, "ORDER"))
				app.order = atoi(eq);
		}
		fclose(fp);
		if (!app.id[0] || !app.name[0] || !tsv_safe(app.name) ||
		    !tsv_safe(app.icon))
			continue;
		apps[app_count++] = app;
	}
	closedir(dir);
}

static int app_compare(const void *a, const void *b)
{
	const struct app_def *x = a, *y = b;

	if (x->order != y->order)
		return x->order - y->order;
	return strcasecmp(x->name, y->name);
}

static void load_apps(void)
{
	app_count = 0;
	load_app_dir(SYSTEM_APPS_DIR);
	load_app_dir(USER_APPS_DIR);
	qsort(apps, (size_t)app_count, sizeof(apps[0]), app_compare);
}

/* ------------------------------------------------------------------ */
/* Per-user library state                                             */
/* ------------------------------------------------------------------ */

static void user_file(const char *name, char *out, size_t size)
{
	snprintf(out, size, USERDATA_USERS "/%s/library/%s", active_user, name);
}

static void clear_user_state(void)
{
	free(history);
	history = NULL;
	history_count = history_cap = 0;
	free(favorites);
	favorites = NULL;
	favorite_count = 0;
	for (size_t i = 0; i < collection_count; i++)
		free(collections[i].games);
	free(collections);
	collections = NULL;
	collection_count = 0;
	session_game[0] = '\0';
}

static struct history_entry *history_for(const char *id, bool create)
{
	for (size_t i = 0; i < history_count; i++)
		if (!strcmp(history[i].id, id))
			return &history[i];
	if (!create)
		return NULL;
	if (history_count == history_cap) {
		history_cap = history_cap ? history_cap * 2 : 32;
		history = xrealloc(history, history_cap * sizeof(*history));
	}
	memset(&history[history_count], 0, sizeof(history[history_count]));
	copy_text(history[history_count].id, sizeof(history[history_count].id), id);
	return &history[history_count++];
}

static bool is_favorite(const char *id)
{
	for (size_t i = 0; i < favorite_count; i++)
		if (!strcmp(favorites[i], id))
			return true;
	return false;
}

static void id_list_add(char (**list)[17], size_t *count, const char *id)
{
	*list = xrealloc(*list, (*count + 1) * sizeof(**list));
	copy_text((*list)[*count], 17, id);
	(*count)++;
}

static void id_list_remove(char (*list)[17], size_t *count, const char *id)
{
	for (size_t i = 0; i < *count; i++) {
		if (strcmp(list[i], id))
			continue;
		memmove(&list[i], &list[i + 1], (*count - i - 1) * sizeof(list[0]));
		(*count)--;
		return;
	}
}

static bool id_list_has(char (*list)[17], size_t count, const char *id)
{
	for (size_t i = 0; i < count; i++)
		if (!strcmp(list[i], id))
			return true;
	return false;
}

static void load_user_state(void)
{
	char path[PATH_MAX];
	char line[4096];
	FILE *fp;

	clear_user_state();
	if (!active_user[0])
		return;

	user_file("history.tsv", path, sizeof(path));
	if ((fp = fopen(path, "r")) != NULL) {
		while (fgets(line, sizeof(line), fp)) {
			char id[32];
			long long last, played;
			int sessions;
			struct history_entry *h;

			if (line[0] == '#' ||
			    sscanf(line, "%31s %lld %lld %d", id, &last, &played, &sessions) != 4 ||
			    !valid_game_id(id))
				continue;
			h = history_for(id, true);
			h->last = last;
			h->time = played < 0 ? 0 : played;
			h->sessions = sessions < 0 ? 0 : sessions;
		}
		fclose(fp);
	}

	user_file("favorites.txt", path, sizeof(path));
	if ((fp = fopen(path, "r")) != NULL) {
		while (fgets(line, sizeof(line), fp)) {
			trim(line);
			if (valid_game_id(line) && !is_favorite(line))
				id_list_add(&favorites, &favorite_count, line);
		}
		fclose(fp);
	}

	user_file("collections.tsv", path, sizeof(path));
	if ((fp = fopen(path, "r")) != NULL) {
		while (fgets(line, sizeof(line), fp)) {
			char *name, *members, *save = NULL;
			struct collection *c;

			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] == '#' || !(name = strchr(line, '\t')))
				continue;
			*name++ = '\0';
			members = strchr(name, '\t');
			if (members)
				*members++ = '\0';
			if (!line[0] || strlen(line) >= sizeof(c->id) || !name[0])
				continue;
			collections = xrealloc(collections, (collection_count + 1) * sizeof(*collections));
			c = &collections[collection_count++];
			memset(c, 0, sizeof(*c));
			copy_text(c->id, sizeof(c->id), line);
			copy_text(c->name, sizeof(c->name), name);
			for (char *tok = members ? strtok_r(members, " ", &save) : NULL; tok;
			     tok = strtok_r(NULL, " ", &save))
				if (valid_game_id(tok) && !id_list_has(c->games, c->count, tok))
					id_list_add(&c->games, &c->count, tok);
		}
		fclose(fp);
	}
	log_msg("user %s: %zu history, %zu favorites, %zu collections",
		active_user, history_count, favorite_count, collection_count);
}

static int save_user_file(const char *name, struct strbuf *b)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];
	int rc;

	if (!active_user[0])
		return -1;
	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/library", active_user);
	mkdir_p(dir);
	user_file(name, path, sizeof(path));
	rc = write_atomic(path, b->data ? b->data : "", b->len);
	if (rc)
		log_msg("save %s failed: %s", path, strerror(errno));
	free(b->data);
	return rc;
}

static int save_history(void)
{
	struct strbuf b = { 0 };

	sb_append(&b, "# game-id last-played time-played sessions\n");
	for (size_t i = 0; i < history_count; i++)
		sb_append(&b, "%s %lld %lld %d\n", history[i].id, history[i].last,
			  history[i].time, history[i].sessions);
	return save_user_file("history.tsv", &b);
}

static int save_favorites(void)
{
	struct strbuf b = { 0 };

	for (size_t i = 0; i < favorite_count; i++)
		sb_append(&b, "%s\n", favorites[i]);
	return save_user_file("favorites.txt", &b);
}

static int save_collections(void)
{
	struct strbuf b = { 0 };

	for (size_t i = 0; i < collection_count; i++) {
		sb_append(&b, "%s\t%s\t", collections[i].id, collections[i].name);
		for (size_t j = 0; j < collections[i].count; j++)
			sb_append(&b, "%s%s", j ? " " : "", collections[i].games[j]);
		sb_append(&b, "\n");
	}
	return save_user_file("collections.tsv", &b);
}

static struct collection *collection_by_id(const char *id)
{
	for (size_t i = 0; i < collection_count; i++)
		if (!strcmp(collections[i].id, id))
			return &collections[i];
	return NULL;
}

static void refresh_active_user(void)
{
	char value[64] = "";
	FILE *fp = fopen(ACTIVE_FILE, "r");

	if (fp) {
		if (fgets(value, sizeof(value), fp))
			trim(value);
		fclose(fp);
	}
	if (!valid_user_id(value))
		value[0] = '\0';
	if (!strcmp(value, active_user))
		return;
	copy_text(active_user, sizeof(active_user), value);
	load_user_state();
}

/* ------------------------------------------------------------------ */
/* Snapshots                                                          */
/* ------------------------------------------------------------------ */

static void append_game(struct strbuf *b, const char *key, const struct game *g)
{
	const struct history_entry *h = history_for(g->id, false);
	const struct system_def *s = &systems[g->sys];

	sb_append(b, "%s=%s\t%s\t%s\t", key, g->id, s->id, g->title);
	sb_append(b, "%s", g->cover ? g->cover : "");
	sb_append(b, "\t%d\t%lld\t%lld\t%d\t%d\n", s->aspect,
		  h ? h->last : 0LL, h ? h->time : 0LL,
		  g->available ? 1 : 0, is_favorite(g->id) ? 1 : 0);
}

static int history_compare(const void *a, const void *b)
{
	const struct history_entry *x = *(const struct history_entry *const *)a;
	const struct history_entry *y = *(const struct history_entry *const *)b;

	return (x->last < y->last) - (x->last > y->last);
}

/* Played games, most recent first; limit 0 lists the whole history. */
static void append_recent(struct strbuf *b, const char *key, size_t limit)
{
	const struct history_entry **order;
	size_t n = 0, shown = 0;

	if (!history_count)
		return;
	order = malloc(history_count * sizeof(*order));
	if (!order)
		abort();
	for (size_t i = 0; i < history_count; i++)
		if (history[i].last > 0)
			order[n++] = &history[i];
	qsort(order, n, sizeof(*order), history_compare);
	for (size_t i = 0; i < n && (!limit || shown < limit); i++) {
		long idx = find_game(order[i]->id);

		if (idx < 0)
			continue;
		append_game(b, key, &games[idx]);
		shown++;
	}
	free(order);
}

static void build_status(struct strbuf *b)
{
	size_t available = 0;
	int per_system[MAX_SYSTEMS] = { 0 };

	for (size_t i = 0; i < game_count; i++) {
		if (!games[i].available)
			continue;
		available++;
		per_system[games[i].sys]++;
	}
	sb_append(b, "user=%s\nscanning=%d\ngames=%zu\n", active_user,
		  scanning ? 1 : 0, available);
	if (active_user[0]) {
		append_recent(b, "recent", RECENT_LIMIT);
		sb_append(b, "collection=favorites\t\t%zu\n", favorite_count);
		for (size_t i = 0; i < collection_count; i++)
			sb_append(b, "collection=%s\t%s\t%zu\n", collections[i].id,
				  collections[i].name, collections[i].count);
	}
	for (int i = 0; i < system_count; i++)
		if (per_system[i] > 0)
			sb_append(b, "system=%s\t%s\t%d\t%d\t%s\t%s\n", systems[i].id,
				  systems[i].name, per_system[i], systems[i].aspect,
				  systems[i].color, systems[i].icon);
	for (int i = 0; i < app_count; i++)
		sb_append(b, "app=%s\t%s\t%s\n", apps[i].id, apps[i].name, apps[i].icon);
	sb_append(b, "end=1\n");
}

static int title_compare(const void *a, const void *b)
{
	const struct game *x = *(const struct game *const *)a;
	const struct game *y = *(const struct game *const *)b;
	int c = strcasecmp(x->title, y->title);

	return c ? c : strcmp(x->rel, y->rel);
}

static const struct game **sorted_games(bool (*keep)(const struct game *, const void *),
					const void *arg, size_t *count)
{
	const struct game **list = malloc((game_count + 1) * sizeof(*list));
	size_t n = 0;

	if (!list)
		abort();
	for (size_t i = 0; i < game_count; i++)
		if (keep(&games[i], arg))
			list[n++] = &games[i];
	qsort(list, n, sizeof(*list), title_compare);
	*count = n;
	return list;
}

static bool keep_system(const struct game *g, const void *arg)
{
	return g->sys == *(const int *)arg;
}

static bool keep_favorite(const struct game *g, const void *arg)
{
	(void)arg;
	return is_favorite(g->id);
}

static bool keep_collection(const struct game *g, const void *arg)
{
	const struct collection *c = arg;

	return id_list_has(c->games, c->count, g->id);
}

/* GAMES <scope>: system:<id> | favorites | collection:<id> | recent */
static bool build_games(struct strbuf *b, const char *scope)
{
	const struct game **list = NULL;
	size_t n = 0;

	if (!strncmp(scope, "system:", 7)) {
		int sys = system_by_id(scope + 7);

		if (sys < 0)
			return false;
		list = sorted_games(keep_system, &sys, &n);
	} else if (!strcmp(scope, "favorites")) {
		list = sorted_games(keep_favorite, NULL, &n);
	} else if (!strncmp(scope, "collection:", 11)) {
		const struct collection *c = collection_by_id(scope + 11);

		if (!c)
			return false;
		list = sorted_games(keep_collection, c, &n);
	} else if (!strcmp(scope, "recent")) {
		append_recent(b, "game", 0);
		sb_append(b, "end=1\n");
		return true;
	} else {
		return false;
	}
	for (size_t i = 0; i < n; i++)
		append_game(b, "game", list[i]);
	free(list);
	sb_append(b, "end=1\n");
	return true;
}

/* ------------------------------------------------------------------ */
/* Game Details and Delete Game                                       */
/* ------------------------------------------------------------------ */

static void rom_stem(const char *rel, char *out, size_t size)
{
	const char *base = strrchr(rel, '/');
	char *dot;

	copy_text(out, size, base ? base + 1 : rel);
	dot = strrchr(out, '.');
	if (dot && dot != out)
		*dot = '\0';
}

/*
 * Provider-neutral metadata (EPIC-015): one "key=value" per line, values on
 * one line ("\n" escaped by the writer). Unknown keys are passed through as
 * meta_<key> so a new provider field needs no libraryd change.
 */
static void append_metadata(struct strbuf *b, const struct game *g)
{
	char stem[512];
	char path[PATH_MAX];
	char line[4096];
	FILE *fp;

	rom_stem(g->rel, stem, sizeof(stem));
	if (!pathf(path, sizeof(path), METADATA_ROOT "/%s/%s.txt", systems[g->sys].id, stem))
		return;
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || !(eq = strchr(line, '=')) || eq == line)
			continue;
		*eq++ = '\0';
		if (!tsv_safe(eq) || strspn(line, "abcdefghijklmnopqrstuvwxyz_0123456789") != strlen(line))
			continue;
		sb_append(b, "meta_%s=%s\n", line, eq);
	}
	fclose(fp);
}

static void build_details(struct strbuf *b, const struct game *g)
{
	const struct history_entry *h = history_for(g->id, false);

	append_game(b, "game", g);
	sb_append(b, "system_name=%s\nsessions=%d\npath=%s\nsize=%lld\nadded=%lld\n",
		  systems[g->sys].name, h ? h->sessions : 0, g->rel, g->size, g->added);
	for (size_t i = 0; i < collection_count; i++)
		sb_append(b, "member=%s\t%s\t%d\n", collections[i].id, collections[i].name,
			  id_list_has(collections[i].games, collections[i].count, g->id) ? 1 : 0);
	append_metadata(b, g);
	sb_append(b, "end=1\n");
}

/* True when another catalog game's descriptor references rel. */
static bool referenced_elsewhere(const char *rel, const struct game *self)
{
	static char entries[64][PATH_MAX];

	for (size_t i = 0; i < game_count; i++) {
		const struct game *o = &games[i];
		size_t n;

		if (o == self || !o->available)
			continue;
		if (!strcmp(o->rel, rel))
			return true;
		n = descriptor_entries(o->rel, entries, 64);
		for (size_t j = 0; j < n; j++)
			if (!strcmp(entries[j], rel))
				return true;
	}
	return false;
}

/*
 * Delete Game: the ROM file and the files its descriptor owns; the shared
 * cover and metadata derived from it. Saves, states and the users' history
 * are kept (explicit cleanup only). Returns NULL on success, else the
 * error; the main file is removed last so a failure leaves a launchable
 * game rather than a dangling entry.
 */
static const char *delete_game(long idx)
{
	static char owned[128][PATH_MAX];
	static char sub[64][PATH_MAX];
	struct game *g = &games[idx];
	char path[PATH_MAX];
	char stem[512];
	size_t n, top;
	bool failed = false;

	if (!g->available)
		return "unavailable";
	/* .m3u -> discs (.cue) -> tracks (.bin): two descriptor levels. */
	n = top = descriptor_entries(g->rel, owned, 64);
	for (size_t i = 0; i < top; i++) {
		size_t m = descriptor_entries(owned[i], sub, 64);

		for (size_t j = 0; j < m && n < 128; j++)
			memcpy(owned[n++], sub[j], PATH_MAX);
	}
	/* Tracks first, then the descriptors that listed them. */
	for (size_t k = n; k-- > 0;) {
		if (referenced_elsewhere(owned[k], g))
			continue;
		if (!pathf(path, sizeof(path), ROMS_ROOT "/%s", owned[k]))
			continue;
		if (unlink(path) != 0 && errno != ENOENT) {
			log_msg("delete %s: %s", path, strerror(errno));
			failed = true;
		}
	}
	if (failed)
		return "io";
	if (!pathf(path, sizeof(path), ROMS_ROOT "/%s", g->rel) ||
	    (unlink(path) != 0 && errno != ENOENT)) {
		log_msg("delete %s: %s", path, strerror(errno));
		return "io";
	}
	sync();
	rom_stem(g->rel, stem, sizeof(stem));
	if (g->cover && !strncmp(g->cover, COVERS_ROOT "/", strlen(COVERS_ROOT) + 1))
		unlink(g->cover);
	if (pathf(path, sizeof(path), METADATA_ROOT "/%s/%s.txt", systems[g->sys].id, stem))
		unlink(path);
	log_msg("deleted game %s (%s)", g->id, g->rel);
	free(g->rel);
	free(g->title);
	free(g->cover);
	memmove(&games[idx], &games[idx + 1], (game_count - (size_t)idx - 1) * sizeof(*games));
	game_count--;
	save_catalog();
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Clients                                                            */
/* ------------------------------------------------------------------ */

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->used = 0;
}

static void send_text(struct client *c, const char *data, size_t len)
{
	size_t off = 0;

	while (c->fd >= 0 && off < len) {
		ssize_t n = send(c->fd, data + off, len - off, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			close_client(c);
			return;
		}
		off += (size_t)n;
	}
}

static void reply(struct client *c, const char *text)
{
	send_text(c, text, strlen(text));
}

static void send_status(struct client *c)
{
	struct strbuf b = { 0 };

	build_status(&b);
	send_text(c, b.data, b.len);
	free(b.data);
}

static void notify_all(void)
{
	struct strbuf b = { 0 };

	build_status(&b);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed)
			send_text(&clients[i], b.data, b.len);
	free(b.data);
}

static bool known_game(const char *id)
{
	return valid_game_id(id) && find_game(id) >= 0;
}

static void handle_command(struct client *c, char *line)
{
	char *arg = strchr(line, '\t');
	char *arg2 = NULL;

	trim(line);
	if (arg) {
		*arg++ = '\0';
		arg2 = strchr(arg, '\t');
		if (arg2)
			*arg2++ = '\0';
	}

	if (!strcmp(line, "STATUS")) {
		send_status(c);
	} else if (!strcmp(line, "SUBSCRIBE")) {
		c->subscribed = true;
		send_status(c);
	} else if (!strcmp(line, "SCAN")) {
		load_apps();
		start_scan();
		reply(c, "OK\n");
		notify_all();
	} else if (!strcmp(line, "GAMES") && arg) {
		struct strbuf b = { 0 };

		if (build_games(&b, arg))
			send_text(c, b.data, b.len);
		else
			reply(c, "ERR scope\n");
		free(b.data);
	} else if (!strcmp(line, "RESOLVE") && arg) {
		/* Launch lookup for the Emulation Service: system and
		 * absolute content path of an available game. */
		long idx = valid_game_id(arg) ? find_game(arg) : -1;
		struct strbuf b = { 0 };

		if (idx < 0) {
			reply(c, "ERR game\n");
			return;
		}
		if (!games[idx].available) {
			reply(c, "ERR unavailable\n");
			return;
		}
		sb_append(&b, "OK %s\t%s/%s\n", systems[games[idx].sys].id,
			  ROMS_ROOT, games[idx].rel);
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!active_user[0] &&
		   (!strncmp(line, "SESSION_", 8) || !strncmp(line, "FAVORITE", 8) ||
		    !strncmp(line, "COLLECTION_", 11) ||
		    !strcmp(line, "STATS_RESET") || !strcmp(line, "DETAILS"))) {
		reply(c, "ERR no active user\n");
	} else if (!strcmp(line, "DETAILS") && arg) {
		long idx = valid_game_id(arg) ? find_game(arg) : -1;
		struct strbuf b = { 0 };

		if (idx < 0) {
			reply(c, "ERR game\n");
			return;
		}
		build_details(&b, &games[idx]);
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!strcmp(line, "STATS_RESET") && arg) {
		/* One game, or "all" of the active user's statistics (EPIC-020). */
		if (!strcmp(arg, "all")) {
			history_count = 0;
		} else if (known_game(arg)) {
			struct history_entry *h = history_for(arg, false);

			if (h) {
				size_t i = (size_t)(h - history);

				memmove(&history[i], &history[i + 1],
					(history_count - i - 1) * sizeof(*history));
				history_count--;
			}
		} else {
			reply(c, "ERR args\n");
			return;
		}
		reply(c, save_history() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "DELETE") && arg && arg2) {
		long idx = valid_game_id(arg) ? find_game(arg) : -1;
		const char *error;
		char text[48];

		if (idx < 0 || strcmp(arg2, "CONFIRM")) {
			reply(c, "ERR args\n");
			return;
		}
		if (session_game[0] && !strcmp(session_game, arg)) {
			reply(c, "ERR busy\n");
			return;
		}
		error = delete_game(idx);
		snprintf(text, sizeof(text), error ? "ERR %s\n" : "OK\n", error);
		reply(c, text);
		notify_all();
	} else if (!strcmp(line, "SESSION_BEGIN") && arg) {
		struct history_entry *h;

		if (!known_game(arg)) {
			reply(c, "ERR game\n");
			return;
		}
		h = history_for(arg, true);
		h->last = wall_now();
		h->sessions++;
		copy_text(session_game, sizeof(session_game), arg);
		clock_gettime(CLOCK_MONOTONIC, &session_start);
		reply(c, save_history() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "SESSION_END") && arg) {
		struct timespec now;
		long long elapsed;
		struct history_entry *h;

		if (!session_game[0] || strcmp(session_game, arg)) {
			reply(c, "ERR no session\n");
			return;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = (long long)(now.tv_sec - session_start.tv_sec);
		if (elapsed < 0)
			elapsed = 0;
		if (elapsed > MAX_SESSION_SEC)
			elapsed = MAX_SESSION_SEC;
		h = history_for(arg, true);
		h->time += elapsed;
		h->last = wall_now();
		session_game[0] = '\0';
		reply(c, save_history() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "SESSION_RECORD") && arg && arg2) {
		long long seconds = atoll(arg2);
		struct history_entry *h;

		if (!known_game(arg) || seconds < 0 || seconds > MAX_SESSION_SEC) {
			reply(c, "ERR args\n");
			return;
		}
		h = history_for(arg, true);
		h->last = wall_now();
		h->time += seconds;
		h->sessions++;
		reply(c, save_history() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "FAVORITE") && arg && arg2) {
		bool on = !strcmp(arg2, "1");

		if (!known_game(arg) || (!on && strcmp(arg2, "0"))) {
			reply(c, "ERR args\n");
			return;
		}
		if (on && !is_favorite(arg))
			id_list_add(&favorites, &favorite_count, arg);
		else if (!on)
			id_list_remove(favorites, &favorite_count, arg);
		reply(c, save_favorites() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "COLLECTION_CREATE") && arg) {
		struct collection *col;
		char out[48];
		static unsigned counter;

		if (!arg[0] || strlen(arg) >= sizeof(col->name) || !tsv_safe(arg)) {
			reply(c, "ERR name\n");
			return;
		}
		collections = xrealloc(collections, (collection_count + 1) * sizeof(*collections));
		col = &collections[collection_count++];
		memset(col, 0, sizeof(*col));
		snprintf(col->id, sizeof(col->id), "c%08llx",
			 (unsigned long long)((wall_now() << 4) + (counter++ & 15)) & 0xffffffffULL);
		copy_text(col->name, sizeof(col->name), arg);
		snprintf(out, sizeof(out), save_collections() ? "ERR persistence\n" : "OK %s\n", col->id);
		reply(c, out);
		notify_all();
	} else if ((!strcmp(line, "COLLECTION_ADD") || !strcmp(line, "COLLECTION_REMOVE")) &&
		   arg && arg2) {
		struct collection *col = collection_by_id(arg);

		if (!col || !known_game(arg2)) {
			reply(c, "ERR args\n");
			return;
		}
		if (line[11] == 'A' && !id_list_has(col->games, col->count, arg2))
			id_list_add(&col->games, &col->count, arg2);
		else if (line[11] == 'R')
			id_list_remove(col->games, &col->count, arg2);
		reply(c, save_collections() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "COLLECTION_RENAME") && arg && arg2) {
		struct collection *col = collection_by_id(arg);

		if (!col || !arg2[0] || !tsv_safe(arg2)) {
			reply(c, "ERR args\n");
			return;
		}
		copy_text(col->name, sizeof(col->name), arg2);
		reply(c, save_collections() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "COLLECTION_DELETE") && arg) {
		struct collection *col = collection_by_id(arg);
		size_t idx;

		if (!col) {
			reply(c, "ERR collection\n");
			return;
		}
		idx = (size_t)(col - collections);
		free(col->games);
		memmove(&collections[idx], &collections[idx + 1],
			(collection_count - idx - 1) * sizeof(*collections));
		collection_count--;
		reply(c, save_collections() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else {
		reply(c, "ERR unknown command\n");
	}
}

static void service_client(struct client *c)
{
	ssize_t n = recv(c->fd, c->buf + c->used, sizeof(c->buf) - c->used - 1, 0);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close_client(c);
		return;
	}
	c->used += (size_t)n;
	c->buf[c->used] = '\0';
	while (c->fd >= 0 && (nl = strchr(c->buf, '\n')) != NULL) {
		size_t len = (size_t)(nl - c->buf) + 1;

		*nl = '\0';
		handle_command(c, c->buf);
		if (c->fd < 0)
			return;
		memmove(c->buf, c->buf + len, c->used - len + 1);
		c->used -= len;
	}
	if (c->used >= sizeof(c->buf) - 1)
		close_client(c);
}

static int make_listener(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
		close(fd);
		return -1;
	}
	chmod(SOCKET_PATH, 0666);
	return fd;
}

static void accept_clients(int listen_fd)
{
	for (;;) {
		/* Client sockets stay blocking with a send timeout: a large GAMES
		 * reply is written whole, a stuck reader is dropped. */
		int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
		int slot = -1;

		if (fd < 0)
			return;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			close(fd);
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		clients[slot].fd = fd;
		clients[slot].subscribed = false;
		clients[slot].used = 0;
		/* The listener is non-blocking; keep accepting until EAGAIN. */
	}
}

/* ------------------------------------------------------------------ */
/* Main loop                                                          */
/* ------------------------------------------------------------------ */

static void finish_scan(void)
{
	char drain[16];
	struct scan_result *scan;

	while (read(scan_pipe[0], drain, sizeof(drain)) > 0)
		;
	pthread_join(scan_thread, NULL);
	scanning = false;
	scan = scan_output;
	scan_output = NULL;
	if (scan) {
		if (reconcile(scan))
			save_catalog();
		free_scan(scan);
	}
	if (rescan_pending) {
		rescan_pending = false;
		start_scan();
	}
	notify_all();
}

int main(void)
{
	struct sigaction sa;
	int listen_fd, inotify_fd, watch = -1;
	FILE *pf;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	mkdir_p(ACTIVE_DIR);
	load_registry();
	ensure_rom_dirs();
	load_catalog();
	load_apps();
	refresh_active_user();

	if (pipe2(scan_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		log_msg("pipe: %s", strerror(errno));
		return 1;
	}
	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0)
		watch = inotify_add_watch(inotify_fd, ACTIVE_DIR,
					  IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE);
	if (watch < 0)
		log_msg("active-user watch unavailable: %s", strerror(errno));

	listen_fd = make_listener();
	if (listen_fd < 0) {
		log_msg("listen %s: %s", SOCKET_PATH, strerror(errno));
		return 1;
	}
	pf = fopen(PIDFILE, "w");
	if (pf) {
		fprintf(pf, "%ld\n", (long)getpid());
		fclose(pf);
	}

	/* Existing catalog is served immediately; discovery reconciles it in
	 * the background (EPIC-012 non-blocking startup). */
	start_scan();

	while (running) {
		struct pollfd fds[3 + MAX_CLIENTS];
		int map[3 + MAX_CLIENTS];
		int n = 0;

		fds[n] = (struct pollfd){ listen_fd, POLLIN, 0 };
		map[n++] = -1;
		fds[n] = (struct pollfd){ scan_pipe[0], POLLIN, 0 };
		map[n++] = -2;
		if (inotify_fd >= 0) {
			fds[n] = (struct pollfd){ inotify_fd, POLLIN, 0 };
			map[n++] = -3;
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			fds[n] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
			map[n++] = i;
		}
		if (poll(fds, (nfds_t)n, -1) < 0) {
			if (errno == EINTR)
				continue;
			log_msg("poll: %s", strerror(errno));
			break;
		}
		for (int j = 0; j < n; j++) {
			if (!fds[j].revents)
				continue;
			if (map[j] == -1) {
				accept_clients(listen_fd);
			} else if (map[j] == -2) {
				finish_scan();
			} else if (map[j] == -3) {
				char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
				char before[40];

				while (read(inotify_fd, events, sizeof(events)) > 0)
					;
				copy_text(before, sizeof(before), active_user);
				refresh_active_user();
				if (strcmp(before, active_user))
					notify_all();
			} else if (fds[j].revents & (POLLHUP | POLLERR)) {
				close_client(&clients[map[j]]);
			} else {
				service_client(&clients[map[j]]);
			}
		}
	}

	if (scanning)
		pthread_join(scan_thread, NULL);
	for (int i = 0; i < MAX_CLIENTS; i++)
		close_client(&clients[i]);
	close(listen_fd);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
