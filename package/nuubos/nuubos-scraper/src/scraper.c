/* SPDX-License-Identifier: MIT */
/*
 * nuubos-scraper — Game Metadata (EPIC-015).
 *
 *   game GAME [force|fill]   scrape one game (force: also a matched game,
 *                            new data first; fill: only what is missing)
 *   bulk all|missing[:SECONDS]|lang|system:<id>
 *                            all: games without metadata; missing: also
 *                            matched games that lack any data the source
 *                            can give (cover, screenshot, description,
 *                            release, genre, developer, publisher,
 *                            players), skipping games tried in the last
 *                            SECONDS (<rom name>.tried); lang: matched
 *                            games without text in the active user's
 *                            language (a background job)
 *   search GAME              set the name used to look the game up (name on
 *                            stdin; empty = back to the file name)
 *   accept GAME              approve a match waiting for review
 *   reject GAME              discard a match; bulk scraping skips the game
 *   clear GAME               remove scraped metadata and media of a game
 *   provider status | provider set screenscraper|thegamesdb|libretro
 *   account status | account set NAME (password on stdin) | account clear
 *   apikey set (key on stdin) | apikey clear      TheGamesDB private key
 *
 * Optional online enrichment: the Library never depends on it. Runs only
 * when the user asks (no automatic network use, EPIC-055). Source data is
 * mapped into the provider-neutral store shared by every user:
 *   /userdata/library/metadata/<system>/<rom name>.txt   key=value lines
 *   /userdata/library/covers/<system>/<rom name>.png|jpg  (Library covers)
 *   /userdata/library/screenshots/<system>/<rom name>.png|jpg
 *   /userdata/library/cache/                               source caches
 * Everything is derived data: removable and rebuildable, never the ROMs.
 *
 * Sources (one active, device-wide, /state/scraper/scraper.conf):
 * - screenscraper, thegamesdb: through Skyscraper (package skyscraper,
 *   GPL-3.0, run as a separate program), which brings its own registered
 *   access to both services. The user's ScreenScraper account (more
 *   requests and threads) and TheGamesDB private key are optional,
 *   device-wide in /state/scraper (root only) and reach Skyscraper only
 *   through a config file on tmpfs (/run/nuubos/scraper). Skyscraper's
 *   resource cache lives in /userdata/library/cache/skyscraper; nuubOS
 *   reads its results from a Pegasus list generated into a scratch folder.
 * - libretro: covers and screenshots of thumbnails.libretro.com, matched by
 *   the No-Intro/Redump name, read directly; no account. The per-system
 *   name index is cached for a week.
 *
 * Languages: localized text (description, genre) is stored for every
 * language it was fetched in ("description@it=", "genre@it="), the plain
 * keys holding the latest one; libraryd shows the user's language when it
 * is there. Fetching another language never loses the ones already kept.
 *
 * Matching: Skyscraper identifies the game (checksums for ScreenScraper,
 * its title matching otherwise); libretro by the dump name. An identified
 * game is applied ("match=auto"). A match the user accepted ("match=user")
 * is never replaced unless forced; "match=review" (older data) waits for
 * Accept. A search name set by the user ("search=") replaces the file name
 * in the lookup.
 *
 * As a nuubos-jobd worker it reports @progress / @result / @error and stops
 * on SIGTERM (Skyscraper included), leaving no partial file. Quota,
 * credential, network and service failures stop the run (no retry loop).
 */

#include <ctype.h>
#include <dirent.h>
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef USERDATA_ROOT
#define USERDATA_ROOT "/userdata"
#endif
#ifndef STATE_ROOT
#define STATE_ROOT "/state"
#endif
#ifndef SKYSCRAPER_BIN
#define SKYSCRAPER_BIN "/usr/bin/Skyscraper"
#endif
#ifndef LIBRETRO_BASE
#define LIBRETRO_BASE "https://thumbnails.libretro.com"
#endif
#define USER_AGENT "nuubOS"

#define LIBRARY_SOCKET RUN_ROOT "/libraryd.sock"
#define ACTIVE_USER_FILE RUN_ROOT "/user/active"
#define SKY_HOME RUN_ROOT "/scraper"
#define ROMS_ROOT USERDATA_ROOT "/roms"
#define META_ROOT USERDATA_ROOT "/library/metadata"
#define COVERS_ROOT USERDATA_ROOT "/library/covers"
#define SHOTS_ROOT USERDATA_ROOT "/library/screenshots"
#define CACHE_ROOT USERDATA_ROOT "/library/cache"
#define SCRAPER_STATE STATE_ROOT "/scraper"
#define SETTINGS_FILE SCRAPER_STATE "/scraper.conf"
#define ACCOUNT_FILE SCRAPER_STATE "/screenscraper.conf"
#define TGDB_FILE SCRAPER_STATE "/thegamesdb.conf"
#define LIBRETRO_INDEX_AGE (7 * 24 * 3600)
#define MAX_BATCH 4096

enum provider { PROV_SCREENSCRAPER, PROV_THEGAMESDB, PROV_LIBRETRO, PROV_COUNT };
static const char *const provider_ids[PROV_COUNT] = { "screenscraper", "thegamesdb", "libretro" };

enum mode { MODE_NEW, MODE_FORCE, MODE_FILL };
enum outcome { SCRAPED, REVIEW, NOT_FOUND, SKIPPED, FAILED, STOP };

static volatile sig_atomic_t cancelled;
/* bulk lang: ask the source again instead of Skyscraper's cache. */
static bool refresh_cache;
/* bulk missing:SECONDS: games tried after this time are skipped. */
static long long skip_tried_after;
static bool as_job;
static const char *stop_reason = "";
static const char *skip_reason = "matched";
/* The ScreenScraper account was refused (Skyscraper went on without it). */
static bool account_refused;

struct game_info {
	char id[32];
	char system[32];
	char title[256];
	char rel[PATH_MAX];
	char stem[512];
};

struct account {
	char user[64];
	char password[128];
};

struct metadata {
	char provider[16];
	char provider_id[32];
	char match[16];
	char search[256];
	char title[256];
	char description[4096];
	char release[32];
	char developer[128];
	char publisher[128];
	char genre[128];
	char players[32];
	char rating[16];
	char cover_url[1024];
	char shot_url[1024];
	char screenshot[PATH_MAX];
	/* Language of description/genre (ScreenScraper/TheGamesDB runs). */
	char lang[8];
	/* "key@xx=value" lines of other languages, kept as they are. */
	char other_langs[16384];
	/* Not stored: a cover/screenshot file a source produced locally. */
	char cover_file[PATH_MAX];
	char shot_file[PATH_MAX];
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

static int mkdir_p(const char *path, mode_t mode)
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
	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		return -1;
	return chmod(tmp, mode);
}

static void remove_tree(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	char child[PATH_MAX];

	if (!d) {
		unlink(path);
		return;
	}
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
			snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
			remove_tree(child);
		}
	closedir(d);
	rmdir(path);
}

/* One line per value: line breaks become the two characters "\n". */
static void one_line(const char *in, char *out, size_t size)
{
	size_t o = 0;

	for (const char *p = in ? in : ""; *p && o + 3 < size; p++) {
		/* Never cut a UTF-8 sequence: stop before one that does not fit. */
		if (((unsigned char)*p & 0xc0) == 0xc0) {
			size_t len = 1;

			while (((unsigned char)p[len] & 0xc0) == 0x80)
				len++;
			if (o + len + 1 > size)
				break;
			memcpy(out + o, p, len);
			o += len;
			p += len - 1;
			continue;
		}
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

/* Write "text" to path atomically; private files are 0600. */
static int write_file(const char *path, const char *text, bool private)
{
	char tmp[PATH_MAX + 8];
	mode_t old = umask(private ? 077 : 022);
	FILE *fp;
	int rc = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	umask(old);
	if (!fp)
		return -1;
	fputs(text, fp);
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0)
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc)
		unlink(tmp);
	return rc;
}

