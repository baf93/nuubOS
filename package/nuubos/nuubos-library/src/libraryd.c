/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
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
 *     play history (Last Played, Time Played, sessions), custom
 *     collections (the former favorites list is migrated into a collection
 *     named by the UI, MIGRATE_FAVORITES) and game tags (TAG_SET/TAG_CLEAR);
 *   - Find (SEARCH/FILTERS): title, system, tag, year, genre, players and
 *     never-played filters, random pick, over a metadata index rebuilt at
 *     every scan (the scraper requests one after writing metadata);
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
#include <ftw.h>
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
#ifndef STATE_ROOT
#define STATE_ROOT "/state"
#endif
#define SOCKET_PATH RUN_ROOT "/libraryd.sock"
#define DISC_SETS_DIR RUN_ROOT "/library-discs"
#define PIDFILE RUN_ROOT "/libraryd.pid"
#define REGISTRY_PATH SHARE_ROOT "/systems.conf"
#define ROMS_ROOT USERDATA_ROOT "/roms"
#define CATALOG_DIR USERDATA_ROOT "/library"
#define CATALOG_PATH CATALOG_DIR "/catalog.tsv"
#define USERDATA_USERS USERDATA_ROOT "/users"
/* This console's identity among the user's consoles (created once). */
#define CONSOLE_ID_FILE STATE_ROOT "/config/console-id"
/* Library view of the console (device setting): SHOW_UNAVAILABLE=1 lists
 * games whose file is gone or on a games card that is not inserted;
 * WRAP_ROWS=1 makes Home's rows go round from the last item to the first
 * (GitHub #28, on by default: user decision 2026-10-29). */
#define LIBRARY_CONFIG STATE_ROOT "/config/nuubos-library.conf"
#define ACTIVE_DIR RUN_ROOT "/user"
#define ACTIVE_FILE ACTIVE_DIR "/active"
#define UI_READY RUN_ROOT "/ui-ready"
/* Longest wait of the boot scan for nuubUI (main()). */
#define BOOT_SCAN_WAIT_MS 30000
#define SYSTEM_ICONS_DIR SHARE_ROOT "/systems"
#define COVERS_ROOT CATALOG_DIR "/covers"
#define METADATA_ROOT CATALOG_DIR "/metadata"
#define SYSTEM_APPS_DIR SHARE_ROOT "/applications"
#define USER_APPS_DIR USERDATA_ROOT "/applications"
/* Games card (docs/dev/platform.md "SD2 = games card"): nuubos-storagectl
 * publishes its ROM folder; its games are catalogued as "sd2:<path>".
 * ':' never occurs in a USERDATA path (exFAT forbids it). */
#define SD2_STATUS RUN_ROOT "/storage/sd2.status"
#define SD2_PREFIX "sd2:"
#define SD2_PREFIX_LEN 4

/* v2: paths relative to roms/, identity = system + file name + size,
 * absolute cover paths. Older catalogs are rebuilt by the first scan. */
#define CATALOG_HEADER "# nuubOS library catalog v2"
#define MAX_CLIENTS 16
#define MAX_LINE 1024
#define MAX_SYSTEMS 128
#define MAX_APPS 64
#define MAX_SCAN_DEPTH 6
#define RECENT_LIMIT 15
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
	/* Metadata index (EPIC-015 store), refreshed by every scan:
	 * release year (0 unknown), most players (0 unknown), genre. */
	int year;
	int players;
	char *genre;
};

/*
 * Play history of one game for the active user. last/time/sessions and
 * the reset fields are this console's own record (history.tsv); r_* add
 * up what the user's other consoles recorded (their files in
 * library/consoles/, synchronized by Syncthing when Play Activity is on).
 * A statistics reset cannot erase another console's file: it stores the
 * other consoles' totals as negative offsets and the reset time, which
 * every console applies (a game played before the latest reset is not
 * "recent" any more).
 */
struct history_entry {
	char id[17];
	long long last;
	long long time;
	int sessions;
	long long off_time;
	int off_sessions;
	long long reset;
	long long r_last, r_time, r_reset;
	int r_sessions;
};

struct collection {
	char id[16];
	char name[72];
	char (*games)[17];
	size_t count;
};

/*
 * Game tags, per user (tags.tsv: "<game id>\t<tag>,<tag>"). The status
 * tags are one at a time (setting one clears the others); the other tags
 * stand alone. Unknown names in the file are dropped. The order here is
 * the order of the game line field and of FILTERS.
 */
static const struct {
	const char *name;
	bool status;
} tag_defs[] = {
	{ "backlog", true },
	{ "playing", true },
	{ "completed", true },
	{ "mastered", true },
	{ "dropped", true },
	{ "broken", false },
};
#define TAG_COUNT (sizeof(tag_defs) / sizeof(tag_defs[0]))