/* The value of KEY= in a small config file. */
static bool conf_value(const char *path, const char *key, char *out, size_t size)
{
	char line[512];
	size_t klen = strlen(key);
	bool found = false;
	FILE *fp = fopen(path, "r");

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

/* Name comparison key: the title before any "(...)"/"[...]" tag, lower
 * case letters and digits only, without the article "the" (so "Legend of
 * Zelda, The" matches "The Legend of Zelda"). */
static void name_key(const char *in, char *out, size_t size)
{
	char word[128];
	size_t o = 0, w = 0;

	for (const char *p = in;; p++) {
		bool end = !*p || *p == '(' || *p == '[';

		if (!end && isalnum((unsigned char)*p)) {
			if (w + 1 < sizeof(word))
				word[w++] = (char)tolower((unsigned char)*p);
			continue;
		}
		word[w] = '\0';
		if (w && strcmp(word, "the") && o + w + 1 < size) {
			memcpy(out + o, word, w);
			o += w;
		}
		w = 0;
		if (end)
			break;
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

/*
 * Search name for a second try when a file name finds nothing: no (...)
 * [...] {...} tags (regions, years, [N64], hacks), '_' as spaces,
 * no ", The", no version/revision words (v1.1, Rev 1, Rev A). Empty when
 * nothing is left.
 */
static void search_title(const char *stem, char *out, size_t size)
{
	char tmp[512];
	size_t o = 0;
	int depth = 0;

	for (const char *p = stem; *p && o + 1 < sizeof(tmp); p++) {
		if (*p == '(' || *p == '[' || *p == '{') {
			depth++;
			continue;
		}
		if (*p == ')' || *p == ']' || *p == '}') {
			if (depth > 0)
				depth--;
			continue;
		}
		if (depth)
			continue;
		tmp[o++] = *p == '_' ? ' ' : *p;
	}
	tmp[o] = '\0';

	/* Words, dropping version/revision tokens and a trailing ", The". */
	o = 0;
	out[0] = '\0';
	for (char *save = NULL, *w = strtok_r(tmp, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
		size_t len = strlen(w);

		if ((w[0] == 'v' || w[0] == 'V') && len > 1 && isdigit((unsigned char)w[1]))
			continue;
		if (!strcasecmp(w, "rev")) {
			char *next = strtok_r(NULL, " ", &save);

			(void)next;
			continue;
		}
		if (o + len + 2 >= size)
			break;
		if (o)
			out[o++] = ' ';
		memcpy(out + o, w, len);
		o += len;
		out[o] = '\0';
	}
	if (o >= 5 && !strcasecmp(out + o - 5, ", The"))
		out[o -= 5] = '\0';
	while (o && (out[o - 1] == ' ' || out[o - 1] == '-' || out[o - 1] == ','))
		out[--o] = '\0';
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
		}
	}
	stem_of(g->rel, g->stem, sizeof(g->stem));
	return g->system[0] && g->rel[0];
}

static void library_rescan(void)
{
	char buf[256];

	(void)library_request("SCAN\n", buf, sizeof(buf));
}

/* ------------------------------------------------------------------ */
/* System mapping                                                      */
/* ------------------------------------------------------------------ */

/*
 * nuubOS system id -> Skyscraper platform (its peas.json; used for
 * ScreenScraper and TheGamesDB) and libretro thumbnail folders
 * (thumbnails.libretro.com; a second one for a system nuubOS merges,
 * e.g. Neo Geo Pocket + Color). A source without an entry does not scrape
 * that system.
 */
static const struct system_map {
	const char *id;
	const char *sky;
	const char *libretro[2];
} systems[] = {
	{ "nes", "nes", { "Nintendo - Nintendo Entertainment System" } },
	{ "snes", "snes", { "Nintendo - Super Nintendo Entertainment System" } },
	{ "n64", "n64", { "Nintendo - Nintendo 64" } },
	{ "gb", "gb", { "Nintendo - Game Boy" } },
	{ "gbc", "gbc", { "Nintendo - Game Boy Color" } },
	{ "gba", "gba", { "Nintendo - Game Boy Advance" } },
	{ "nds", "nds", { "Nintendo - Nintendo DS" } },
	{ "virtualboy", "virtualboy", { "Nintendo - Virtual Boy" } },
	{ "fds", "fds", { "Nintendo - Family Computer Disk System" } },
	{ "pokemini", "pokemini", { "Nintendo - Pokemon Mini" } },
	{ "satellaview", "snes", { "Nintendo - Satellaview" } },
	{ "sufami", "snes", { "Nintendo - Sufami Turbo" } },
	{ "mastersystem", "mastersystem", { "Sega - Master System - Mark III" } },
	{ "megadrive", "megadrive", { "Sega - Mega Drive - Genesis" } },
	{ "segacd", "segacd", { "Sega - Mega-CD - Sega CD" } },
	{ "sega32x", "sega32x", { "Sega - 32X" } },
	{ "gamegear", "gamegear", { "Sega - Game Gear" } },
	{ "saturn", "saturn", { "Sega - Saturn" } },
	{ "dreamcast", "dreamcast", { "Sega - Dreamcast" } },
	{ "sg1000", "sg-1000", { "Sega - SG-1000" } },
	{ "naomi", "naomi", { "Sega - Naomi" } },
	{ "atomiswave", "atomiswave", { "Atomiswave" } },
	{ "psx", "psx", { "Sony - PlayStation" } },
	{ "psp", "psp", { "Sony - PlayStation Portable" } },
	{ "pcengine", "pcengine", { "NEC - PC Engine - TurboGrafx 16" } },
	{ "pcecd", "pcenginecd", { "NEC - PC Engine CD - TurboGrafx-CD" } },
	{ "pc88", "pc88", { "NEC - PC-8001 - PC-8801" } },
	{ "pc98", "pc98", { "NEC - PC-98" } },
	{ "neogeo", "neogeo", { "SNK - Neo Geo" } },
	{ "geolith", "neogeo", { "SNK - Neo Geo" } },
	{ "neogeocd", "neogeocd", { "SNK - Neo Geo CD" } },
	{ "ngp", "ngp", { "SNK - Neo Geo Pocket", "SNK - Neo Geo Pocket Color" } },
	{ "wonderswan", "wonderswan", { "Bandai - WonderSwan", "Bandai - WonderSwan Color" } },
	{ "lynx", "atarilynx", { "Atari - Lynx" } },
	{ "atari2600", "atari2600", { "Atari - 2600" } },
	{ "atari5200", "atari5200", { "Atari - 5200" } },
	{ "atari7800", "atari7800", { "Atari - 7800" } },
	{ "atari800", "atari800", { "Atari - 8-bit" } },
	{ "atarixe", "atari800", { "Atari - 8-bit" } },
	{ "atarist", "atarist", { "Atari - ST" } },
	{ "c64", "c64", { "Commodore - 64" } },
	{ "c128", "c128", { NULL } },
	{ "amiga", "amiga", { "Commodore - Amiga" } },
	{ "vic20", "vic20", { "Commodore - VIC-20" } },
	{ "plus4", "plus4", { "Commodore - Plus-4" } },
	{ "amstradcpc", "amstradcpc", { "Amstrad - CPC" } },
	{ "msx", "msx", { "Microsoft - MSX", "Microsoft - MSX2" } },
	{ "zxspectrum", "zxspectrum", { "Sinclair - ZX Spectrum" } },
	{ "x68000", "x68000", { "Sharp - X68000" } },
	{ "x1", "x1", { "Sharp - X1" } },
	{ "dos", "pc", { "DOS" } },
	{ "scummvm", "scummvm", { "ScummVM" } },
	{ "macintosh", "macintosh", { NULL } },
	{ "apple2", "apple2", { NULL } },
	{ "coleco", "coleco", { "Coleco - ColecoVision" } },
	{ "channelf", "channelf", { "Fairchild - Channel F" } },
	{ "vectrex", "vectrex", { "GCE - Vectrex" } },
	{ "intellivision", "intellivision", { "Mattel - Intellivision" } },
	{ "odyssey2", "videopac", { "Magnavox - Odyssey2" } },
	{ "supervision", "supervision", { "Watara - Supervision" } },
	{ "megaduck", "megaduck", { NULL } },
	{ "gamecom", "gamecom", { "Tiger - Game.com" } },
	{ "vtech", "vsmile", { "VTech - V.Smile" } },
	{ "cassettevision", "scv", { "Epoch - Super Cassette Vision" } },
	{ "samcoupe", "samcoupe", { NULL } },
	{ "coco", "coco", { NULL } },
	{ "dragon32", "dragon32", { NULL } },
	{ "ti99", "ti99", { NULL } },
	{ "trs80", "trs-80", { NULL } },
	{ "oric", "oric", { NULL } },
	{ "fmtowns", "fmtowns", { NULL } },
	{ "palm", "palm", { NULL } },
	{ "tic80", "tic80", { "TIC-80" } },
	{ "pico8", "pico8", { NULL } },
	{ "lutro", NULL, { "Lutro" } },
	{ "arcade", "arcade", { "FBNeo - Arcade Games" } },
	{ "fbneo", "fba", { "FBNeo - Arcade Games" } },
	{ "cps1", "fba", { "FBNeo - Arcade Games" } },
	{ "cps2", "fba", { "FBNeo - Arcade Games" } },
	{ "cps3", "fba", { "FBNeo - Arcade Games" } },
	{ "mame", "mame", { "MAME" } },
};

static const struct system_map *system_of(const char *id)
{
	for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); i++)
		if (!strcmp(systems[i].id, id))
			return &systems[i];
	return NULL;
}

static bool system_supported(int provider, const char *id)
{
	const struct system_map *s = system_of(id);

	if (!s)
		return false;
	return provider == PROV_LIBRETRO ? s->libretro[0] != NULL : s->sky != NULL;
}

/* Region preference of libretro dumps: the file name's region tag first,
 * then world, USA, Europe, Japan. */
static void region_order(const char *name, const char *out[8])
{
	static const struct { const char *tag; const char *region; } tags[] = {
		{ "(Europe", "eu" }, { "(E)", "eu" }, { "Europe)", "eu" },
		{ "(USA", "us" }, { "(U)", "us" }, { "USA)", "us" },
		{ "(Japan", "jp" }, { "(J)", "jp" }, { "Japan)", "jp" },
		{ "(World", "wor" },
	};
	static const char *const base[] = { "wor", "us", "eu", "jp", NULL };
	int n = 0;

	for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]) && !n; i++)
		if (strstr(name, tags[i].tag))
			out[n++] = tags[i].region;
	for (int i = 0; base[i]; i++)
		if (!n || strcmp(out[0], base[i]))
			out[n++] = base[i];
	out[n] = NULL;
}

/* The active user's nuubOS language (description language preference). */
static void user_language(char *out, size_t size)
{
	char user[64] = "";
	char path[PATH_MAX];
	char lang[16];
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
	if (conf_value(path, "LANGUAGE", lang, sizeof(lang)) && strlen(lang) == 2)
		copy_text(out, size, lang);
}

/* ------------------------------------------------------------------ */
/* Settings, account and key                                           */
/* ------------------------------------------------------------------ */

static void load_account(struct account *a)
{
	memset(a, 0, sizeof(*a));
	conf_value(ACCOUNT_FILE, "USER", a->user, sizeof(a->user));
	conf_value(ACCOUNT_FILE, "PASSWORD", a->password, sizeof(a->password));
}

static void load_key(char *key, size_t size)
{
	key[0] = '\0';
	conf_value(TGDB_FILE, "APIKEY", key, size);
}

static bool provider_ready(int provider)
{
	return provider == PROV_LIBRETRO || access(SKYSCRAPER_BIN, X_OK) == 0;
}

/* The chosen source; without a choice ScreenScraper (when Skyscraper is
 * installed), else libretro. */
static int active_provider(void)
{
	char value[32];

	if (conf_value(SETTINGS_FILE, "PROVIDER", value, sizeof(value)))
		for (int i = 0; i < PROV_COUNT; i++)
			if (!strcmp(value, provider_ids[i]))
				return i;
	return provider_ready(PROV_SCREENSCRAPER) ? PROV_SCREENSCRAPER : PROV_LIBRETRO;
}

static int ensure_state_dir(void)
{
	return mkdir_p(SCRAPER_STATE, 0700);
}

/* ------------------------------------------------------------------ */
/* HTTP (libretro)                                                     */
/* ------------------------------------------------------------------ */

struct buffer {
	char *data;
	size_t len;
	size_t cap;
	size_t limit;
};

static size_t to_buffer(const void *ptr, size_t size, size_t n, void *userdata)
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

static size_t curl_to_buffer(char *ptr, size_t size, size_t n, void *userdata)
{
	return to_buffer(ptr, size, n, userdata);
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

	if (!curl)
		return -1;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_to_buffer);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, b);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	if (curl_easy_perform(curl) == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	return status;
}

/* ------------------------------------------------------------------ */
/* Media                                                               */
/* ------------------------------------------------------------------ */

static const char *image_ext(const unsigned char *d, size_t len)
{
	if (len > 8 && !memcmp(d, "\x89PNG", 4))
		return "png";
	if (len > 3 && d[0] == 0xff && d[1] == 0xd8)
		return "jpg";
	return NULL;
}

/* Store image data as <base>.<png|jpg> atomically; other extensions of
 * the same base are removed so a replaced image never shadows the new
 * one. */
static bool store_image(const char *data, size_t len, const char *base, char *path, size_t size)
{
	static const char *const exts[] = { "png", "jpg", "jpeg" };
	const char *ext = image_ext((const unsigned char *)data, len);
	char part[PATH_MAX + 8], dir[PATH_MAX], *slash;
	FILE *fp;
	bool ok;

	if (!ext)
		return false;
	snprintf(path, size, "%s.%s", base, ext);
	copy_text(dir, sizeof(dir), path);
	if ((slash = strrchr(dir, '/'))) {
		*slash = '\0';
		mkdir_p(dir, 0755);
	}
	snprintf(part, sizeof(part), "%s.part", path);
	fp = fopen(part, "wb");
	if (!fp)
		return false;
	ok = fwrite(data, 1, len, fp) == len;
	ok = (fflush(fp) == 0) && ok;
	ok = (fclose(fp) == 0) && ok;
	ok = ok && !cancelled && rename(part, path) == 0;
	if (!ok) {
		unlink(part);
		return false;
	}
	for (size_t i = 0; i < 3; i++)
		if (strcmp(exts[i], ext)) {
			snprintf(part, sizeof(part), "%s.%s", base, exts[i]);
			unlink(part);
		}
	return true;
}

static bool download_media(const char *url, const char *base, char *path, size_t size)
{
	struct buffer b = { .limit = 8 * 1024 * 1024 };
	bool ok = http_get(url, &b) == 200 && b.data && store_image(b.data, b.len, base, path, size);

	free(b.data);
	return ok;
}

static bool import_media(const char *file, const char *base, char *path, size_t size)
{
	struct buffer b = { .limit = 16 * 1024 * 1024 };
	char chunk[65536];
	size_t n;
	bool ok;
	FILE *fp = fopen(file, "rb");

	if (!fp)
		return false;
	while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0 && to_buffer(chunk, 1, n, &b) == n)
		;
	fclose(fp);
	ok = b.data && store_image(b.data, b.len, base, path, size);
	free(b.data);
	return ok;
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

#define META_FIELDS(X) \
	X(provider) X(provider_id) X(match) X(search) X(title) X(description) \
	X(release) X(developer) X(publisher) X(genre) X(players) X(rating) \
	X(screenshot) X(cover_url) X(shot_url) X(lang)

/* Localized fields kept per language. */
static bool localized_key(const char *key, size_t len)
{
	return (len == 11 && !strncmp(key, "description", 11)) || (len == 5 && !strncmp(key, "genre", 5));
}

/* Keeps "key@xx=value" unless xx is `skip` (the language rewritten now). */
static void keep_other_lang(struct metadata *m, const char *key, const char *lang, const char *value,
			    const char *skip)
{
	size_t used = strlen(m->other_langs);
	char probe[32];

	if (skip && !strcmp(lang, skip))
		return;
	snprintf(probe, sizeof(probe), "%s@%s=", key, lang);
	/* One value per key and language (the first kept). */
	for (const char *at = strstr(m->other_langs, probe); at; at = strstr(at + 1, probe))
		if (at == m->other_langs || at[-1] == '\n')
			return;
	snprintf(m->other_langs + used, sizeof(m->other_langs) - used, "%s@%s=%s\n", key, lang, value);
}

static bool read_metadata(const struct game_info *g, struct metadata *m)
{
	static char line[16384];
	char path[PATH_MAX];
	FILE *fp;

	memset(m, 0, sizeof(*m));
	meta_path(g, "txt", path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || !(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		{
			char *at = strchr(line, '@');

			if (at && localized_key(line, (size_t)(at - line)) && strlen(at + 1) == 2) {
				*at = '\0';
				keep_other_lang(m, line, at + 1, eq, NULL);
				continue;
			}
		}
#define READ(f) if (!strcmp(line, #f)) copy_text(m->f, sizeof(m->f), eq); else
		META_FIELDS(READ) {}
#undef READ
	}
	fclose(fp);
	return true;
}

static int write_metadata(const struct game_info *g, const struct metadata *m)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8], dir[PATH_MAX];
	FILE *fp;
	int rc = 0;

	snprintf(dir, sizeof(dir), META_ROOT "/%s", g->system);
	if (mkdir_p(dir, 0755) != 0)
		return -1;
	meta_path(g, "txt", path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "# nuubOS game metadata (provider-neutral)\n");
	/* Credential-free media references (cover_url, shot_url) for Accept. */
#define WRITE(f) if (m->f[0]) fprintf(fp, #f "=%s\n", m->f);
	META_FIELDS(WRITE)
#undef WRITE
	/* This run's language, then the others already fetched. */
	if (m->lang[0]) {
		if (m->description[0])
			fprintf(fp, "description@%s=%s\n", m->lang, m->description);
		if (m->genre[0])
			fprintf(fp, "genre@%s=%s\n", m->lang, m->genre);
	}
	{
		char probe_d[24], probe_g[24];
		const char *p = m->other_langs;

		snprintf(probe_d, sizeof(probe_d), "description@%s=", m->lang);
		snprintf(probe_g, sizeof(probe_g), "genre@%s=", m->lang);
		while (*p) {
			const char *nl = strchr(p, '\n');
			size_t len = nl ? (size_t)(nl - p) : strlen(p);

			if (!m->lang[0] || (strncmp(p, probe_d, strlen(probe_d)) && strncmp(p, probe_g, strlen(probe_g))))
				fprintf(fp, "%.*s\n", (int)len, p);
			p += len + (nl ? 1 : 0);
		}
	}
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

/* Data fields: the destination wins unless empty. */
static void merge_fields(struct metadata *dst, const struct metadata *src)
{
#define MERGE(f) if (!dst->f[0]) copy_text(dst->f, sizeof(dst->f), src->f);
	MERGE(title) MERGE(description) MERGE(release) MERGE(developer)
	MERGE(publisher) MERGE(genre) MERGE(players) MERGE(rating)
	MERGE(screenshot) MERGE(cover_url) MERGE(shot_url)
	MERGE(cover_file) MERGE(shot_file)
#undef MERGE
}