struct game_tags {
	char id[17];
	unsigned mask;
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
	int year;
	int players;
	char *genre;
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
static char console_id[40];
static int inotify_fd = -1, consoles_wd = -1;
static size_t history_count;
static size_t history_cap;
static char (*favorites)[17];
static size_t favorite_count;
static struct collection *collections;
static size_t collection_count;
static struct game_tags *game_tags;
static size_t game_tag_count;

static struct app_def apps[MAX_APPS];
static int app_count;

static struct client clients[MAX_CLIENTS];

static int scan_pipe[2] = { -1, -1 };
static pthread_t scan_thread;
static bool scanning;
static bool rescan_pending;
/* The boot scan waits for UI_READY (watch ui_wd on RUN_ROOT, at the latest
 * at boot_scan_deadline_ms): main(). */
static bool boot_scan_pending;
static int ui_wd = -1;
static long long boot_scan_deadline_ms;
/* Unavailable games in the lists (off by default, user request 2026-10-20:
 * they cluttered the library). Search, counts and previews never had them. */
static bool show_unavailable;
static bool wrap_rows = true;

/* Games added or available again since the last SCAN request found idle. */
static size_t scan_new;
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

static bool rel_on_sd2(const char *rel)
{
	return !strncmp(rel, SD2_PREFIX, SD2_PREFIX_LEN);
}

/* File name part of a catalog path ("gba/x.gba", "sd2:x.gba" -> "x.gba"). */
static const char *rel_name(const char *rel)
{
	const char *base = strrchr(rel, '/');

	if (base)
		return base + 1;
	return rel_on_sd2(rel) ? rel + SD2_PREFIX_LEN : rel;
}

static const char *rom_name(const char *rel);

/*
 * Fast discovery identity (EPIC-012): system + file name + size. Moving a
 * ROM to another folder keeps its identity (and the users' history);
 * a renamed or changed file is a new game, never a false merge.
 */
static void game_id_for(int sys, const char *rel, long long size, char out[17])
{
	char key[600];

	snprintf(key, sizeof(key), "%s\x1f%lld", rom_name(rel), size);
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

/* ROM folder of the games card while it is mounted, else "". Written by
 * the main thread only while no scan runs (start_scan). */
static char sd2_roms[PATH_MAX];
/* The games card can take new games (mounted read-write). */
static bool sd2_writable;

/* Absolute path of a catalog path; false while its card is missing. */
static bool rom_path(char *out, size_t size, const char *rel)
{
	if (rel_on_sd2(rel))
		return sd2_roms[0] && pathf(out, size, "%s/%s", sd2_roms, rel + SD2_PREFIX_LEN);
	return pathf(out, size, ROMS_ROOT "/%s", rel);
}

/* The games card's ROM folder: a "roms" folder at its root in any case
 * (roms, ROMS, Roms), else the root itself (system folders at the root,
 * ArkOS layout); none while it is not mounted. */
static void refresh_sd2_root(void)
{
	char line[PATH_MAX];
	char mount[PATH_MAX] = "";
	char roms[PATH_MAX] = "";
	bool mounted = false;
	bool writable = false;
	FILE *fp = fopen(SD2_STATUS, "r");

	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (!strncmp(line, "SD2_STATE=", 10)) {
				mounted = !strcmp(line + 10, "ready") || !strcmp(line + 10, "readonly");
				writable = !strcmp(line + 10, "ready");
			}
			else if (!strncmp(line, "SD2_PATH=", 9))
				copy_text(mount, sizeof(mount), line + 9);
		}
		fclose(fp);
	}
	if (mounted && mount[0] == '/') {
		DIR *dir = opendir(mount);
		struct dirent *de;

		copy_text(roms, sizeof(roms), mount);
		while (dir && (de = readdir(dir)) != NULL) {
			char path[PATH_MAX];
			struct stat st;

			if (strcasecmp(de->d_name, "roms") || !pathf(path, sizeof(path), "%s/%s", mount, de->d_name) ||
			    stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
				continue;
			copy_text(roms, sizeof(roms), path);
			break;
		}
		if (dir)
			closedir(dir);
	}
	if (strcmp(roms, sd2_roms))
		log_msg("games card: %s", roms[0] ? roms : "none");
	copy_text(sd2_roms, sizeof(sd2_roms), roms);
	sd2_writable = writable && roms[0];
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

/*
 * Where the ROM's name starts in rel. A game in a folder of its own
 * (GitHub #30: PSP homebrew and PSN games, "<Folder>/EBOOT.PBP") is named
 * after the folder, unless the folder is a system's: every such game is
 * called EBOOT, and its covers, metadata and identity would collide.
 */
static const char *rom_name(const char *rel)
{
	const char *name = rel_name(rel);
	const char *start;
	char parent[NAME_MAX + 1];
	size_t len;

	if (strcasecmp(name, "EBOOT.PBP") || name == rel || name[-1] != '/')
		return name;
	start = name - 1;
	while (start > rel && start[-1] != '/' && start[-1] != ':')
		start--;
	len = (size_t)(name - 1 - start);
	if (!len || len >= sizeof(parent))
		return name;
	memcpy(parent, start, len);
	parent[len] = '\0';
	return system_by_folder(parent) < 0 ? start : name;
}

/* The ROM's name without extension: covers, metadata, scraper. */
static void rom_stem(const char *rel, char *out, size_t size)
{
	const char *name = rom_name(rel);
	const char *slash = strchr(name, '/');
	char *dot;

	if (slash) {
		snprintf(out, size, "%.*s", (int)(slash - name), name);
		return;
	}
	copy_text(out, size, name);
	dot = strrchr(out, '.');
	if (dot && dot != out)
		*dot = '\0';
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

/* Length of the registry extension of sys that name ends with (the longest,
 * without its dot), 0 if none. */
static size_t system_extension_len(int sys, const char *name)
{
	const char *p = systems[sys].exts + 1;
	size_t best = 0;

	while (*p) {
		const char *end = strchr(p, ',');
		size_t len = end ? (size_t)(end - p) : strlen(p);

		if (len > best && has_suffix(name, p, len))
			best = len;
		if (!end)
			break;
		p = end + 1;
	}
	return best;
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
 * derived identity only; metadata providers enrich it later (EPIC-015).
 * The whole registry extension goes, also one with several dots
 * ("Celeste.p8.png" -> "Celeste", not "Celeste.p8"). */
static char *title_from_rel(int sys, const char *rel)
{
	char stem[512];
	char out[512];
	size_t o = 0;
	int depth = 0;
	size_t ext_len;
	char *dot;

	copy_text(stem, sizeof(stem), rel_name(rel));
	ext_len = system_extension_len(sys, stem);
	if (rom_name(rel) != rel_name(rel)) {
		rom_stem(rel, stem, sizeof(stem));
	} else if (ext_len) {
		stem[strlen(stem) - ext_len - 1] = '\0';
	} else {
		dot = strrchr(stem, '.');
		if (dot && dot != stem)
			*dot = '\0';
	}
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
	if (rel_on_sd2(desc_rel))
		return name[0] && pathf(out, size, SD2_PREFIX "%s", name);
	return pathf(out, size, "%s", name) && out[0] != '\0';
}

/* Lines of a .m3u (discs) or .cue (FILE tracks), relative to roms/. */
static size_t descriptor_lines(const char *rel, char entries[][PATH_MAX], size_t max)
{
	char path[PATH_MAX];
	char line[1024];
	bool cue = has_suffix(rel, "cue", 3);
	bool m3u = has_suffix(rel, "m3u", 3);
	size_t n = 0;
	FILE *fp;

	if ((!cue && !m3u) || !rom_path(path, sizeof(path), rel))
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

/*
 * Per-disc playlists: some collections give every disc its own one-line
 * .m3u ("Game (Disc 1).m3u", "Game (Disc 2).m3u", ...), which would make
 * each disc a separate game without disc change. "(Disc 1)" (or "(CD1)")
 * whose "(Disc 2)" sibling exists in the same folder leads a disc set: the
 * one game, named and identified by the first disc's playlist (saves,
 * states, covers keep matching); the others are its files.
 */

static bool regular_file(const char *path);

/* Offset and length of the disc number in a playlist name, false if none
 * ("(Disc N", "(CD N", "(CDN", any case; the last one). */
static bool disc_number_at(const char *name, size_t *at, size_t *len)
{
	bool found = false;

	for (const char *p = strchr(name, '('); p; p = strchr(p + 1, '(')) {
		const char *q = p + 1;

		if (!strncasecmp(q, "disc", 4))
			q += 4;
		else if (!strncasecmp(q, "cd", 2))
			q += 2;
		else
			continue;
		while (*q == ' ')
			q++;
		if (!isdigit((unsigned char)*q))
			continue;
		*at = (size_t)(q - name);
		*len = strspn(q, "0123456789");
		found = true;
	}
	return found;
}

/* The other playlists of the disc set rel leads, in disc order; 0 when
 * rel leads none. */
static size_t disc_set_members(const char *rel, char members[][PATH_MAX], size_t max)
{
	static __thread char one[2][PATH_MAX];
	const char *name = rel_name(rel);
	size_t at, len, n = 0;

	if (!has_suffix(name, "m3u", 3) || !disc_number_at(name, &at, &len) ||
	    strtol(name + at, NULL, 10) != 1 || descriptor_lines(rel, one, 2) != 1)
		return 0;
	for (int disc = 2; n < max; disc++) {
		char path[PATH_MAX];

		if (!pathf(members[n], PATH_MAX, "%.*s%d%s", (int)(name - rel + at), rel, disc,
			   name + at + len) ||
		    !rom_path(path, sizeof(path), members[n]) || !regular_file(path) ||
		    descriptor_lines(members[n], one, 2) != 1)
			break;
		n++;
	}
	return n;
}

/* Entries of a .m3u (discs) or .cue (FILE tracks), relative to roms/; a
 * disc set's lead also owns its other playlists and their discs. */
static size_t descriptor_entries(const char *rel, char entries[][PATH_MAX], size_t max)
{
	static __thread char members[16][PATH_MAX];
	size_t n = descriptor_lines(rel, entries, max);
	size_t m = n ? disc_set_members(rel, members, 16) : 0;

	for (size_t i = 0; i < m && n < max; i++)
		n += descriptor_lines(members[i], entries + n, max - n);
	for (size_t i = 0; i < m && n < max; i++)
		memcpy(entries[n++], members[i], PATH_MAX);
	return n;
}

/* The playlist of all the discs of the set rel leads, written for the
 * launch as DISC_SETS_DIR/<rel's name> (same name: the emulator names saves
 * and states after it); false when rel leads no set or on error. */
static bool disc_set_playlist(const char *rel, char *out, size_t size)
{
	static char members[16][PATH_MAX];
	static char disc[1][PATH_MAX];
	char tmp[PATH_MAX];
	char path[PATH_MAX];
	size_t m = disc_set_members(rel, members, 16);
	bool ok;
	FILE *fp;

	if (!m || !pathf(out, size, DISC_SETS_DIR "/%s", rel_name(rel)) ||
	    !pathf(tmp, sizeof(tmp), "%s.tmp", out))
		return false;
	mkdir_p(DISC_SETS_DIR);
	fp = fopen(tmp, "w");
	if (!fp)
		return false;
	ok = descriptor_lines(rel, disc, 1) == 1 && rom_path(path, sizeof(path), disc[0]) &&
	     fprintf(fp, "%s\n", path) > 0;
	for (size_t i = 0; ok && i < m; i++)
		ok = descriptor_lines(members[i], disc, 1) == 1 &&
		     rom_path(path, sizeof(path), disc[0]) && fprintf(fp, "%s\n", path) > 0;
	if (fclose(fp) != 0 || !ok || rename(tmp, out) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
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
		if (sniff_depth < 2 && descriptor_lines(rel, first, 1) == 1) {
			sniff_depth++;
			sys = sniff_system(first[0]);
			sniff_depth--;
		}
		return sys;
	}
	if (!rom_path(path, sizeof(path), rel))
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

/*
 * PortMaster ports (EPIC-058): the scripts directly in roms/ports of the
 * console's own card, or of the SD2 card when the user chose it on the
 * PortMaster page (GitHub #21, portsd's /state/ports/location.conf): the
 * only folder nuubos-portsd's sandbox sees. The folder of a port beside its
 * script holds the port's data (archives, images, its own runtimes), never
 * games, so the scan does not enter it.
 */
#define PORTS_SYSTEM "ports"
static bool regular_file(const char *path);
#define PORTS_LOCATION_FILE STATE_ROOT "/ports/location.conf"
/* The ports folder (portsd asks for it in STATUS), "" while it is on an
 * SD2 card that is out, and its catalog folder ("ports" or "sd2:<name>").
 * Written by the main thread only while no scan runs, as sd2_roms. */
static char ports_root[PATH_MAX];
static char ports_rel[PATH_MAX] = "ports";
static bool ports_sd2;
/* The SD2 card's ports folder whatever the setting ("" without the card):
 * where the PortMaster page's switch moves them. */
static char sd2_ports[PATH_MAX];

static void refresh_ports_root(void)
{
	char line[64], name[NAME_MAX + 1] = "ports";
	FILE *fp = fopen(PORTS_LOCATION_FILE, "r");

	ports_sd2 = false;
	sd2_ports[0] = '\0';
	if (fp) {
		while (fgets(line, sizeof(line), fp))
			if (!strncmp(line, "LOCATION=sd2", 12))
				ports_sd2 = true;
		fclose(fp);
	}
	ports_root[0] = '\0';
	if (sd2_roms[0]) {
		/* Its own spelling on the card (ports, PORTS, Ports). */
		DIR *dir = opendir(sd2_roms);
		struct dirent *de;

		while (dir && (de = readdir(dir)) != NULL)
			if (!strcasecmp(de->d_name, "ports")) {
				copy_text(name, sizeof(name), de->d_name);
				break;
			}
		if (dir)
			closedir(dir);
		pathf(sd2_ports, sizeof(sd2_ports), "%s/%s", sd2_roms, name);
	}
	if (!ports_sd2) {
		copy_text(ports_root, sizeof(ports_root), ROMS_ROOT "/ports");
		copy_text(ports_rel, sizeof(ports_rel), "ports");
		return;
	}
	copy_text(ports_root, sizeof(ports_root), sd2_ports);
	pathf(ports_rel, sizeof(ports_rel), SD2_PREFIX "%s", name);
}

static bool is_ports(int sys)
{
	return sys >= 0 && !strcmp(systems[sys].id, PORTS_SYSTEM);
}

/* What PortMaster's gameinfo.xml (EmulationStation gamelist) says of one
 * port script. */
struct port_info {
	char *script;
	char *image;
	char *desc;
	char *developer;
	char *genre;
	int year;
};

/* Text of <tag> in one <game> block, entities decoded and on one line;
 * NULL when absent or empty. */
static char *xml_text(const char *block, const char *end, const char *tag)
{
	char open_tag[32], close_tag[32];
	const char *a, *b;
	char *out;
	size_t o = 0;

	snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
	snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
	a = strstr(block, open_tag);
	if (!a || a >= end)
		return NULL;
	a += strlen(open_tag);
	b = strstr(a, close_tag);
	if (!b || b > end || b == a)
		return NULL;
	out = xrealloc(NULL, (size_t)(b - a) + 1);
	while (a < b) {
		static const char *const ents[][2] = {
			{ "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" },
		};
		bool done = false;

		for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]) && !done; i++) {
			size_t n = strlen(ents[i][0]);

			if ((size_t)(b - a) >= n && !strncmp(a, ents[i][0], n)) {
				out[o++] = ents[i][1][0];
				a += n;
				done = true;
			}
		}
		if (done)
			continue;
		if (a[0] == '&' && a[1] == '#') {
			char *q;
			long c = strtol(a + 2, &q, 10);

			if (q < b && *q == ';' && c >= 32 && c < 127) {
				out[o++] = (char)c;
				a = q + 1;
				continue;
			}
		}
		/* One line, tab-free: the catalog and the details are TSV. */
		out[o++] = (*a == '\n' || *a == '\r' || *a == '\t') ? ' ' : *a;
		a++;
	}
	out[o] = '\0';
	trim(out);
	if (!out[0]) {
		free(out);
		return NULL;
	}
	return out;
}

/* Every gameinfo.xml in the ports' folders (roms/ports/<port>/), and
 * roms/ports/gamelist.xml when one is there. */
static struct port_info *load_port_infos(size_t *count)
{
	struct port_info *list = NULL;
	size_t cap = 0;
	DIR *dir = ports_root[0] ? opendir(ports_root) : NULL;
	struct dirent *de;

	*count = 0;
	if (!dir)
		return NULL;
	while ((de = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		static char text[1 << 16];
		size_t len;
		const char *p;
		FILE *fp;

		if (de->d_name[0] == '.')
			continue;
		if (!pathf(path, sizeof(path), !strcmp(de->d_name, "gamelist.xml") ? "%s/gamelist.xml" : "%s/%s/gameinfo.xml",
			   ports_root, de->d_name))
			continue;
		if (!(fp = fopen(path, "r")))
			continue;
		len = fread(text, 1, sizeof(text) - 1, fp);
		fclose(fp);
		text[len] = '\0';
		for (p = strstr(text, "<game>"); p; p = strstr(p, "<game>")) {
			const char *end = strstr(p, "</game>");
			char *script, *image, *release;
			struct port_info info = { 0 };

			if (!end)
				break;
			script = xml_text(p, end, "path");
			if (script && !strncmp(script, "./", 2) && !strchr(script + 2, '/') && tsv_safe(script + 2)) {
				info.script = xstrdup(script + 2);
				/* Image paths are relative to roms/ports, inside it. */
				image = xml_text(p, end, "image");
				if (image && !strncmp(image, "./", 2) && !strstr(image, "..") && tsv_safe(image)) {
					char full[PATH_MAX];

					if (pathf(full, sizeof(full), "%s/%s", ports_root, image + 2) && regular_file(full))
						info.image = xstrdup(full);
				}
				free(image);
				info.desc = xml_text(p, end, "desc");
				info.developer = xml_text(p, end, "developer");
				info.genre = xml_text(p, end, "genre");
				release = xml_text(p, end, "releasedate");
				if (release && strlen(release) >= 4) {
					/* "20200423T000000" */
					char four[5];
					int y;

					memcpy(four, release, 4);
					four[4] = '\0';
					y = atoi(four);
					if (y > 1950 && y < 2100)
						info.year = y;
				}
				free(release);
				if (*count == cap) {
					cap = cap ? cap * 2 : 16;
					list = xrealloc(list, cap * sizeof(*list));
				}
				list[(*count)++] = info;
			}
			free(script);
			p = end;
		}
	}
	closedir(dir);
	return list;
}

static void free_port_infos(struct port_info *list, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		free(list[i].script);
		free(list[i].image);
		free(list[i].desc);
		free(list[i].developer);
		free(list[i].genre);
	}
	free(list);
}

static const struct port_info *port_info_for(const struct port_info *list, size_t count, const char *rel)
{
	const char *name = rel_name(rel);

	for (size_t i = 0; i < count; i++)
		if (!strcasecmp(list[i].script, name))
			return &list[i];
	return NULL;
}

static bool skip_directory(const char *name)
{
	static const char *const skipped[] = {
		"media", "images", "videos", "manuals", "covers", "boxart",
		"snap", "snaps", "screenshots", "bios", "music", "nuubos-backup", NULL
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
	const char *name = rom_name(rel);
	char stem[512];
	char cover_rel[PATH_MAX];
	char path[PATH_MAX];

	rom_stem(rel, stem, sizeof(stem));
	for (int i = 0; exts[i]; i++)
		if (pathf(path, sizeof(path), COVERS_ROOT "/%s/%s.%s", systems[sys].id, stem, exts[i]) &&
		    regular_file(path))
			return xstrdup(path);
	for (int i = 0; exts[i]; i++)
		if (pathf(cover_rel, sizeof(cover_rel), "%.*smedia/covers/%s.%s",
			  (int)(name - rel), rel, stem, exts[i]) &&
		    rom_path(path, sizeof(path), cover_rel) &&
		    regular_file(path))
			return xstrdup(path);
	return NULL;
}

/*
 * The system of a file: its nearest folder named after a system when that
 * system takes the file, else the only system taking its extension, else a
 * header sniff for shared disc/cartridge formats. A file in a system's
 * folder that the system does not take is none: the data of a game in
 * another format (GitHub #31: the .col stages of a PSP port were listed as
 * ColecoVision games).
 */
static int classify(const char *rel, const char *name, int folder_sys)
{
	int sys;

	if (folder_sys >= 0)
		return system_accepts(folder_sys, name) ? folder_sys : -1;
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

	if (!(sub[0] ? rom_path(path, sizeof(path), sub) : pathf(path, sizeof(path), ROMS_ROOT)))
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
		/* "" and "sd2:" are the roots of the two cards. */
		if (!pathf(rel, sizeof(rel), "%s%s%s", sub,
			   sub[0] && strcmp(sub, SD2_PREFIX) ? "/" : "", de->d_name) ||
		    !rom_path(full, sizeof(full), rel) ||
		    stat(full, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			int here;

			if (depth >= MAX_SCAN_DEPTH || skip_directory(de->d_name) || is_ports(folder_sys))
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
		if (is_ports(sys) && strcasecmp(sub, ports_rel))
			continue; /* a script anywhere else is not a port */
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
			.year = 0,
			.players = 0,
			.genre = NULL,
		};
	}
	closedir(dir);
}

/* Year, players and genre of a game's metadata file (scan thread). */
static void read_meta_index(struct scan_item *it)
{
	char stem[512];
	char path[PATH_MAX];
	char line[1024];
	FILE *fp;

	rom_stem(it->rel, stem, sizeof(stem));
	if (!pathf(path, sizeof(path), METADATA_ROOT "/%s/%s.txt", systems[it->sys].id, stem))
		return;
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "release=", 8)) {
			/* "1994", "1994-11-21", "19941121T000000" */
			char four[5];
			int y;

			memcpy(four, line + 8, 4);
			four[4] = '\0';
			y = atoi(four);
			if (y > 1950 && y < 2100)
				it->year = y;
		} else if (!strncmp(line, "players=", 8)) {
			/* "1", "1-4", "2" -> the largest number */
			int most = 0;

			for (const char *q = line + 8; *q; q++)
				if (isdigit((unsigned char)*q)) {
					int v = atoi(q);

					if (v > most && v < 100)
						most = v;
					while (isdigit((unsigned char)q[1]))
						q++;
				}
			it->players = most;
		} else if (!strncmp(line, "genre=", 6) && line[6] && tsv_safe(line + 6)) {
			free(it->genre);
			it->genre = xstrdup(line + 6);
		}
	}
	fclose(fp);
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
	size_t port_count = 0;
	struct port_info *ports = NULL;

	walk(&ctx, "", 0, -1);
	if (sd2_roms[0])
		walk(&ctx, SD2_PREFIX, 0, -1);
	for (size_t i = 0; i < ctx.out->count && !ports; i++)
		if (is_ports(ctx.out->items[i].sys))
			ports = load_port_infos(&port_count);

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
		read_meta_index(it);
		if (is_ports(it->sys)) {
			const struct port_info *info = port_info_for(ports, port_count, it->rel);

			/* A scraped or user cover first, then PortMaster's image. */
			if (info && !it->cover && info->image)
				it->cover = xstrdup(info->image);
			if (info && !it->year)
				it->year = info->year;
			if (info && !it->genre && info->genre)
				it->genre = xstrdup(info->genre);
		}
		ctx.out->items[keep++] = *it;
	}
	ctx.out->count = keep;
	free_port_infos(ports, port_count);
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
		log_msg("scan: %zu files no system takes there were skipped (shared format outside a system folder, or another system's file in one)",
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

/* Netplay (nuubos-emud): the available game whose ROM file is named name
 * (case-insensitive), else one whose file name without extension is the
 * same; -1 when none. A games-card game counts like the others. */
static long match_game(const char *name)
{
	const char *dot = strrchr(name, '.');
	size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
	long same_stem = -1;

	for (size_t i = 0; i < game_count; i++) {
		const char *file;
		const char *fdot;

		if (!games[i].available || !games[i].rel)
			continue;
		file = strrchr(games[i].rel, '/');
		file = file ? file + 1 : games[i].rel;
		if (!strcasecmp(file, name))
			return (long)i;
		fdot = strrchr(file, '.');
		if (same_stem < 0 && fdot && (size_t)(fdot - file) == stem &&
		    !strncasecmp(file, name, stem))
			same_stem = (long)i;
	}
	return same_stem;
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
		/* Derived from the file name only, so a rule change also
		 * reaches games catalogued before it. */
		g->title = title_from_rel(sys, g->rel);
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
static bool referenced_elsewhere(const char *rel, const struct game *self);

/* A file in a system's folder that this system does not take (classify):
 * an entry of an older scan that is no game (GitHub #31). */
static bool outside_its_folder(const char *rel)
{
	char dir[PATH_MAX];
	const char *name;
	char *save = NULL;
	int folder_sys = -1;

	if (!strncmp(rel, SD2_PREFIX, strlen(SD2_PREFIX)))
		rel += strlen(SD2_PREFIX);
	name = strrchr(rel, '/');
	if (!name || (size_t)(name - rel) >= sizeof(dir))
		return false;
	memcpy(dir, rel, (size_t)(name - rel));
	dir[name - rel] = '\0';
	for (char *part = strtok_r(dir, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
		int here = system_by_folder(part);

		if (here >= 0)
			folder_sys = here;
	}
	return folder_sys >= 0 && !system_accepts(folder_sys, name + 1);
}

static bool reconcile(struct scan_result *scan)
{
	size_t added = 0, back = 0, gone = 0, changed = 0, duplicates = 0, absorbed = 0;
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
			g->title = title_from_rel(it->sys, it->rel);
			g->cover = it->cover;
			g->size = it->size;
			g->mtime = it->mtime;
			g->added = now;
			g->available = true;
			g->seen = true;
			g->year = it->year;
			g->players = it->players;
			g->genre = it->genre;
			it->rel = NULL;
			it->cover = NULL;
			it->genre = NULL;
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
		/* Metadata is not catalog state: refreshed silently. */
		g->year = it->year;
		g->players = it->players;
		free(g->genre);
		g->genre = it->genre;
		it->genre = NULL;
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
	for (size_t i = 0; i < game_count;) {
		struct game *g = &games[i];
		char path[PATH_MAX];

		/* Its file is still there but now belongs to another game (a
		 * disc of a set, a file a playlist lists) or to no system: the
		 * entry goes rather than staying unavailable. */
		if (!g->seen && rom_path(path, sizeof(path), g->rel) && regular_file(path) &&
		    (referenced_elsewhere(g->rel, g) || outside_its_folder(g->rel))) {
			free(g->rel);
			free(g->title);
			free(g->cover);
			free(g->genre);
			memmove(g, g + 1, (game_count - i - 1) * sizeof(*games));
			game_count--;
			absorbed++;
			continue;
		}
		if (!g->seen && g->available) {
			g->available = false;
			gone++;
		}
		i++;
	}
	log_msg("scan: %zu found, %zu added, %zu changed, %zu available again, %zu unavailable, %zu absorbed, %zu duplicates",
		scan->count, added, changed, back, gone, absorbed, duplicates);
	scan_new += added + back;
	return added || changed || back || gone || absorbed;
}

static void free_scan(struct scan_result *scan)
{
	for (size_t i = 0; i < scan->count; i++) {
		free(scan->items[i].rel);
		free(scan->items[i].cover);
		free(scan->items[i].genre);
	}
	free(scan->items);
	free(scan);
}

static void start_scan(void)
{
	boot_scan_pending = false;
	if (scanning) {
		rescan_pending = true;
		return;
	}
	refresh_sd2_root();
	refresh_ports_root();
	if (pthread_create(&scan_thread, NULL, scan_main, NULL) != 0) {
		log_msg("scan thread failed: %s", strerror(errno));
		return;
	}
	scanning = true;
	log_msg("scan started");
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* nuubUI is ready (or the wait timed out): the boot scan may run. */
static void end_boot_wait(void)
{
	if (ui_wd >= 0)
		inotify_rm_watch(inotify_fd, ui_wd);
	ui_wd = -1;
	if (boot_scan_pending)
		start_scan();
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
	free(game_tags);
	game_tags = NULL;
	game_tag_count = 0;
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

static int tag_by_name(const char *name)
{
	for (size_t i = 0; i < TAG_COUNT; i++)
		if (!strcmp(tag_defs[i].name, name))
			return (int)i;
	return -1;
}

static unsigned tags_of(const char *id)
{
	for (size_t i = 0; i < game_tag_count; i++)
		if (!strcmp(game_tags[i].id, id))
			return game_tags[i].mask;
	return 0;
}

/* A status tag replaces the game's other status tag. */
static void set_tag(const char *id, int tag, bool on)
{
	size_t i;

	for (i = 0; i < game_tag_count; i++)
		if (!strcmp(game_tags[i].id, id))
			break;
	if (i == game_tag_count) {
		if (!on)
			return;
		game_tags = xrealloc(game_tags, (game_tag_count + 1) * sizeof(*game_tags));
		memset(&game_tags[i], 0, sizeof(game_tags[i]));
		copy_text(game_tags[i].id, sizeof(game_tags[i].id), id);
		game_tag_count++;
	}
	if (on && tag_defs[tag].status)
		for (size_t t = 0; t < TAG_COUNT; t++)
			if (tag_defs[t].status)
				game_tags[i].mask &= ~(1u << t);
	if (on)
		game_tags[i].mask |= 1u << tag;
	else
		game_tags[i].mask &= ~(1u << tag);
	if (!game_tags[i].mask) {
		memmove(&game_tags[i], &game_tags[i + 1], (game_tag_count - i - 1) * sizeof(*game_tags));
		game_tag_count--;
	}
}

/* "completed,broken" (empty when none). */
static void tag_list(unsigned mask, char *out, size_t size)
{
	size_t len = 0;

	out[0] = '\0';
	for (size_t t = 0; t < TAG_COUNT && len < size; t++)
		if (mask & (1u << t))
			len += (size_t)snprintf(out + len, size - len, "%s%s", len ? "," : "", tag_defs[t].name);
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

/* The combined history of all the user's consoles. */
static long long hist_reset(const struct history_entry *h)
{
	return h->reset > h->r_reset ? h->reset : h->r_reset;
}

static long long hist_last(const struct history_entry *h)
{
	long long last = h->last > h->r_last ? h->last : h->r_last;

	return last > hist_reset(h) ? last : 0;
}

static long long hist_time(const struct history_entry *h)
{
	long long t = h->time + h->off_time + h->r_time;

	return t > 0 ? t : 0;
}

static int hist_sessions(const struct history_entry *h)
{
	int n = h->sessions + h->off_sessions + h->r_sessions;

	return n > 0 ? n : 0;
}

/* Statistics to zero on every console of the user: this console's own
 * record cleared, the others' totals cancelled by offsets. */
static void reset_history(struct history_entry *h)
{
	h->last = h->time = 0;
	h->sessions = 0;
	h->off_time = -h->r_time;
	h->off_sessions = -h->r_sessions;
	h->reset = (long long)time(NULL);
}

/* "id last time sessions [off_time off_sessions reset]" */
static bool parse_history_line(const char *line, char *id, long long *v)
{
	long long sessions = 0, off_sessions = 0;
	int n;

	memset(v, 0, 6 * sizeof(*v));
	if (line[0] == '#')
		return false;
	n = sscanf(line, "%31s %lld %lld %lld %lld %lld %lld", id, &v[0], &v[1], &sessions, &v[3],
		   &off_sessions, &v[5]);
	v[2] = sessions;
	v[4] = off_sessions;
	return (n == 4 || n == 7) && valid_game_id(id);
}

static void load_console_id(void)
{
	char text[64] = "";
	FILE *fp = fopen(CONSOLE_ID_FILE, "r");

	if (fp) {
		if (fgets(text, sizeof(text), fp))
			trim(text);
		fclose(fp);
	}
	if (strlen(text) < 8 || strspn(text, "0123456789abcdef-") != strlen(text)) {
		fp = fopen("/proc/sys/kernel/random/uuid", "r");
		text[0] = '\0';
		if (fp) {
			if (fgets(text, sizeof(text), fp))
				trim(text);
			fclose(fp);
		}
		if (!text[0])
			snprintf(text, sizeof(text), "%08lx-%08lx", (unsigned long)time(NULL), (unsigned long)getpid());
		mkdir_p(STATE_ROOT "/config");
		if (write_atomic(CONSOLE_ID_FILE, text, strlen(text)) != 0)
			log_msg("console id not saved: %s", strerror(errno));
	}
	copy_text(console_id, sizeof(console_id), text);
}

static void load_library_config(void)
{
	char line[128];
	FILE *fp = fopen(LIBRARY_CONFIG, "r");

	show_unavailable = false;
	wrap_rows = true;
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, "SHOW_UNAVAILABLE=", 17))
			show_unavailable = atoi(line + 17) != 0;
		else if (!strncmp(line, "WRAP_ROWS=", 10))
			wrap_rows = atoi(line + 10) != 0;
	}
	fclose(fp);
}

static int save_library_config(void)
{
	char text[64];

	mkdir_p(STATE_ROOT "/config");
	snprintf(text, sizeof(text), "SHOW_UNAVAILABLE=%d\nWRAP_ROWS=%d\n", show_unavailable ? 1 : 0,
		 wrap_rows ? 1 : 0);
	return write_atomic(LIBRARY_CONFIG, text, strlen(text));
}

/* A game the lists show: available, or any while unavailable ones are on. */
static bool listed(const struct game *g)
{
	return g->available || show_unavailable;
}

/* The other consoles' files: r_* of every game, from scratch. */
static void load_remote_history(void)
{
	char dir[PATH_MAX], own[96];
	DIR *d;
	struct dirent *e;

	for (size_t i = 0; i < history_count; i++)
		history[i].r_last = history[i].r_time = history[i].r_reset = history[i].r_sessions = 0;
	if (!active_user[0])
		return;
	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/library/consoles", active_user);
	snprintf(own, sizeof(own), "%s.tsv", console_id);
	if (!(d = opendir(dir)))
		return;
	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name);
		char path[PATH_MAX + 260], line[256];
		FILE *fp;

		/* Only finished files: no dot files (Syncthing's temporary
		 * names), no .sync-conflict copies, not this console. */
		if (e->d_name[0] == '.' || len < 5 || strcmp(e->d_name + len - 4, ".tsv") ||
		    strstr(e->d_name, ".sync-conflict") || !strcmp(e->d_name, own))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (!(fp = fopen(path, "r")))
			continue;
		while (fgets(line, sizeof(line), fp)) {
			char id[32];
			long long v[6];
			struct history_entry *h;

			if (!parse_history_line(line, id, v))
				continue;
			h = history_for(id, true);
			if (v[0] > h->r_last)
				h->r_last = v[0];
			h->r_time += v[1] + v[3];
			h->r_sessions += (int)(v[2] + v[4]);
			if (v[5] > h->r_reset)
				h->r_reset = v[5];
		}
		fclose(fp);
	}
	closedir(d);
}

/* Follow library/consoles of the active user (created here). */
static void watch_consoles(void)
{
	char dir[PATH_MAX];

	if (inotify_fd < 0)
		return;
	if (consoles_wd >= 0)
		inotify_rm_watch(inotify_fd, consoles_wd);
	consoles_wd = -1;
	if (!active_user[0])
		return;
	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/library/consoles", active_user);
	mkdir_p(dir);
	consoles_wd = inotify_add_watch(inotify_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM);
}

static int save_history(void);

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
			long long v[6];
			struct history_entry *h;

			if (!parse_history_line(line, id, v))
				continue;
			h = history_for(id, true);
			h->last = v[0];
			h->time = v[1] < 0 ? 0 : v[1];
			h->sessions = v[2] < 0 ? 0 : (int)v[2];
			h->off_time = v[3];
			h->off_sessions = (int)v[4];
			h->reset = v[5];
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
	user_file("tags.tsv", path, sizeof(path));
	if ((fp = fopen(path, "r")) != NULL) {
		while (fgets(line, sizeof(line), fp)) {
			char *names, *save = NULL;

			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] == '#' || !(names = strchr(line, '\t')))
				continue;
			*names++ = '\0';
			if (!valid_game_id(line))
				continue;
			for (char *tok = strtok_r(names, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
				int t = tag_by_name(tok);

				if (t >= 0)
					set_tag(line, t, true);
			}
		}
		fclose(fp);
	}
	load_remote_history();
	watch_consoles();
	/* History from before the export existed reaches the other consoles. */
	{
		char own[PATH_MAX + 64];

		snprintf(own, sizeof(own), USERDATA_USERS "/%s/library/consoles/%s.tsv", active_user, console_id);
		if (console_id[0] && history_count && access(own, F_OK) != 0 && save_history() != 0)
			log_msg("history export failed");
	}
	log_msg("user %s: %zu history, %zu favorites, %zu collections, %zu tagged",
		active_user, history_count, favorite_count, collection_count, game_tag_count);
}