/* Data the active source can still add to this game. */
static bool missing_data(const struct game_info *g, const struct metadata *m, int provider)
{
	if (!cover_exists(g) || !m->screenshot[0] || access(m->screenshot, F_OK) != 0)
		return true;
	if (provider == PROV_LIBRETRO)
		return false;
	/* Any field a text source can give (user request 2026-10-10: a
	 * cover alone is not complete). */
	return !m->description[0] || !m->release[0] || !m->genre[0] ||
	       !m->developer[0] || !m->publisher[0] || !m->players[0];
}

/* Install media: files a source produced (Skyscraper) or references
 * (libretro, older ScreenScraper data) downloaded now. */
static void install_media(const struct game_info *g, struct metadata *m, bool replace_cover, bool replace_shot)
{
	char base[PATH_MAX - 8], path[PATH_MAX];
	bool want_cover = replace_cover || !cover_exists(g);
	bool want_shot = replace_shot || !m->screenshot[0] || access(m->screenshot, F_OK) != 0;

	if (want_cover) {
		snprintf(base, sizeof(base), COVERS_ROOT "/%s/%s", g->system, g->stem);
		if (m->cover_file[0])
			(void)import_media(m->cover_file, base, path, sizeof(path));
		else if (m->cover_url[0] && !strncmp(m->cover_url, LIBRETRO_BASE, strlen(LIBRETRO_BASE)))
			(void)download_media(m->cover_url, base, path, sizeof(path));
	}
	if (want_shot) {
		bool ok = false;

		snprintf(base, sizeof(base), SHOTS_ROOT "/%s/%s", g->system, g->stem);
		if (m->shot_file[0])
			ok = import_media(m->shot_file, base, path, sizeof(path));
		else if (m->shot_url[0] && !strncmp(m->shot_url, LIBRETRO_BASE, strlen(LIBRETRO_BASE)))
			ok = download_media(m->shot_url, base, path, sizeof(path));
		if (ok)
			copy_text(m->screenshot, sizeof(m->screenshot), path);
	}
}

/* ------------------------------------------------------------------ */
/* Source: Skyscraper (ScreenScraper, TheGamesDB)                      */
/* ------------------------------------------------------------------ */

/* One game of a Skyscraper run and what it returned. */
struct sky_item {
	const struct game_info *g;
	char file[PATH_MAX];
	bool found;
	struct metadata m;
};

/* INI string value: backslash before " and \. */
static void ini_quote(const char *in, char *out, size_t size)
{
	size_t o = 0;

	for (const char *p = in; *p && o + 3 < size; p++) {
		if (*p == '"' || *p == '\\')
			out[o++] = '\\';
		out[o++] = *p;
	}
	out[o] = '\0';
}

/* Skyscraper's home on tmpfs: its config.ini carries the user's account
 * and key and never reaches USERDATA. Raw cover and screenshot exports
 * (artwork.xml without layers), sizes for Home and the Details hero. */
static int sky_prepare(void)
{
	struct account a;
	char key[128], lang[8], q[300];
	char text[2048];
	size_t o;

	if (mkdir_p(SKY_HOME "/.skyscraper", 0700) != 0 || mkdir_p(CACHE_ROOT "/skyscraper", 0755) != 0)
		return -1;
	load_account(&a);
	load_key(key, sizeof(key));
	user_language(lang, sizeof(lang));
	o = (size_t)snprintf(text, sizeof(text),
		"[main]\n"
		"cacheFolder=\"" CACHE_ROOT "/skyscraper\"\n"
		"unattend=\"true\"\n"
		"spaceCheck=\"false\"\n"
		"videos=\"false\"\n"
		"cacheWheels=\"false\"\n"
		"cacheMarquees=\"false\"\n"
		"cacheTextures=\"false\"\n"
		"brackets=\"false\"\n"
		"theInFront=\"true\"\n"
		"maxLength=\"4000\"\n"
		/* A checksum match is right even when the file name carries
		 * another year (user report 2026-10-10: famous games missed,
		 * e.g. "(1993)" vs 1994 in ScreenScraper). */
		"ignoreYearInFilename=\"true\"\n"
		"lang=\"%s\"\n", lang);
	if (a.user[0]) {
		char creds[200];

		snprintf(creds, sizeof(creds), "%s:%s", a.user, a.password);
		ini_quote(creds, q, sizeof(q));
		o += (size_t)snprintf(text + o, sizeof(text) - o, "[screenscraper]\nuserCreds=\"%s\"\n", q);
	}
	if (key[0]) {
		ini_quote(key, q, sizeof(q));
		o += (size_t)snprintf(text + o, sizeof(text) - o, "[thegamesdb]\nuserCreds=\"%s\"\n", q);
	}
	if (o >= sizeof(text) || write_file(SKY_HOME "/.skyscraper/config.ini", text, true) != 0)
		return -1;
	return write_file(SKY_HOME "/.skyscraper/artwork.xml",
			  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			  "<artwork>\n"
			  "  <output type=\"cover\" height=\"720\"/>\n"
			  "  <output type=\"screenshot\" width=\"640\"/>\n"
			  "</artwork>\n", false);
}

/*
 * Run Skyscraper with argv, following its "#n/m, (found/missed)" lines as job
 * progress (offset + n of total) and its messages for the stop reason.
 * Returns the exit status, -1 when it could not run.
 */
static int sky_run(char *const argv[], size_t offset, size_t total)
{
	int pipefd[2], status = -1;
	pid_t pid;
	FILE *out;
	char line[4096];

	if (pipe(pipefd) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDONLY);

		dup2(devnull, 0);
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		setenv("HOME", SKY_HOME, 1);
		setenv("NO_COLOR", "1", 1);
		if (chdir(SKY_HOME) != 0)
			_exit(127);
		execv(SKYSCRAPER_BIN, argv);
		_exit(127);
	}
	close(pipefd[1]);
	out = fdopen(pipefd[0], "r");
	while (out && fgets(line, sizeof(line), out)) {
		unsigned n, m;

		if (cancelled) {
			kill(pid, SIGTERM);
			break;
		}
		/* "#n/m, (found/missed)" closes each game. */
		if (line[0] == '#' && sscanf(line, "#%u/%u", &n, &m) == 2 && n > 0 && total &&
		    strstr(line, ", ("))
			job_progress((int)((offset + n) * 100 / total), NULL);
		if (strstr(line, "request limit"))
			stop_reason = "quota";
		else if (strstr(line, "login error"))
			account_refused = true;
		else if (strstr(line, "API is currently closed") || strstr(line, "is 'thegamesdb' down") ||
			 strstr(line, "server is busy"))
			stop_reason = "busy";
		else if (strstr(line, "blacklisted"))
			stop_reason = "unavailable";
		else if (strstr(line, "Server response status"))
			stop_reason = "service";
	}
	if (out)
		fclose(out);
	else
		close(pipefd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		if (cancelled)
			kill(pid, SIGTERM);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The batch item of a generated "file:" path: the same path, else the
 * same file name (Skyscraper may normalise the folder part). */
static struct sky_item *pegasus_item(struct sky_item *items, size_t count, const char *file)
{
	const char *base = strrchr(file, '/');

	for (size_t i = 0; i < count; i++)
		if (!strcmp(items[i].file, file))
			return &items[i];
	base = base ? base + 1 : file;
	for (size_t i = 0; i < count; i++) {
		const char *b = strrchr(items[i].file, '/');

		if (!strcmp(b ? b + 1 : items[i].file, base))
			return &items[i];
	}
	return NULL;
}

/* One finished "key: value" of the list: "game" opens an entry (its
 * title), "file" names the batch item the following keys belong to. */
static void pegasus_pair(const char *key, char *value, struct sky_item *items, size_t count,
			 struct sky_item **cur, char *title, size_t title_size)
{
	struct metadata *m;
	char *p;

	/* The Pegasus list writes ':' as U+A789 (MODIFIER LETTER COLON) in
	 * values: back to ':'. */
	while ((p = strstr(value, "\xea\x9e\x89"))) {
		*p = ':';
		memmove(p + 1, p + 3, strlen(p + 3) + 1);
	}

	if (!strcmp(key, "game")) {
		*cur = NULL;
		copy_text(title, title_size, value);
		return;
	}
	if (!strcmp(key, "file")) {
		*cur = pegasus_item(items, count, value);
		if (*cur) {
			(*cur)->found = true;
			one_line(title, (*cur)->m.title, sizeof((*cur)->m.title));
		}
		return;
	}
	if (!*cur)
		return;
	m = &(*cur)->m;
	if (!strcmp(key, "description"))
		one_line(value, m->description, sizeof(m->description));
	else if (!strcmp(key, "release"))
		copy_text(m->release, sizeof(m->release), value);
	else if (!strcmp(key, "developer"))
		one_line(value, m->developer, sizeof(m->developer));
	else if (!strcmp(key, "publisher"))
		one_line(value, m->publisher, sizeof(m->publisher));
	else if (!strcmp(key, "genre"))
		one_line(value, m->genre, sizeof(m->genre));
	else if (!strcmp(key, "players"))
		one_line(value, m->players, sizeof(m->players));
	else if (!strcmp(key, "rating"))
		snprintf(m->rating, sizeof(m->rating), "%ld", strtol(value, NULL, 10));
	else if (!strcmp(key, "assets.boxFront"))
		copy_text(m->cover_file, sizeof(m->cover_file), value);
	else if (!strcmp(key, "assets.screenshot"))
		copy_text(m->shot_file, sizeof(m->shot_file), value);
}

/* Results of a generated Pegasus list into the batch. A value continues
 * on indented lines ("  ." stands for an empty line). */
static void pegasus_read(const char *path, struct sky_item *items, size_t count)
{
	static char line[16384], value[16384];
	char key[64] = "", title[512] = "";
	struct sky_item *cur = NULL;
	FILE *fp = fopen(path, "r");

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *colon;

		line[strcspn(line, "\r\n")] = '\0';
		if ((line[0] == ' ' || line[0] == '\t') && key[0]) {
			char *p = line;
			size_t len = strlen(value);

			while (*p == ' ' || *p == '\t')
				p++;
			if (!strcmp(p, "."))
				p = "";
			snprintf(value + len, sizeof(value) - len, "%s%s", len ? "\n" : "", p);
			continue;
		}
		if (key[0])
			pegasus_pair(key, value, items, count, &cur, title, sizeof(title));
		key[0] = '\0';
		colon = strchr(line, ':');
		if (!colon || (size_t)(colon - line) >= sizeof(key) || colon == line)
			continue;
		*colon = '\0';
		copy_text(key, sizeof(key), line);
		copy_text(value, sizeof(value), colon[1] == ' ' ? colon + 2 : colon + 1);
	}
	if (key[0])
		pegasus_pair(key, value, items, count, &cur, title, sizeof(title));
	fclose(fp);
}

/* Skyscraper's --query: words joined with '+'. */
static void sky_query(const char *name, char *out, size_t size)
{
	size_t o = 0;

	for (const char *p = name; *p && o + 1 < size; p++)
		out[o++] = *p == ' ' ? '+' : *p;
	out[o] = '\0';
}

/*
 * Gather data for a batch of games of one system into Skyscraper's cache
 * (-s module), then generate a Pegasus list of the batch into a scratch
 * folder next to the store and read it back. query: the user's search
 * name (single game only).
 */
static enum outcome sky_lookup(int provider, const struct system_map *s, struct sky_item *items, size_t count,
			       const char *query, size_t offset, size_t total)
{
	char outdir[PATH_MAX], media[PATH_MAX], list[PATH_MAX], pegasus[PATH_MAX + 32];
	char exts[1024] = "", q[600];
	char *argv[32];
	int argc, rc;
	FILE *fp;

	if (sky_prepare() != 0)
		return FAILED;
	snprintf(outdir, sizeof(outdir), CACHE_ROOT "/skyscraper-out/%d", (int)getpid());
	snprintf(media, sizeof(media), "%s/media", outdir);
	snprintf(list, sizeof(list), "%s/files.txt", outdir);
	remove_tree(outdir);
	if (mkdir_p(media, 0755) != 0)
		return FAILED;
	fp = fopen(list, "w");
	if (!fp)
		return FAILED;
	for (size_t i = 0; i < count; i++) {
		const char *dot = strrchr(items[i].file, '.');
		char ext[32];

		fprintf(fp, "%s\n", items[i].file);
		/* nuubOS may accept extensions Skyscraper does not list. */
		if (dot && strlen(dot) < sizeof(ext) - 2) {
			snprintf(ext, sizeof(ext), "*%s", dot);
			if (!strstr(exts, ext) && strlen(exts) + strlen(ext) + 2 < sizeof(exts))
				snprintf(exts + strlen(exts), sizeof(exts) - strlen(exts), "%s%s", exts[0] ? " " : "", ext);
		}
	}
	fclose(fp);

	/* Gathering. */
	argc = 0;
	argv[argc++] = "Skyscraper";
	argv[argc++] = "-p";
	argv[argc++] = (char *)s->sky;
	argv[argc++] = "-s";
	argv[argc++] = (char *)provider_ids[provider];
	argv[argc++] = "-i";
	argv[argc++] = ROMS_ROOT;
	argv[argc++] = "--flags";
	argv[argc++] = "unattend,nowheels,nomarquees,notextures";
	/* Another language: Skyscraper's cache holds the text of the last
	 * one, so the source is asked again. */
	if (refresh_cache) {
		argv[argc++] = "--cache";
		argv[argc++] = "refresh";
	}
	if (exts[0]) {
		argv[argc++] = "--addext";
		argv[argc++] = exts;
	}
	if (query && count == 1) {
		sky_query(query, q, sizeof(q));
		argv[argc++] = "--query";
		argv[argc++] = q;
		argv[argc++] = items[0].file;
	} else if (count == 1) {
		argv[argc++] = items[0].file;
	} else {
		argv[argc++] = "--includefrom";
		argv[argc++] = list;
	}
	argv[argc] = NULL;
	rc = sky_run(argv, offset, total);
	if (cancelled) {
		remove_tree(outdir);
		return STOP;
	}
	if (rc < 0 || rc == 127) {
		remove_tree(outdir);
		stop_reason = "unavailable";
		return STOP;
	}

	/* Game list generation from the cache: raw cover and screenshot. */
	argc = 0;
	argv[argc++] = "Skyscraper";
	argv[argc++] = "-p";
	argv[argc++] = (char *)s->sky;
	argv[argc++] = "-f";
	argv[argc++] = "pegasus";
	argv[argc++] = "-i";
	argv[argc++] = ROMS_ROOT;
	argv[argc++] = "-g";
	argv[argc++] = outdir;
	argv[argc++] = "-o";
	argv[argc++] = media;
	argv[argc++] = "--flags";
	argv[argc++] = "unattend,nobrackets";
	if (exts[0]) {
		argv[argc++] = "--addext";
		argv[argc++] = exts;
	}
	if (count == 1) {
		argv[argc++] = items[0].file;
	} else {
		argv[argc++] = "--includefrom";
		argv[argc++] = list;
	}
	argv[argc] = NULL;
	(void)sky_run(argv, 0, 0);
	snprintf(pegasus, sizeof(pegasus), "%s/metadata.pegasus.txt", outdir);
	pegasus_read(pegasus, items, count);
	{
		char lang[8];

		user_language(lang, sizeof(lang));
		for (size_t i = 0; i < count; i++)
			if (items[i].found) {
				copy_text(items[i].m.match, sizeof(items[i].m.match), "auto");
				copy_text(items[i].m.provider, sizeof(items[i].m.provider), provider_ids[provider]);
				if (items[i].m.description[0] || items[i].m.genre[0])
					copy_text(items[i].m.lang, sizeof(items[i].m.lang), lang);
			}
	}
	/* The scratch folder (and its media) goes once the batch is applied:
	 * the caller calls sky_done(). */
	return cancelled ? STOP : SCRAPED;
}

static void sky_done(void)
{
	char outdir[PATH_MAX];

	snprintf(outdir, sizeof(outdir), CACHE_ROOT "/skyscraper-out/%d", (int)getpid());
	remove_tree(outdir);
}

/* ------------------------------------------------------------------ */
/* Source: libretro thumbnails                                         */
/* ------------------------------------------------------------------ */

struct name_index {
	char folder[96];
	char *names;		/* NUL-separated */
	size_t count;
};

/* Thumbnail names of a folder (Named_Boxarts listing), one per line in
 * the cache, refreshed after a week. */
static bool libretro_index(const char *folder, struct name_index *ix)
{
	static struct name_index cache[4];
	char path[PATH_MAX], url[512];
	struct stat st;
	struct buffer b = { .limit = 16 * 1024 * 1024 };

	for (size_t i = 0; i < 4; i++)
		if (cache[i].names && !strcmp(cache[i].folder, folder)) {
			*ix = cache[i];
			return true;
		}
	snprintf(path, sizeof(path), CACHE_ROOT "/libretro/%s.txt", folder);
	if (stat(path, &st) != 0 || time(NULL) - st.st_mtime > LIBRETRO_INDEX_AGE || st.st_size == 0) {
		struct buffer list = { .limit = 16 * 1024 * 1024 };
		CURL *esc = curl_easy_init();
		char *e;
		long status;

		if (!esc)
			return false;
		e = curl_easy_escape(esc, folder, 0);
		snprintf(url, sizeof(url), LIBRETRO_BASE "/%s/Named_Boxarts/", e ? e : "");
		curl_free(e);
		status = http_get(url, &b);
		if (status != 200) {
			curl_easy_cleanup(esc);
			free(b.data);
			if (!cancelled)
				stop_reason = status < 0 ? "network" : status >= 500 ? "busy" : "service";
			return false;
		}
		/* Apache listing: href="<escaped name>.png" per thumbnail. */
		for (char *p = b.data; p && (p = strstr(p, "href=\"")); ) {
			char *start = p + 6, *end = strchr(start, '"');
			int len;
			char *dec;

			if (!end)
				break;
			p = end;
			if (end - start < 5 || strncmp(end - 4, ".png", 4) || *start == '?' || *start == '/')
				continue;
			dec = curl_easy_unescape(esc, start, (int)(end - start - 4), &len);
			if (dec && !memchr(dec, '\n', (size_t)len)) {
				to_buffer(dec, 1, (size_t)len, &list);
				to_buffer("\n", 1, 1, &list);
			}
			curl_free(dec);
		}
		curl_easy_cleanup(esc);
		free(b.data);
		memset(&b, 0, sizeof(b));
		if (!list.data || mkdir_p(CACHE_ROOT "/libretro", 0755) != 0 ||
		    write_file(path, list.data, false) != 0) {
			free(list.data);
			return false;
		}
		b = list;
	} else {
		FILE *fp = fopen(path, "r");
		char chunk[65536];
		size_t n;

		if (!fp)
			return false;
		while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0 && to_buffer(chunk, 1, n, &b) == n)
			;
		fclose(fp);
	}
	if (!b.data)
		return false;
	memset(ix, 0, sizeof(*ix));
	copy_text(ix->folder, sizeof(ix->folder), folder);
	ix->names = b.data;
	for (char *p = b.data; *p; p++)
		if (*p == '\n') {
			*p = '\0';
			ix->count++;
		}
	for (size_t i = 0; i < 4; i++)
		if (!cache[i].names) {
			cache[i] = *ix;
			break;
		}
	return true;
}