static int save_user_file(const char *name, struct strbuf *b)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];
	int rc;

	if (!active_user[0])
		return -1;
	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/library%s", active_user,
		 strchr(name, '/') ? "/consoles" : "");
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

	char name[96];
	int rc;

	sb_append(&b, "# game-id last-played time-played sessions off-time off-sessions reset\n");
	for (size_t i = 0; i < history_count; i++) {
		const struct history_entry *h = &history[i];

		/* Games only another console played: nothing of our own. */
		if (!h->last && !h->time && !h->sessions && !h->off_time && !h->off_sessions && !h->reset)
			continue;
		sb_append(&b, "%s %lld %lld %d %lld %d %lld\n", h->id, h->last, h->time, h->sessions,
			  h->off_time, h->off_sessions, h->reset);
	}
	/* The same record for the user's other consoles (Play Activity). */
	{
		struct strbuf copy = { 0 };

		if (b.data)
			sb_append(&copy, "%s", b.data);
		snprintf(name, sizeof(name), "consoles/%s.tsv", console_id);
		if (console_id[0] && save_user_file(name, &copy))
			log_msg("history export failed");
	}
	rc = save_user_file("history.tsv", &b);
	return rc;
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

static int save_tags(void)
{
	struct strbuf b = { 0 };
	char names[96];

	sb_append(&b, "# game-id tags\n");
	for (size_t i = 0; i < game_tag_count; i++) {
		tag_list(game_tags[i].mask, names, sizeof(names));
		sb_append(&b, "%s\t%s\n", game_tags[i].id, names);
	}
	return save_user_file("tags.tsv", &b);
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
	char tags[96];

	tag_list(tags_of(g->id), tags, sizeof(tags));
	sb_append(b, "%s=%s\t%s\t%s\t", key, g->id, s->id, g->title);
	sb_append(b, "%s", g->cover ? g->cover : "");
	/* Field 9 (the former favorite flag): where the game is, 0 = SD1, 2 = SD2.
	 * Field 13: the user's tags of the game ("completed,broken"). */
	sb_append(b, "\t%d\t%lld\t%lld\t%d\t%d\t%d\t%d\t%s\t%s\n", s->aspect,
		  h ? hist_last(h) : 0LL, h ? hist_time(h) : 0LL,
		  g->available ? 1 : 0, rel_on_sd2(g->rel) ? 2 : 0, g->year, g->players,
		  g->genre ? g->genre : "", tags);
}