/* libretro thumbnail file names replace &*\/:`<>?\| with '_'. */
static void libretro_sanitize(const char *in, char *out, size_t size)
{
	size_t o = 0;

	for (const char *p = in; *p && o + 1 < size; p++)
		out[o++] = strchr("&*/:`<>?\\|", *p) ? '_' : *p;
	out[o] = '\0';
}

/* Lower is better: preferred region first, dumps of unreleased or altered
 * games last, fewer extra tags. */
static int libretro_score(const char *name, const char *const *regions)
{
	static const struct { const char *tag; const char *region; } tags[] = {
		{ "World", "wor" }, { "USA", "us" }, { "Europe", "eu" }, { "Japan", "jp" },
	};
	static const char *const bad[] = { "(Beta", "(Proto", "(Sample", "(Demo", "(Kiosk",
					   "(Pirate", "(Unl", "(Hack", "[BIOS]", "(Program", "(Promo",
					   "(Alt", "(Aftermarket", NULL };
	int score = 100;

	for (int r = 0; regions[r] && score == 100; r++)
		for (size_t t = 0; t < sizeof(tags) / sizeof(tags[0]); t++)
			if (!strcmp(tags[t].region, regions[r]) && strstr(name, tags[t].tag)) {
				score = r * 10;
				break;
			}
	for (int i = 0; bad[i]; i++)
		if (strstr(name, bad[i]))
			score += 1000;
	return score + (int)(strlen(name) / 8);
}

static enum outcome libretro_lookup(const struct game_info *g, const char *name, struct metadata *m)
{
	const struct system_map *s = system_of(g->system);
	const char *regions[8];
	char want[512], key[512], best[512] = "", folder_best[96] = "";
	int best_score = INT_MAX;
	bool exact = false;
	CURL *esc;

	region_order(g->rel, regions);
	libretro_sanitize(name, want, sizeof(want));
	name_key(want, key, sizeof(key));
	for (int f = 0; f < 2 && s->libretro[f] && !exact; f++) {
		struct name_index ix;
		const char *n;
		size_t off = 0;

		if (!libretro_index(s->libretro[f], &ix))
			return stop_reason[0] ? STOP : NOT_FOUND;
		for (size_t i = 0; i < ix.count; i++, off += strlen(n) + 1) {
			char k[512];
			int score;

			n = ix.names + off;
			if (!strcmp(n, want)) {
				copy_text(best, sizeof(best), n);
				copy_text(folder_best, sizeof(folder_best), ix.folder);
				exact = true;
				break;
			}
			/* Same title, any dump tags: the best scoring one. */
			if (!key[0])
				continue;
			name_key(n, k, sizeof(k));
			if (strcmp(k, key))
				continue;
			score = libretro_score(n, regions);
			if (score < best_score) {
				best_score = score;
				copy_text(best, sizeof(best), n);
				copy_text(folder_best, sizeof(folder_best), ix.folder);
			}
		}
	}
	if (!best[0])
		return NOT_FOUND;
	esc = curl_easy_init();
	if (!esc)
		return FAILED;
	{
		char *ef = curl_easy_escape(esc, folder_best, 0);
		char *en = curl_easy_escape(esc, best, 0);

		snprintf(m->cover_url, sizeof(m->cover_url), LIBRETRO_BASE "/%s/Named_Boxarts/%s.png", ef, en);
		snprintf(m->shot_url, sizeof(m->shot_url), LIBRETRO_BASE "/%s/Named_Snaps/%s.png", ef, en);
		curl_free(ef);
		curl_free(en);
	}
	curl_easy_cleanup(esc);
	/* Same title (comparison key): the thumbnails are the game's. */
	copy_text(m->match, sizeof(m->match), "auto");
	copy_text(m->provider, sizeof(m->provider), provider_ids[PROV_LIBRETRO]);
	return SCRAPED;
}

/* ------------------------------------------------------------------ */
/* Scraping                                                            */
/* ------------------------------------------------------------------ */

/* Last attempt at a game (any outcome): <rom name>.tried, its mtime. */
static void mark_tried(const struct game_info *g)
{
	char path[PATH_MAX], dir[PATH_MAX];

	snprintf(dir, sizeof(dir), META_ROOT "/%s", g->system);
	if (mkdir_p(dir, 0755) != 0)
		return;
	meta_path(g, "tried", path, sizeof(path));
	(void)write_file(path, "", false);
}

static bool tried_recently(const struct game_info *g)
{
	char path[PATH_MAX];
	struct stat st;

	if (skip_tried_after <= 0)
		return false;
	meta_path(g, "tried", path, sizeof(path));
	return stat(path, &st) == 0 && (long long)st.st_mtime >= skip_tried_after;
}

/* Whether a game takes part in this run; reads its current metadata. */
static bool wanted(const struct game_info *g, enum mode mode, int provider, struct metadata *old)
{
	char path[PATH_MAX];
	bool matched;

	read_metadata(g, old);
	matched = old->match[0] != '\0';
	skip_reason = "matched";
	if (!system_supported(provider, g->system)) {
		skip_reason = "system";
		return false;
	}
	if (mode == MODE_NEW && matched)
		return false;
	meta_path(g, "rejected", path, sizeof(path));
	if (mode != MODE_FORCE && access(path, F_OK) == 0)
		return false;
	if (mode == MODE_FILL && matched && !missing_data(g, old, provider)) {
		skip_reason = "complete";
		return false;
	}
	if (tried_recently(g)) {
		skip_reason = "recent";
		return false;
	}
	return true;
}

/* Combine what the source returned with what the game had, install media
 * and store. */
static enum outcome apply(const struct game_info *g, enum mode mode, struct metadata *old, struct metadata *fresh)
{
	struct metadata m;
	char path[PATH_MAX];

	copy_text(fresh->search, sizeof(fresh->search), old->search);
	if (mode == MODE_FILL && old->match[0]) {
		/* Only what is missing; everything already there stays. */
		m = *old;
		merge_fields(&m, fresh);
		copy_text(m.cover_file, sizeof(m.cover_file), fresh->cover_file);
		copy_text(m.shot_file, sizeof(m.shot_file), fresh->shot_file);
		copy_text(m.cover_url, sizeof(m.cover_url), fresh->cover_url);
		copy_text(m.shot_url, sizeof(m.shot_url), fresh->shot_url);
		install_media(g, &m, false, false);
		/* References of the source that answered: Accept stays right. */
		if (!old->cover_url[0] && !old->shot_url[0]) {
			m.cover_url[0] = '\0';
			m.shot_url[0] = '\0';
		}
	} else {
		/* New data first; what the source lacks keeps the old value (an
		 * Update never loses data). */
		m = *fresh;
		if (mode == MODE_FORCE)
			merge_fields(&m, old);
		if (!strcmp(m.match, "auto"))
			install_media(g, &m, mode == MODE_FORCE, mode == MODE_FORCE);
	}
	/* Languages already fetched stay (a cache: switching back costs no
	 * request); the previous plain text becomes its own language. */
	copy_text(m.other_langs, sizeof(m.other_langs), old->other_langs);
	if (old->lang[0] && strcmp(old->lang, m.lang)) {
		if (old->description[0])
			keep_other_lang(&m, "description", old->lang, old->description, m.lang);
		if (old->genre[0])
			keep_other_lang(&m, "genre", old->lang, old->genre, m.lang);
	}
	if (!m.lang[0] && old->lang[0] && !strcmp(m.description, old->description))
		copy_text(m.lang, sizeof(m.lang), old->lang);
	if (cancelled)
		return STOP;
	if (write_metadata(g, &m) != 0)
		return FAILED;
	meta_path(g, "rejected", path, sizeof(path));
	unlink(path);
	return !strcmp(m.match, "review") ? REVIEW : SCRAPED;
}

/* Scrape a batch of games of one system (offset/total: job progress). */
static void scrape_batch(int provider, const struct game_info *games, size_t count, enum mode mode,
			 size_t offset, size_t total, size_t *scraped, enum outcome *single)
{
	static struct sky_item items[MAX_BATCH];
	static struct metadata olds[MAX_BATCH];
	const struct system_map *s;
	size_t n = 0;
	const char *query = NULL;

	for (size_t i = 0; i < count; i++) {
		enum outcome o;

		if (!wanted(&games[i], mode, provider, &olds[n])) {
			if (single)
				*single = SKIPPED;
			continue;
		}
		if (provider == PROV_LIBRETRO) {
			struct metadata fresh;
			const char *name = olds[n].search[0] ? olds[n].search : games[i].stem;

			job_progress((int)((offset + i) * 100 / (total ? total : 1)), games[i].title);
			memset(&fresh, 0, sizeof(fresh));
			o = libretro_lookup(&games[i], name, &fresh);
			if (o == SCRAPED)
				o = apply(&games[i], mode, &olds[n], &fresh);
			if (o != STOP)
				mark_tried(&games[i]);
			if (o == SCRAPED || o == REVIEW)
				(*scraped)++;
			if (single)
				*single = o;
			if (o == STOP || cancelled)
				return;
			continue;
		}
		memset(&items[n], 0, sizeof(items[n]));
		items[n].g = &games[i];
		snprintf(items[n].file, sizeof(items[n].file), ROMS_ROOT "/%s", games[i].rel);
		if (olds[n].search[0])
			query = olds[n].search;
		n++;
	}
	if (provider == PROV_LIBRETRO || n == 0)
		return;
	s = system_of(games[0].system);
	/* A search name applies to its own game: such games go alone. */
	if (n > 1 && query) {
		for (size_t i = 0; i < n && !cancelled && !stop_reason[0]; i++) {
			struct sky_item one = items[i];
			struct metadata old = olds[i];

			(void)sky_lookup(provider, s, &one, 1, old.search[0] ? old.search : NULL, offset + i, total);
			if (one.found && apply(one.g, mode, &old, &one.m) != FAILED)
				(*scraped)++;
			if (!stop_reason[0])
				mark_tried(one.g);
			sky_done();
		}
		return;
	}
	(void)sky_lookup(provider, s, items, n, n == 1 ? query : NULL, offset, total);
	/* Games found before a stop (quota...) are kept. */
	for (size_t i = 0; i < n && !cancelled; i++) {
		enum outcome o = !items[i].found ? (stop_reason[0] ? STOP : NOT_FOUND)
			: apply(items[i].g, mode, &olds[i], &items[i].m);

		/* Found now, or not found and about to get its second chance;
		 * games a stop left untouched are not marked. */
		if (o != STOP)
			mark_tried(items[i].g);
		if (o == SCRAPED || o == REVIEW)
			(*scraped)++;
		if (single)
			*single = o;
	}
	sky_done();

	/*
	 * Second chance for what the file names did not find (user report
	 * 2026-10-09: many titles missed): the same source searched by a
	 * cleaned name, then libretro thumbnails (cover and screenshot) by the
	 * title key. Not for games with the user's own search name, nor after
	 * a stop (quota, network...).
	 */
	for (size_t i = 0; i < n && !cancelled && !stop_reason[0]; i++) {
		struct sky_item one;
		struct metadata fresh;
		char clean[512];
		enum outcome o;

		if (items[i].found || olds[i].search[0])
			continue;
		search_title(items[i].g->stem, clean, sizeof(clean));
		o = NOT_FOUND;
		if (clean[0] && strcmp(clean, items[i].g->stem)) {
			one = items[i];
			(void)sky_lookup(provider, s, &one, 1, clean, offset + i, total);
			if (one.found)
				o = apply(one.g, mode, &olds[i], &one.m);
			sky_done();
		}
		/* TheGamesDB by name: description, dates and genre where
		 * ScreenScraper has nothing (user report 2026-10-10: games found
		 * by libretro had a cover and nothing else). */
		if (o == NOT_FOUND && !stop_reason[0] && provider == PROV_SCREENSCRAPER &&
		    system_supported(PROV_THEGAMESDB, items[i].g->system)) {
			one = items[i];
			memset(&one.m, 0, sizeof(one.m));
			one.found = false;
			(void)sky_lookup(PROV_THEGAMESDB, s, &one, 1, clean[0] ? clean : items[i].g->stem, offset + i, total);
			if (one.found)
				o = apply(one.g, mode, &olds[i], &one.m);
			sky_done();
			/* TheGamesDB's failures never stop the run. */
			stop_reason = "";
		}
		if (o == NOT_FOUND && !stop_reason[0] && s->libretro[0]) {
			memset(&fresh, 0, sizeof(fresh));
			if (libretro_lookup(items[i].g, clean[0] ? clean : items[i].g->stem, &fresh) == SCRAPED)
				o = apply(items[i].g, mode, &olds[i], &fresh);
		}
		if (o == SCRAPED || o == REVIEW)
			(*scraped)++;
		if (single && (o == SCRAPED || o == REVIEW))
			*single = o;
	}
}

static int cmd_game(const char *id, enum mode mode)
{
	static struct game_info g;
	int provider = active_provider();
	enum outcome o = FAILED;
	size_t scraped = 0;

	if (!valid_game(id) || !game_details(id, &g)) {
		job_error("game");
		return 1;
	}
	if (!provider_ready(provider)) {
		job_error("unavailable");
		return 1;
	}
	job_progress(-1, g.title);
	scrape_batch(provider, &g, 1, mode, 0, 0, &scraped, &o);
	library_rescan();
	if (cancelled)
		o = STOP;
	switch (o) {
	case SCRAPED:
	case REVIEW:
		if (account_refused) {
			/* Data came, but the account needs attention. */
			job_error("auth");
			return 1;
		}
		printf(as_job ? "@result %s\n" : "OK %s\n", o == REVIEW ? "review" : "scraped");
		return 0;
	case NOT_FOUND:
		job_error(stop_reason[0] ? stop_reason : "not-found");
		return 1;
	case SKIPPED:
		job_error(skip_reason);
		return 1;
	case STOP:
		job_error(cancelled ? "cancelled" : stop_reason[0] ? stop_reason : "service");
		return 1;
	default:
		job_error("storage");
		return 1;
	}
}

/* A ScreenScraper/TheGamesDB match without text in the user's language. */
static bool needs_language(const char *id)
{
	struct game_info g;
	struct metadata m;
	char lang[8], probe[24];

	if (!game_details(id, &g) || !read_metadata(&g, &m) || !m.match[0] || !strcmp(m.provider, "libretro"))
		return false;
	user_language(lang, sizeof(lang));
	if (!strcmp(m.lang, lang))
		return false;
	snprintf(probe, sizeof(probe), "description@%s=", lang);
	return strstr(m.other_langs, probe) == NULL;
}

static int cmd_bulk(const char *scope)
{
	static char status[262144];
	static char list[262144];
	static char ids[MAX_BATCH][17];
	static char systems_of[MAX_BATCH][32];
	static struct game_info games[MAX_BATCH];
	int provider = active_provider();
	enum mode mode = !strncmp(scope, "missing", 7) ? MODE_FILL : !strcmp(scope, "lang") ? MODE_FORCE : MODE_NEW;
	size_t count = 0, scraped = 0, done = 0;
	char *line, *save = NULL;

	if (!strncmp(scope, "missing:", 8)) {
		long long seconds = atoll(scope + 8);

		if (seconds > 0)
			skip_tried_after = (long long)time(NULL) - seconds;
	} else if (strcmp(scope, "all") && strcmp(scope, "missing") && strcmp(scope, "lang") &&
		   strncmp(scope, "system:", 7)) {
		job_error("scope");
		return 1;
	}
	if (!provider_ready(provider)) {
		job_error("unavailable");
		return 1;
	}
	if (!strcmp(scope, "lang")) {
		if (provider == PROV_LIBRETRO) {
			job_error("unavailable");
			return 1;
		}
		refresh_cache = true;
	}
	if (library_request("STATUS\n", status, sizeof(status)) != 0) {
		job_error("library");
		return 1;
	}
	for (line = strtok_r(status, "\n", &save); line && count < MAX_BATCH;
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
		if (!system_supported(provider, system))
			continue;
		snprintf(cmd, sizeof(cmd), "GAMES\tsystem:%s\n", system);
		if (library_request(cmd, list, sizeof(list)) != 0)
			continue;
		for (l2 = strtok_r(list, "\n", &save2); l2 && count < MAX_BATCH;
		     l2 = strtok_r(NULL, "\n", &save2)) {
			char *f = l2;
			int field = 0;

			if (strncmp(l2, "game=", 5) || strlen(l2) < 21)
				continue;
			/* Unavailable games cannot be identified: skip them. */
			for (char *p = l2; *p; p++)
				if (*p == '\t' && ++field == 7) {
					f = p + 1;
					break;
				}
			if (field == 7 && f[0] == '0')
				continue;
			snprintf(ids[count], 17, "%.16s", l2 + 5);
			copy_text(systems_of[count], sizeof(systems_of[count]), system);
			/* lang: matched games whose text is not in this language. */
			if (!strcmp(scope, "lang") && !needs_language(ids[count]))
				continue;
			count++;
		}
	}
	/* One batch per system (one Skyscraper run each). */
	for (size_t start = 0; start < count && !cancelled && !stop_reason[0];) {
		size_t end = start, n = 0;

		while (end < count && !strcmp(systems_of[end], systems_of[start]))
			end++;
		for (size_t i = start; i < end; i++)
			if (game_details(ids[i], &games[n]))
				n++;
		job_progress((int)(done * 100 / count), NULL);
		scrape_batch(provider, games, n, mode, done, count, &scraped, NULL);
		done += end - start;
		start = end;
	}
	library_rescan();
	if (cancelled || stop_reason[0]) {
		job_error(cancelled ? "cancelled" : stop_reason);
		return 1;
	}
	if (account_refused) {
		job_error("auth");
		return 1;
	}
	printf(as_job ? "@result %zu 0 %zu\n" : "OK %zu 0 %zu\n", scraped, count);
	return 0;
}