static int history_compare(const void *a, const void *b)
{
	const struct history_entry *x = *(const struct history_entry *const *)a;
	const struct history_entry *y = *(const struct history_entry *const *)b;

	long long a_last = hist_last(x), b_last = hist_last(y);

	return (a_last < b_last) - (a_last > b_last);
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
		if (hist_last(&history[i]) > 0)
			order[n++] = &history[i];
	qsort(order, n, sizeof(*order), history_compare);
	for (size_t i = 0; i < n && (!limit || shown < limit); i++) {
		long idx = find_game(order[i]->id);

		if (idx < 0 || !listed(&games[idx]))
			continue;
		append_game(b, key, &games[idx]);
		shown++;
	}
	free(order);
}

/*
 * Cover previews of a system (sys >= 0) or a collection: up to PREVIEW_COUNT
 * covers of available games, most recently played first, then by title
 * order of the catalog. Appended as extra tab fields, line ended here.
 */
#define PREVIEW_COUNT 4
static void append_previews(struct strbuf *b, int sys, const struct collection *col)
{
	const struct game *pick[PREVIEW_COUNT] = { NULL };
	long long when[PREVIEW_COUNT] = { 0 };
	size_t n = 0;

	for (size_t i = 0; i < game_count; i++) {
		const struct game *g = &games[i];
		const struct history_entry *h;
		long long last;
		size_t at;

		if (!g->available || !g->cover)
			continue;
		if (sys >= 0 ? g->sys != sys : !id_list_has(col->games, col->count, g->id))
			continue;
		h = active_user[0] ? history_for(g->id, false) : NULL;
		last = h ? hist_last(h) : 0;
		/* Insertion into the short list sorted by last played. */
		at = n;
		while (at > 0 && when[at - 1] < last)
			at--;
		if (at >= PREVIEW_COUNT)
			continue;
		if (n < PREVIEW_COUNT)
			n++;
		for (size_t k = n - 1; k > at; k--) {
			pick[k] = pick[k - 1];
			when[k] = when[k - 1];
		}
		pick[at] = g;
		when[at] = last;
	}
	for (size_t k = 0; k < n; k++)
		sb_append(b, "\t%s", pick[k]->cover);
	sb_append(b, "\n");
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
	sb_append(b, "user=%s\nscanning=%d\ngames=%zu\nscan_new=%zu\nshow_unavailable=%d\nwrap_rows=%d\n",
		  active_user, scanning ? 1 : 0, available, scan_new, show_unavailable ? 1 : 0, wrap_rows ? 1 : 0);
	/* Where new games go (Web administration uploads): the games card
	 * while it can be written, else USERDATA. */
	sb_append(b, "games_root=%s\n", sd2_writable ? sd2_roms : ROMS_ROOT);
	/* The ports folder nuubos-portsd runs (GitHub #21): "" while the SD2
	 * card it is on is out. */
	sb_append(b, "ports_location=%s\nports_root=%s\n", ports_sd2 ? "sd2" : "console", ports_root);
	if (active_user[0]) {
		append_recent(b, "recent", RECENT_LIMIT);
		/* Favorites of an older nuubOS wait for the UI to name the
		 * collection they become (MIGRATE_FAVORITES). */
		if (favorite_count)
			sb_append(b, "favorites_pending=%zu\n", favorite_count);
		for (size_t i = 0; i < collection_count; i++) {
			size_t shown = 0;

			/* Members the collection's page lists. */
			for (size_t k = 0; k < collections[i].count; k++) {
				long idx = find_game(collections[i].games[k]);

				if (idx >= 0 && listed(&games[idx]))
					shown++;
			}
			sb_append(b, "collection=%s\t%s\t%zu", collections[i].id,
				  collections[i].name, shown);
			append_previews(b, -1, &collections[i]);
		}
	}
	for (int i = 0; i < system_count; i++)
		if (per_system[i] > 0) {
			sb_append(b, "system=%s\t%s\t%d\t%d\t%s\t%s", systems[i].id,
				  systems[i].name, per_system[i], systems[i].aspect,
				  systems[i].color, systems[i].icon);
			append_previews(b, i, NULL);
		}
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
	return g->sys == *(const int *)arg && listed(g);
}


static bool keep_collection(const struct game *g, const void *arg)
{
	const struct collection *c = arg;

	return listed(g) && id_list_has(c->games, c->count, g->id);
}

/* GAMES <scope>: system:<id> | collection:<id> | recent */
static bool build_games(struct strbuf *b, const char *scope)
{
	const struct game **list = NULL;
	size_t n = 0;

	if (!strncmp(scope, "system:", 7)) {
		int sys = system_by_id(scope + 7);

		if (sys < 0)
			return false;
		list = sorted_games(keep_system, &sys, &n);
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

/*
 * SEARCH <filters>: "key=value" pairs separated by '|' (values never hold
 * '|' or tabs): q (title words, all must match, case-insensitive),
 * system=<id>, collection=<id>, tag=<name> (an unknown name matches
 * nothing), year=<from>-<to>, genre=<text>
 * (substring), players=<n>
 * (at least n), unplayed=1, random=1 (one game picked at random from the
 * matches). Only available games. Replies game= lines (like GAMES, title
 * order) and end=1.
 */
struct search_filter {
	char words[8][64];
	int word_count;
	int sys;
	/* collection=: its games only; an unknown id matches nothing. */
	const struct collection *col;
	bool col_missing;
	/* tag=: its bit; -2 = unknown tag (nothing matches). */
	int tag;
	int year_from, year_to;
	char genre[64];
	int players;
	bool unplayed;
	bool random;
};

static bool contains_nocase(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return true;
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return true;
	return false;
}

static bool keep_search(const struct game *g, const void *arg)
{
	const struct search_filter *f = arg;
	const struct history_entry *h;

	if (!g->available)
		return false;
	if (f->sys >= 0 && g->sys != f->sys)
		return false;
	if (f->col_missing || (f->col && !keep_collection(g, f->col)))
		return false;
	if (f->tag == -2 || (f->tag >= 0 && !(tags_of(g->id) & (1u << f->tag))))
		return false;
	for (int i = 0; i < f->word_count; i++)
		if (!contains_nocase(g->title, f->words[i]))
			return false;
	if (f->year_from && (g->year < f->year_from || g->year > f->year_to))
		return false;
	if (f->genre[0] && (!g->genre || !contains_nocase(g->genre, f->genre)))
		return false;
	if (f->players && g->players < f->players)
		return false;
	if (f->unplayed) {
		h = active_user[0] ? history_for(g->id, false) : NULL;
		if (h && hist_last(h) > 0)
			return false;
	}
	return true;
}

static void parse_search(const char *spec, struct search_filter *f)
{
	char buf[MAX_LINE];
	char *save = NULL;

	memset(f, 0, sizeof(*f));
	f->sys = -1;
	f->tag = -1;
	copy_text(buf, sizeof(buf), spec);
	for (char *kv = strtok_r(buf, "|", &save); kv; kv = strtok_r(NULL, "|", &save)) {
		char *v = strchr(kv, '=');

		if (!v)
			continue;
		*v++ = '\0';
		if (!strcmp(kv, "q")) {
			char *wsave = NULL;

			for (char *w = strtok_r(v, " ", &wsave); w && f->word_count < 8; w = strtok_r(NULL, " ", &wsave))
				copy_text(f->words[f->word_count++], sizeof(f->words[0]), w);
		} else if (!strcmp(kv, "system")) {
			f->sys = system_by_id(v);
			if (f->sys < 0)
				f->sys = -2; /* unknown system: nothing matches */
		} else if (!strcmp(kv, "collection")) {
			f->col = collection_by_id(v);
			f->col_missing = !f->col;
		} else if (!strcmp(kv, "tag")) {
			f->tag = tag_by_name(v);
			if (f->tag < 0)
				f->tag = -2;
		} else if (!strcmp(kv, "year")) {
			if (sscanf(v, "%d-%d", &f->year_from, &f->year_to) != 2)
				f->year_from = f->year_to = 0;
		} else if (!strcmp(kv, "genre")) {
			copy_text(f->genre, sizeof(f->genre), v);
		} else if (!strcmp(kv, "players")) {
			f->players = atoi(v);
		} else if (!strcmp(kv, "unplayed")) {
			f->unplayed = !strcmp(v, "1");
		} else if (!strcmp(kv, "random")) {
			f->random = !strcmp(v, "1");
		}
	}
}

static void build_search(struct strbuf *b, const char *spec)
{
	struct search_filter f;
	const struct game **list;
	size_t n = 0;

	parse_search(spec, &f);
	list = sorted_games(keep_search, &f, &n);
	if (f.random && n) {
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		srand((unsigned)(ts.tv_nsec ^ ts.tv_sec));
		append_game(b, "game", list[(size_t)rand() % n]);
	} else {
		for (size_t i = 0; i < n; i++)
			append_game(b, "game", list[i]);
	}
	free(list);
	sb_append(b, "end=1\n");
}

/*
 * FILTERS: what the Find page can offer, from available games only:
 * decade=<first year>\t<count>, genre=<name>\t<count> (the first genre of
 * each game, most common first, at most 24), tag=<name>\t<count> (the
 * active user's tags in use), players=<most>.
 */
static int genre_count_compare(const void *a, const void *b)
{
	const int *x = a, *y = b;

	return y[1] - x[1];
}

static void build_filters(struct strbuf *b)
{
	int decades[16] = { 0 };
	char names[64][48];
	int counts[64][2];
	int genre_n = 0;
	int most_players = 0;
	int tag_counts[TAG_COUNT] = { 0 };

	for (size_t i = 0; i < game_count; i++) {
		const struct game *g = &games[i];
		unsigned mask;

		if (!g->available)
			continue;
		mask = tags_of(g->id);
		for (size_t t = 0; t < TAG_COUNT; t++)
			if (mask & (1u << t))
				tag_counts[t]++;
		if (g->year >= 1950 && g->year < 2110)
			decades[(g->year - 1950) / 10 < 16 ? (g->year - 1950) / 10 : 15]++;
		if (g->players > most_players)
			most_players = g->players;
		if (g->genre && g->genre[0]) {
			char first[48];
			int k;

			/* "Platform, Action" / "Platform / Action" -> "Platform" */
			copy_text(first, sizeof(first), g->genre);
			first[strcspn(first, ",/;")] = '\0';
			trim(first);
			if (!first[0])
				continue;
			for (k = 0; k < genre_n; k++)
				if (!strcasecmp(names[k], first))
					break;
			if (k == genre_n) {
				if (genre_n == 64)
					continue;
				copy_text(names[genre_n], sizeof(names[0]), first);
				counts[genre_n][0] = genre_n;
				counts[genre_n][1] = 0;
				genre_n++;
			}
			counts[k][1]++;
		}
	}
	for (int d = 0; d < 16; d++)
		if (decades[d])
			sb_append(b, "decade=%d\t%d\n", 1950 + d * 10, decades[d]);
	qsort(counts, (size_t)genre_n, sizeof(counts[0]), genre_count_compare);
	for (int k = 0; k < genre_n && k < 24; k++)
		sb_append(b, "genre=%s\t%d\n", names[counts[k][0]], counts[k][1]);
	for (size_t t = 0; t < TAG_COUNT; t++)
		if (tag_counts[t])
			sb_append(b, "tag=%s\t%d\n", tag_defs[t].name, tag_counts[t]);
	sb_append(b, "players=%d\nend=1\n", most_players);
}

/* ------------------------------------------------------------------ */
/* Game Details and Delete Game                                       */
/* ------------------------------------------------------------------ */

/*
 * Provider-neutral metadata (EPIC-015): one "key=value" per line, values on
 * one line ("\n" escaped by the writer). Unknown keys are passed through as
 * meta_<key> so a new provider field needs no libraryd change.
 */
/* The active user's interface language ("en" when unknown). */
static void user_language(char out[8])
{
	char path[PATH_MAX];
	char line[64];
	FILE *fp;

	copy_text(out, 8, "en");
	if (!active_user[0] || !pathf(path, sizeof(path), STATE_ROOT "/users/%s/localization.conf", active_user))
		return;
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (!strncmp(line, "LANGUAGE=", 9) && strlen(line + 9) == 2)
			copy_text(out, 8, line + 9);
		else if (!strncmp(line, "LANGUAGE=", 9) && strlen(line + 9) > 3 && line[11] == '-') {
			line[11] = '\0'; /* zh-cn, pt-br, es-419: the scraper's key */
			copy_text(out, 8, line + 9);
		}
	}
	fclose(fp);
}

/*
 * Localized text is kept per language by the scraper ("description@it=");
 * the user's language wins over the plain key (the last one fetched).
 */
static void append_metadata(struct strbuf *b, const struct game *g)
{
	static char line[16384];
	char stem[512];
	char path[PATH_MAX];
	char lang[8];
	char tag[12];
	bool have_desc = false, have_genre = false;
	FILE *fp;

	rom_stem(g->rel, stem, sizeof(stem));
	if (!pathf(path, sizeof(path), METADATA_ROOT "/%s/%s.txt", systems[g->sys].id, stem))
		return;
	fp = fopen(path, "r");
	if (!fp && is_ports(g->sys)) {
		/* No scraped metadata: what PortMaster installed with the port. */
		size_t count;
		struct port_info *list = load_port_infos(&count);
		const struct port_info *info = port_info_for(list, count, g->rel);

		if (info && info->desc)
			sb_append(b, "meta_description=%s\n", info->desc);
		if (info && info->developer)
			sb_append(b, "meta_developer=%s\n", info->developer);
		if (info && info->year)
			sb_append(b, "meta_release=%d\n", info->year);
		free_port_infos(list, count);
		return;
	}
	if (!fp)
		return;
	user_language(lang);
	snprintf(tag, sizeof(tag), "@%s", lang);
	/* Pass 1: the user's language; pass 2: everything else. */
	for (int pass = 1; pass <= 2; pass++) {
		rewind(fp);
		while (fgets(line, sizeof(line), fp)) {
			char *eq, *at;

			/* An over-long line is dropped whole, never split into a
			 * bogus second key. */
			if (!strchr(line, '\n') && !feof(fp)) {
				int c;

				while ((c = fgetc(fp)) != EOF && c != '\n')
					;
				continue;
			}
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] == '#' || !(eq = strchr(line, '=')) || eq == line)
				continue;
			*eq++ = '\0';
			if (!tsv_safe(eq))
				continue;
			at = strchr(line, '@');
			if (at) {
				if (pass != 1 || strcmp(at, tag))
					continue;
				*at = '\0';
				if (!strcmp(line, "description"))
					have_desc = true;
				else if (!strcmp(line, "genre"))
					have_genre = true;
				else
					continue;
				sb_append(b, "meta_%s=%s\n", line, eq);
				continue;
			}
			if (pass != 2 || strspn(line, "abcdefghijklmnopqrstuvwxyz_0123456789") != strlen(line))
				continue;
			if ((have_desc && !strcmp(line, "description")) || (have_genre && !strcmp(line, "genre")))
				continue;
			sb_append(b, "meta_%s=%s\n", line, eq);
		}
	}
	fclose(fp);
}