static int cmd_accept(const char *id)
{
	struct game_info g;
	struct metadata m;

	if (!valid_game(id) || !game_details(id, &g)) {
		puts("ERR game");
		return 1;
	}
	if (!read_metadata(&g, &m) || !m.match[0]) {
		puts("ERR no-match");
		return 1;
	}
	copy_text(m.match, sizeof(m.match), "user");
	install_media(&g, &m, false, false);
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
	static const char *const exts[] = { "png", "jpg", "jpeg" };
	struct game_info g;
	struct metadata m;
	char path[PATH_MAX];
	bool had;

	if (!valid_game(id) || !game_details(id, &g)) {
		puts("ERR game");
		return 1;
	}
	had = read_metadata(&g, &m);
	/* Scraped media goes with the metadata; user-supplied covers next
	 * to the ROMs (media/covers) are never touched. */
	if (had && !strncmp(m.screenshot, SHOTS_ROOT "/", strlen(SHOTS_ROOT) + 1))
		unlink(m.screenshot);
	if (!reject && had && m.provider[0])
		for (size_t i = 0; i < 3; i++) {
			snprintf(path, sizeof(path), COVERS_ROOT "/%s/%s.%s", g.system, g.stem, exts[i]);
			unlink(path);
		}
	meta_path(&g, "txt", path, sizeof(path));
	unlink(path);
	/* The user's search name survives a rejected match. */
	if (reject && had && m.search[0]) {
		struct metadata keep;

		memset(&keep, 0, sizeof(keep));
		copy_text(keep.search, sizeof(keep.search), m.search);
		(void)write_metadata(&g, &keep);
	}
	if (reject) {
		FILE *fp;

		snprintf(path, sizeof(path), META_ROOT "/%s", g.system);
		mkdir_p(path, 0755);
		meta_path(&g, "rejected", path, sizeof(path));
		fp = fopen(path, "w");
		if (fp)
			fclose(fp);
	}
	library_rescan();
	puts("OK");
	return 0;
}

/* The name used to look the game up instead of its file name. */
static int cmd_search(const char *id)
{
	struct game_info g;
	struct metadata m;
	char name[256] = "", clean[256];
	char path[PATH_MAX];

	if (!valid_game(id) || !game_details(id, &g)) {
		puts("ERR game");
		return 1;
	}
	if (fgets(name, sizeof(name), stdin))
		name[strcspn(name, "\r\n")] = '\0';
	trim(name);
	one_line(name, clean, sizeof(clean));
	read_metadata(&g, &m);
	copy_text(m.search, sizeof(m.search), clean);
	if (!m.search[0] && !m.match[0]) {
		/* Nothing left but the search name: no metadata file. */
		meta_path(&g, "txt", path, sizeof(path));
		unlink(path);
	} else if (write_metadata(&g, &m) != 0) {
		puts("ERR storage");
		return 1;
	}
	/* A new name gets a new chance. */
	meta_path(&g, "rejected", path, sizeof(path));
	unlink(path);
	library_rescan();
	puts("OK");
	return 0;
}

static void print_status(void)
{
	struct account a;
	char key[128];
	int provider = active_provider();
	size_t klen;

	load_account(&a);
	load_key(key, sizeof(key));
	klen = strlen(key);
	printf("provider=%s\navailable=%d\n", provider_ids[provider], provider_ready(provider) ? 1 : 0);
	for (int i = 0; i < PROV_COUNT; i++)
		printf("%s=%s\n", provider_ids[i], provider_ready(i) ? "available" : "unavailable");
	/* Only the end of the key, to recognise it. */
	printf("user=%s\napikey=%s\nend=1\n", a.user, klen > 4 ? key + klen - 4 : "");
}

static int cmd_account(int argc, char **argv)
{
	struct account a;
	char text[256];

	if (argc == 3 && !strcmp(argv[2], "status")) {
		print_status();
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
		if (strlen(argv[3]) >= sizeof(a.user) || strpbrk(argv[3], "=:\n\r")) {
			puts("ERR name");
			return 1;
		}
		copy_text(a.user, sizeof(a.user), argv[3]);
		if (!fgets(a.password, sizeof(a.password), stdin)) {
			puts("ERR password");
			return 1;
		}
		a.password[strcspn(a.password, "\r\n")] = '\0';
		if (!a.password[0]) {
			puts("ERR password");
			return 1;
		}
		/* Checked by ScreenScraper at the next scraping (Skyscraper
		 * reports a refused login, shown as "check the account"). */
		snprintf(text, sizeof(text), "USER=%s\nPASSWORD=%s\n", a.user, a.password);
		if (ensure_state_dir() != 0 || write_file(ACCOUNT_FILE, text, true) != 0) {
			puts("ERR storage");
			return 1;
		}
		puts("OK");
		return 0;
	}
	fprintf(stderr, "Usage: nuubos-scraper account status|clear|set NAME\n");
	return 2;
}

static int cmd_apikey(int argc, char **argv)
{
	char key[128] = "", text[160];

	if (argc == 3 && !strcmp(argv[2], "clear")) {
		if (unlink(TGDB_FILE) != 0 && errno != ENOENT) {
			puts("ERR storage");
			return 1;
		}
		puts("OK");
		return 0;
	}
	if (argc != 3 || strcmp(argv[2], "set")) {
		fprintf(stderr, "Usage: nuubos-scraper apikey set|clear\n");
		return 2;
	}
	if (fgets(key, sizeof(key), stdin))
		key[strcspn(key, "\r\n")] = '\0';
	trim(key);
	if (!key[0] || strpbrk(key, "=: \t\"")) {
		puts("ERR key");
		return 1;
	}
	snprintf(text, sizeof(text), "APIKEY=%s\n", key);
	if (ensure_state_dir() != 0 || write_file(TGDB_FILE, text, true) != 0) {
		puts("ERR storage");
		return 1;
	}
	puts("OK");
	return 0;
}

static int cmd_provider(int argc, char **argv)
{
	char text[64];

	if (argc == 4 && !strcmp(argv[2], "set"))
		for (int i = 0; i < PROV_COUNT; i++)
			if (!strcmp(argv[3], provider_ids[i])) {
				snprintf(text, sizeof(text), "PROVIDER=%s\n", provider_ids[i]);
				if (ensure_state_dir() != 0 || write_file(SETTINGS_FILE, text, false) != 0) {
					puts("ERR storage");
					return 1;
				}
				puts("OK");
				return 0;
			}
	if (argc == 3 && !strcmp(argv[2], "status")) {
		print_status();
		return 0;
	}
	fprintf(stderr, "Usage: nuubos-scraper provider status|set screenscraper|thegamesdb|libretro\n");
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
	signal(SIGPIPE, SIG_IGN);
	as_job = getenv("NUUBOS_JOB_ID") != NULL;
	curl_global_init(CURL_GLOBAL_DEFAULT);

	if (argc >= 3 && !strcmp(argv[1], "game"))
		rc = cmd_game(argv[2], argc < 4 ? MODE_NEW : !strcmp(argv[3], "force") ? MODE_FORCE
			      : !strcmp(argv[3], "fill") ? MODE_FILL : MODE_NEW);
	else if (argc == 3 && !strcmp(argv[1], "bulk"))
		rc = cmd_bulk(argv[2]);
	else if (argc == 3 && !strcmp(argv[1], "search"))
		rc = cmd_search(argv[2]);
	else if (argc == 3 && !strcmp(argv[1], "accept"))
		rc = cmd_accept(argv[2]);
	else if (argc == 3 && !strcmp(argv[1], "reject"))
		rc = cmd_reject_or_clear(argv[2], true);
	else if (argc == 3 && !strcmp(argv[1], "clear"))
		rc = cmd_reject_or_clear(argv[2], false);
	else if (argc >= 3 && !strcmp(argv[1], "account"))
		rc = cmd_account(argc, argv);
	else if (argc >= 3 && !strcmp(argv[1], "apikey"))
		rc = cmd_apikey(argc, argv);
	else if (argc >= 3 && !strcmp(argv[1], "provider"))
		rc = cmd_provider(argc, argv);
	else
		fprintf(stderr, "Usage: nuubos-scraper game GAME [force|fill] | bulk all|missing|system:ID | "
				"search GAME | accept|reject|clear GAME | provider status|set NAME | "
				"account status|clear|set NAME | apikey set|clear\n");
	curl_global_cleanup();
	return rc;
}