/* The catalog path of a "<Folder>/EBOOT.PBP" game's folder (rom_name). */
static bool game_folder(const char *rel, char *out, size_t size)
{
	const char *name = rel_name(rel);

	if (rom_name(rel) == name)
		return false;
	snprintf(out, size, "%.*s", (int)(name - 1 - rel), rel);
	return true;
}

/* Another catalog game inside that folder (then only the EBOOT goes). */
static bool folder_shared(const struct game *self, const char *folder)
{
	size_t len = strlen(folder);

	for (size_t i = 0; i < game_count; i++)
		if (&games[i] != self && !strncmp(games[i].rel, folder, len) && games[i].rel[len] == '/')
			return true;
	return false;
}

static void build_details(struct strbuf *b, const struct game *g)
{
	const struct history_entry *h = history_for(g->id, false);

	char file[PATH_MAX];

	append_game(b, "game", g);
	sb_append(b, "system_name=%s\nsessions=%d\npath=%s\nsize=%lld\nadded=%lld\n",
		  systems[g->sys].name, h ? hist_sessions(h) : 0, g->rel, g->size, g->added);
	/* The ROM's name for covers and metadata (rom_stem). */
	rom_stem(g->rel, file, sizeof(file));
	sb_append(b, "name=%s\n", file);
	/* Delete Game removes the whole folder (delete_game). */
	if (game_folder(g->rel, file, sizeof(file)) && !folder_shared(g, file))
		sb_append(b, "folder=1\n");
	/* Absolute file and the ROM folder of its card, while the card is there. */
	if (rom_path(file, sizeof(file), g->rel))
		sb_append(b, "file=%s\nroot=%s\n", file,
			  rel_on_sd2(g->rel) ? sd2_roms : ROMS_ROOT);
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
 * game rather than a dangling entry. An unavailable game (its file gone,
 * or on a games card that is not inserted) loses only its library entry,
 * cover and metadata: no file is touched (user request 2026-10-20).
 */
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	if (remove(path) != 0 && errno != ENOENT) {
		log_msg("delete %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

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
		goto entry;
	/* A game in a folder of its own (GitHub #30, user 2026-10-30): the
	 * whole folder, its data beside the EBOOT.PBP included. Never another
	 * system's folder, a mount point below it, or a folder holding other
	 * games of the catalog. */
	{
		char folder[PATH_MAX];

		if (game_folder(g->rel, folder, sizeof(folder)) && !folder_shared(g, folder)) {
			if (!rom_path(path, sizeof(path), folder) ||
			    nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT) != 0)
				return "io";
			sync();
			goto entry;
		}
	}
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
		if (!rom_path(path, sizeof(path), owned[k]))
			continue;
		if (unlink(path) != 0 && errno != ENOENT) {
			log_msg("delete %s: %s", path, strerror(errno));
			failed = true;
		}
	}
	if (failed)
		return "io";
	if (!rom_path(path, sizeof(path), g->rel) ||
	    (unlink(path) != 0 && errno != ENOENT)) {
		log_msg("delete %s: %s", path, strerror(errno));
		return "io";
	}
	sync();
entry:
	rom_stem(g->rel, stem, sizeof(stem));
	if (g->cover && !strncmp(g->cover, COVERS_ROOT "/", strlen(COVERS_ROOT) + 1))
		unlink(g->cover);
	if (pathf(path, sizeof(path), METADATA_ROOT "/%s/%s.txt", systems[g->sys].id, stem))
		unlink(path);
	/* The scraper's "tried" mark (Fill Missing Data). */
	if (pathf(path, sizeof(path), METADATA_ROOT "/%s/%s.tried", systems[g->sys].id, stem))
		unlink(path);
	log_msg("deleted game %s (%s)", g->id, g->rel);
	free(g->rel);
	free(g->title);
	free(g->cover);
	free(g->genre);
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
	} else if (!strcmp(line, "SHOW_UNAVAILABLE") && arg && (!strcmp(arg, "0") || !strcmp(arg, "1"))) {
		show_unavailable = arg[0] == '1';
		reply(c, save_library_config() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "WRAP_ROWS") && arg && (!strcmp(arg, "0") || !strcmp(arg, "1"))) {
		wrap_rows = arg[0] == '1';
		reply(c, save_library_config() ? "ERR persistence\n" : "OK\n");
		notify_all();
	} else if (!strcmp(line, "SCAN")) {
		if (!scanning)
			scan_new = 0;
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
	} else if (!strcmp(line, "PORTS_ROOT")) {
		/* nuubos-portsd, before a session or a PortMaster job (GitHub
		 * #21): the ports folder now ("" while its SD2 card is out) and
		 * the SD2 card's ports folder ("" without the card). Not re-read
		 * during a scan (the scan thread reads it). */
		struct strbuf b = { 0 };

		if (!scanning) {
			refresh_sd2_root();
			refresh_ports_root();
		}
		sb_append(&b, "OK %s\t%s\t%s\n", ports_sd2 ? "sd2" : "console", ports_root, sd2_ports);
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!strcmp(line, "RESOLVE") && arg) {
		/* Launch lookup for the Emulation Service: system and
		 * absolute content path of an available game (and, for a
		 * disc set, the ROM's path). */
		long idx = valid_game_id(arg) ? find_game(arg) : -1;
		struct strbuf b = { 0 };

		if (idx < 0) {
			reply(c, "ERR game\n");
			return;
		}
		char path[PATH_MAX];

		/* The card may have gone since the last scan. */
		if (!games[idx].available || !rom_path(path, sizeof(path), games[idx].rel) ||
		    !regular_file(path)) {
			reply(c, "ERR unavailable\n");
			return;
		}
		char playlist[PATH_MAX];

		/* A disc set launches from one playlist of all its discs; the
		 * third field is the ROM's own path (its card). */
		if (disc_set_playlist(games[idx].rel, playlist, sizeof(playlist)))
			sb_append(&b, "OK %s\t%s\t%s\n", systems[games[idx].sys].id, playlist, path);
		else
			sb_append(&b, "OK %s\t%s\n", systems[games[idx].sys].id, path);
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!strcmp(line, "MATCH") && arg) {
		/* Netplay: the game of a content file name another console
		 * announces. */
		long idx = match_game(arg);
		struct strbuf b = { 0 };

		if (idx < 0) {
			reply(c, "ERR game\n");
			return;
		}
		sb_append(&b, "OK %s\t%s\n", games[idx].id, systems[games[idx].sys].id);
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!strcmp(line, "SEARCH")) {
		struct strbuf b = { 0 };

		build_search(&b, arg ? arg : "");
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!strcmp(line, "FILTERS")) {
		struct strbuf b = { 0 };

		build_filters(&b);
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!strcmp(line, "SYSTEMS")) {
		/* Every system of the registry, with or without games (Game
		 * Details: Search System, GitHub #29). */
		struct strbuf b = { 0 };

		for (int i = 0; i < system_count; i++)
			sb_append(&b, "system=%s\t%s\n", systems[i].id, systems[i].name);
		sb_append(&b, "end=1\n");
		send_text(c, b.data, b.len);
		free(b.data);
	} else if (!active_user[0] &&
		   (!strncmp(line, "SESSION_", 8) || !strncmp(line, "MIGRATE_", 8) ||
		    !strncmp(line, "COLLECTION_", 11) || !strncmp(line, "TAG_", 4) ||
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
			for (size_t i = 0; i < history_count; i++)
				reset_history(&history[i]);
		} else if (known_game(arg)) {
			struct history_entry *h = history_for(arg, false);

			if (h)
				reset_history(h);
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
	} else if (!strcmp(line, "MIGRATE_FAVORITES") && arg) {
		/* The old favorites list becomes an ordinary collection named
		 * by the UI in the user's language; favorites.txt goes away. */
		char path[PATH_MAX];
		char out[48];
		struct collection *col;
		static unsigned mcounter;

		if (!favorite_count) {
			reply(c, "OK\n");
			return;
		}
		if (!arg[0] || strlen(arg) >= sizeof(col->name) || !tsv_safe(arg)) {
			reply(c, "ERR name\n");
			return;
		}
		collections = xrealloc(collections, (collection_count + 1) * sizeof(*collections));
		col = &collections[collection_count++];
		memset(col, 0, sizeof(*col));
		snprintf(col->id, sizeof(col->id), "c%08llx",
			 (unsigned long long)((wall_now() << 4) + 8 + (mcounter++ & 7)) & 0xffffffffULL);
		copy_text(col->name, sizeof(col->name), arg);
		for (size_t i = 0; i < favorite_count; i++)
			id_list_add(&col->games, &col->count, favorites[i]);
		if (save_collections()) {
			reply(c, "ERR persistence\n");
			return;
		}
		free(favorites);
		favorites = NULL;
		favorite_count = 0;
		user_file("favorites.txt", path, sizeof(path));
		unlink(path);
		snprintf(out, sizeof(out), "OK %s\n", col->id);
		reply(c, out);
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
	} else if ((!strcmp(line, "TAG_SET") || !strcmp(line, "TAG_CLEAR")) && arg && arg2) {
		int tag = tag_by_name(arg2);

		if (tag < 0 || !known_game(arg)) {
			reply(c, "ERR args\n");
			return;
		}
		set_tag(arg, tag, line[4] == 'S');
		reply(c, save_tags() ? "ERR persistence\n" : "OK\n");
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
	int listen_fd, watch = -1;
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
	load_console_id();
	load_library_config();
	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	refresh_active_user();

	if (pipe2(scan_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		log_msg("pipe: %s", strerror(errno));
		return 1;
	}
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
	 * the background (EPIC-012 non-blocking startup). At boot it waits for
	 * nuubUI to be ready: its directory reads on USERDATA and the games
	 * card compete with the UI's own start on the card (docs/dev/
	 * platform.md "Boot time"). Not without a catalog (first boot): there
	 * is nothing to show without the scan. */
	if (game_count > 0 && inotify_fd >= 0 && access(UI_READY, F_OK) != 0)
		ui_wd = inotify_add_watch(inotify_fd, RUN_ROOT, IN_CREATE | IN_MOVED_TO);
	if (ui_wd >= 0) {
		/* Games on the games card launch before the scan too. */
		refresh_sd2_root();
		refresh_ports_root();
		boot_scan_pending = true;
		boot_scan_deadline_ms = monotonic_ms() + BOOT_SCAN_WAIT_MS;
	} else {
		start_scan();
	}

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
		int timeout = -1;

		if (ui_wd >= 0) {
			long long left = boot_scan_deadline_ms - monotonic_ms();

			timeout = left > 0 ? (int)left : 0;
		}
		int ready = poll(fds, (nfds_t)n, timeout);

		if (ready == 0 && ui_wd >= 0)
			end_boot_wait();
		if (ready < 0) {
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
				bool consoles = false, user = false, ui_ready = false;
				ssize_t len;

				while ((len = read(inotify_fd, events, sizeof(events))) > 0)
					for (char *q = events; q < events + len;) {
						const struct inotify_event *ev = (const struct inotify_event *)q;

						if (ev->wd == ui_wd) {
							if (ev->len && !strcmp(ev->name, "ui-ready"))
								ui_ready = true;
						} else if (ev->wd == consoles_wd)
							/* Our own export is no news. */
							consoles = consoles || !ev->len ||
								   strncmp(ev->name, console_id, strlen(console_id));
						else
							user = true;
						q += sizeof(*ev) + ev->len;
					}
				if (ui_ready)
					end_boot_wait();
				copy_text(before, sizeof(before), active_user);
				if (user)
					refresh_active_user();
				if (strcmp(before, active_user)) {
					notify_all();
				} else if (consoles) {
					/* Another console's play history arrived. */
					load_remote_history();
					notify_all();
				}
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
