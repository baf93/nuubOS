/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-library-import — add games from any folder (USB drive, network
 * location, /userdata) to the game library (EPIC-012).
 *
 *   plan SRC            what importing SRC (a file or a folder) does:
 *                         files=<n> bytes=<n> skipped=<n> asks=<n>
 *                         place<TAB><system><TAB><files>      (one per system)
 *                         ask<TAB><n><TAB><label><TAB><files><TAB><system,...>
 *                         cand<TAB><n><TAB><system><TAB><display name>
 *   run SRC [ANSWERS]   the import, as the job library-import: ANSWERS =
 *                       "<n>=<system>,..." for the ask lines of plan (an
 *                       unanswered ask is skipped). Reports @progress and
 *                       @result <copied> <already there> <skipped>, then
 *                       asks libraryd to scan.
 *
 * Placement follows the Web administration's Add Games (pages.js
 * placeGames) so both behave alike:
 *   - a folder named like a system (id, folder, name or alias) gives its
 *     files that system; the path below it is kept;
 *   - a disc set is an index and the files it names: .cue -> tracks,
 *     .gdi -> tracks, .ccd -> .img/.sub, .m3u (multi-disc games such as
 *     PS1, floppy sets) -> discs, nested (.m3u -> .cue -> .bin) and in
 *     subfolders; tracks an index does not name follow the first index of
 *     their folder. A set goes as a whole to the one system taking its
 *     index and its discs, or the one its first track's header names
 *     (libraryd's sniff_system strings); otherwise it is asked once;
 *   - any other file goes to the only system taking its extension (or
 *     the one its header names), several candidates are asked once per
 *     extension, no candidate = skipped.
 * Files keep their folder below SRC's parent (a selected folder keeps its
 * name). Nothing is ever overwritten: a file already at its destination
 * counts as already there. Copies go through "<name>.part" + rename.
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef REGISTRY_PATH
#define REGISTRY_PATH "/usr/share/nuubos/systems.conf"
#endif
#ifndef LIBRARY_SOCKET
#define LIBRARY_SOCKET "/run/nuubos/libraryd.sock"
#endif

#define MAX_SYSTEMS 256
#define MAX_FILES 20000
#define MAX_DEPTH 16
#define INDEX_MAX (256 * 1024)
#define SNIFF_BYTES 40960
#define SPACE_MARGIN (16LL << 20)

struct system {
	char id[32];
	char name[96];
	char folder[64];
	char exts[512];
	char aliases[256];
};

struct file {
	char *abs;
	char *rel;      /* folder below SRC's parent ("" = none) */
	char *name;
	char *key;      /* lower-case rel/name, to match index entries */
	long long size;
	int sys;        /* system index, -1 unknown */
	int owner;      /* index file claiming it, -1 none */
	int ask;        /* ask group, -1 none */
	bool placed;    /* system decided by a folder name */
	bool skipped;
};

struct ask {
	char key[PATH_MAX + 8];
	int first;
	int count;
	bool cand[MAX_SYSTEMS];
};

static struct system systems[MAX_SYSTEMS];
static int system_count;
static struct file *files;
static int file_count;
static struct ask *asks;
static int ask_count;
static int skipped_count;
static volatile sig_atomic_t cancelled;

static const char *const disc_index[] = { "cue", "m3u", "gdi", "ccd" };
static const char *const disc_tracks[] = { "bin", "img", "sub", "iso", "raw", "wav" };
static const struct { const char *magic; const char *system; } disc_magic[] = {
	{ "PLAYSTATION", "psx" },
	{ "SEGASATURN", "saturn" },
	{ "SEGADISCSYSTEM", "segacd" },
	{ "SEGAKATANA", "dreamcast" },
};

static void on_term(int sig)
{
	(void)sig;
	cancelled = 1;
}

static char *xstrdup(const char *s)
{
	char *d = strdup(s);

	if (!d) {
		printf("@error memory\n");
		exit(1);
	}
	return d;
}

static void lower(char *s)
{
	for (; *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

static const char *ext_of(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && dot != name ? dot + 1 : "";
}

static bool ext_in(const char *name, const char *const *list, size_t n)
{
	const char *e = ext_of(name);

	for (size_t i = 0; i < n; i++)
		if (!strcasecmp(e, list[i]))
			return true;
	return false;
}

#define IS_INDEX(n) ext_in((n), disc_index, sizeof(disc_index) / sizeof(disc_index[0]))
#define IS_TRACK(n) ext_in((n), disc_tracks, sizeof(disc_tracks) / sizeof(disc_tracks[0]))

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

static void copy_field(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static bool load_registry(void)
{
	FILE *fp = fopen(REGISTRY_PATH, "r");
	char line[2048];

	if (!fp)
		return false;
	while (system_count < MAX_SYSTEMS && fgets(line, sizeof(line), fp)) {
		char *f[7] = { 0 };
		char *p = line;
		int n = 0;

		line[strcspn(line, "\r\n")] = '\0';
		if (!line[0] || line[0] == '#')
			continue;
		while (n < 7) {
			f[n++] = p;
			p = strchr(p, '|');
			if (!p)
				break;
			*p++ = '\0';
		}
		if (n < 4 || !f[0][0])
			continue;
		struct system *s = &systems[system_count++];
		copy_field(s->id, sizeof(s->id), f[0]);
		copy_field(s->name, sizeof(s->name), f[1]);
		copy_field(s->folder, sizeof(s->folder), f[2]);
		/* ",ext1,ext2," so a suffix test can look for ",ext," */
		snprintf(s->exts, sizeof(s->exts), ",%s,", f[3]);
		copy_field(s->aliases, sizeof(s->aliases), n > 6 ? f[6] : "");
	}
	fclose(fp);
	return system_count > 0;
}

/* Registry extensions may have several dots ("p8.png"). */
static bool accepts(int sys, const char *name)
{
	const char *p = systems[sys].exts + 1;
	size_t len = strlen(name);

	while (*p) {
		const char *end = strchr(p, ',');
		size_t n = end ? (size_t)(end - p) : strlen(p);

		if (n && len > n + 1 && name[len - n - 1] == '.' && !strncasecmp(name + len - n, p, n))
			return true;
		if (!end)
			break;
		p = end + 1;
	}
	return false;
}

static int system_by_id(const char *id)
{
	for (int i = 0; i < system_count; i++)
		if (!strcmp(systems[i].id, id))
			return i;
	return -1;
}

/* A folder name that names a system: id, folder, display name or alias. */
static int system_by_folder(const char *dir)
{
	for (int i = 0; i < system_count; i++) {
		const struct system *s = &systems[i];
		const char *a = s->aliases;

		if (!strcasecmp(dir, s->id) || !strcasecmp(dir, s->folder) || !strcasecmp(dir, s->name))
			return i;
		while (*a) {
			const char *end = strchr(a, ',');
			size_t n = end ? (size_t)(end - a) : strlen(a);

			if (n && strlen(dir) == n && !strncasecmp(dir, a, n))
				return i;
			if (!end)
				break;
			a = end + 1;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Walking SRC                                                         */
/* ------------------------------------------------------------------ */

static void add_file(const char *abs, const char *rel, const char *name, long long size)
{
	static int cap;
	struct file *f;
	char key[PATH_MAX * 2];

	if (file_count >= MAX_FILES)
		return;
	if (file_count == cap) {
		cap = cap ? cap * 2 : 256;
		files = realloc(files, (size_t)cap * sizeof(*files));
		if (!files) {
			printf("@error memory\n");
			exit(1);
		}
	}
	f = &files[file_count++];
	memset(f, 0, sizeof(*f));
	f->abs = xstrdup(abs);
	f->rel = xstrdup(rel);
	f->name = xstrdup(name);
	snprintf(key, sizeof(key), "%s%s%s", rel, rel[0] ? "/" : "", name);
	lower(key);
	f->key = xstrdup(key);
	f->size = size;
	f->sys = -1;
	f->owner = -1;
	f->ask = -1;
}

static void walk(const char *abs, const char *rel, int depth)
{
	struct dirent **list = NULL;
	int n;

	if (depth > MAX_DEPTH)
		return;
	n = scandir(abs, &list, NULL, alphasort);
	for (int i = 0; i < n; i++) {
		const char *name = list[i]->d_name;
		char path[PATH_MAX], sub[PATH_MAX];
		struct stat st;

		if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "lost+found") ||
		    snprintf(path, sizeof(path), "%s/%s", abs, name) >= (int)sizeof(path) ||
		    stat(path, &st) != 0) {
			free(list[i]);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", name);
			walk(path, sub, depth + 1);
		} else if (S_ISREG(st.st_mode) && name[0] != '.' &&
			   !(strlen(name) > 5 && !strcmp(name + strlen(name) - 5, ".part"))) {
			add_file(path, rel, name, (long long)st.st_size);
		}
		free(list[i]);
	}
	free(list);
}

static bool collect(const char *src)
{
	char abs[PATH_MAX];
	struct stat st;
	const char *slash;

	if (!realpath(src, abs) || stat(abs, &st) != 0)
		return false;
	slash = strrchr(abs, '/');
	if (S_ISDIR(st.st_mode))
		walk(abs, slash ? slash + 1 : abs, 0);
	else if (S_ISREG(st.st_mode))
		add_file(abs, "", slash ? slash + 1 : abs, (long long)st.st_size);
	else
		return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* Placement                                                           */
/* ------------------------------------------------------------------ */

/* Folder named like a system: that system, the path below it kept. */
static void place_by_folder(struct file *f)
{
	char *p = f->rel;

	while (*p) {
		char *end = strchr(p, '/');
		char part[NAME_MAX + 1];
		size_t n = end ? (size_t)(end - p) : strlen(p);
		int sys;

		snprintf(part, sizeof(part), "%.*s", (int)n, p);
		sys = system_by_folder(part);
		if (sys >= 0) {
			f->sys = sys;
			f->placed = true;
			memmove(f->rel, end ? end + 1 : p + n, strlen(end ? end + 1 : p + n) + 1);
			return;
		}
		if (!end)
			break;
		p = end + 1;
	}
}

/* dir + a path from an index ('.', '..', backslashes), lower case. */
static void join_ref(char *out, size_t size, const char *dir, const char *ref)
{
	char buf[PATH_MAX * 2];
	char *parts[256];
	int n = 0;
	char *save = NULL;

	snprintf(buf, sizeof(buf), "%s/%s", dir, ref);
	for (char *c = buf; *c; c++)
		if (*c == '\\')
			*c = '/';
	for (char *t = strtok_r(buf, "/", &save); t && n < 256; t = strtok_r(NULL, "/", &save)) {
		if (!strcmp(t, "."))
			continue;
		if (!strcmp(t, "..")) {
			if (n)
				n--;
			continue;
		}
		parts[n++] = t;
	}
	out[0] = '\0';
	for (int i = 0; i < n; i++) {
		size_t len = strlen(out);

		snprintf(out + len, size - len, "%s%s", i ? "/" : "", parts[i]);
	}
	lower(out);
}

static int file_by_key(const char *key)
{
	for (int i = 0; i < file_count; i++)
		if (!files[i].placed && !strcmp(files[i].key, key))
			return i;
	return -1;
}

static void claim(int index, const char *ref)
{
	char key[PATH_MAX * 2];
	int k;

	join_ref(key, sizeof(key), files[index].rel, ref);
	k = file_by_key(key);
	if (k >= 0 && k != index && files[k].owner < 0)
		files[k].owner = index;
}

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

/* FILE "name" TYPE | FILE name TYPE (cue); the 5th field (gdi). */
static char *quoted_or_word(char *p, char **end)
{
	char *e;

	while (isspace((unsigned char)*p))
		p++;
	if (*p == '"') {
		e = strchr(++p, '"');
	} else {
		e = p;
		while (*e && !isspace((unsigned char)*e))
			e++;
	}
	if (!e || e == p)
		return NULL;
	*end = e;
	return p;
}

static void claim_index_refs(int i)
{
	const struct file *f = &files[i];
	const char *ext = ext_of(f->name);
	char *text, *save = NULL;
	FILE *fp;
	size_t n;

	if (!strcasecmp(ext, "ccd")) {
		char ref[NAME_MAX + 8];
		int base = (int)(ext - f->name) - 1;

		snprintf(ref, sizeof(ref), "%.*s.img", base, f->name);
		claim(i, ref);
		snprintf(ref, sizeof(ref), "%.*s.sub", base, f->name);
		claim(i, ref);
		return;
	}
	if (f->size > INDEX_MAX || !(fp = fopen(f->abs, "r")))
		return;
	text = calloc(1, INDEX_MAX + 1);
	n = text ? fread(text, 1, INDEX_MAX, fp) : 0;
	fclose(fp);
	if (!text)
		return;
	text[n] = '\0';
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *l = trim(line), *end = NULL, *name;

		if (!*l)
			continue;
		if (!strcasecmp(ext, "m3u")) {
			if (*l == '#')
				continue;
			l[strcspn(l, "|")] = '\0';
			claim(i, trim(l));
		} else if (!strcasecmp(ext, "cue")) {
			if (strncasecmp(l, "FILE", 4) || !isspace((unsigned char)l[4]))
				continue;
			if ((name = quoted_or_word(l + 4, &end))) {
				*end = '\0';
				claim(i, name);
			}
		} else if (!strcasecmp(ext, "gdi")) {
			char *p = l;
			int fields = 0;

			while (fields < 4) {
				while (isdigit((unsigned char)*p))
					p++;
				if (!isspace((unsigned char)*p))
					break;
				while (isspace((unsigned char)*p))
					p++;
				fields++;
			}
			if (fields == 4 && (name = quoted_or_word(p, &end))) {
				*end = '\0';
				claim(i, name);
			}
		}
	}
	free(text);
}

static int top_of(int i)
{
	int steps = 0;

	while (files[i].owner >= 0 && steps++ < file_count)
		i = files[i].owner;
	return i;
}

/* The system a disc or cartridge header names, -1 if none. */
static int sniff(const struct file *f)
{
	unsigned char buf[SNIFF_BYTES];
	ssize_t n;
	int fd = open(f->abs, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n <= 0)
		return -1;
	for (size_t i = 0; i < sizeof(disc_magic) / sizeof(disc_magic[0]); i++)
		if (memmem(buf, (size_t)n, disc_magic[i].magic, strlen(disc_magic[i].magic)))
			return system_by_id(disc_magic[i].system);
	if (!strcasecmp(ext_of(f->name), "bin") && n > 0x104 && !memcmp(buf + 0x100, "SEGA", 4))
		return system_by_id("megadrive");
	return -1;
}

static int new_ask(const char *key, int first)
{
	static int cap;

	for (int i = 0; i < ask_count; i++)
		if (!strcmp(asks[i].key, key))
			return i;
	if (ask_count == cap) {
		cap = cap ? cap * 2 : 16;
		asks = realloc(asks, (size_t)cap * sizeof(*asks));
		if (!asks) {
			printf("@error memory\n");
			exit(1);
		}
	}
	memset(&asks[ask_count], 0, sizeof(asks[0]));
	snprintf(asks[ask_count].key, sizeof(asks[0].key), "%s", key);
	asks[ask_count].first = first;
	return ask_count++;
}

/* Decide a system for a group of files (a disc set or one file), or an
 * ask, or skip it. members[0] is the index or the file. */
static void decide(const int *members, int count, bool set)
{
	bool cand[MAX_SYSTEMS];
	int ncand = 0, only = -1, track = -1;
	char key[PATH_MAX + 8];

	for (int s = 0; s < system_count; s++) {
		bool ok = true;

		for (int m = 0; m < count && ok; m++) {
			const char *name = files[members[m]].name;

			if (set && IS_TRACK(name))
				continue;
			ok = accepts(s, name);
		}
		cand[s] = ok;
		if (ok) {
			ncand++;
			only = s;
		}
	}
	if (ncand > 1) {
		for (int m = 0; m < count && track < 0; m++) {
			const char *e = ext_of(files[members[m]].name);

			if (!set || !strcasecmp(e, "bin") || !strcasecmp(e, "img") || !strcasecmp(e, "iso"))
				track = members[m];
		}
		int s = track >= 0 ? sniff(&files[track]) : -1;
		if (s >= 0 && cand[s]) {
			ncand = 1;
			only = s;
		}
	}
	if (ncand == 0 && !set) {
		files[members[0]].skipped = true;
		skipped_count++;
		return;
	}
	if (ncand == 1) {
		for (int m = 0; m < count; m++)
			files[members[m]].sys = only;
		return;
	}
	if (set) {
		snprintf(key, sizeof(key), "disc:%s", files[members[0]].key);
	} else {
		char e[64];

		snprintf(e, sizeof(e), "%s", ext_of(files[members[0]].name));
		lower(e);
		snprintf(key, sizeof(key), "ext:%s", e);
	}
	int a = new_ask(key, members[0]);
	for (int s = 0; s < system_count; s++)
		asks[a].cand[s] = asks[a].cand[s] || cand[s] || ncand == 0;
	for (int m = 0; m < count; m++)
		files[members[m]].ask = a;
	asks[a].count += count;
}

static void plan_files(void)
{
	int *members = malloc((size_t)(file_count ? file_count : 1) * sizeof(int));

	if (!members) {
		printf("@error memory\n");
		exit(1);
	}
	for (int i = 0; i < file_count; i++)
		place_by_folder(&files[i]);
	for (int i = 0; i < file_count; i++)
		if (!files[i].placed && IS_INDEX(files[i].name))
			claim_index_refs(i);
	/* Tracks no index names follow the first index of their folder. */
	for (int i = 0; i < file_count; i++) {
		if (files[i].placed || files[i].owner >= 0 || !IS_TRACK(files[i].name))
			continue;
		for (int k = 0; k < file_count; k++)
			if (!files[k].placed && IS_INDEX(files[k].name) && !strcmp(files[k].rel, files[i].rel)) {
				files[i].owner = k;
				break;
			}
	}
	for (int i = 0; i < file_count; i++) {
		int n = 0;

		if (files[i].placed || !IS_INDEX(files[i].name) || top_of(i) != i)
			continue;
		members[n++] = i;
		for (int k = 0; k < file_count; k++)
			if (k != i && !files[k].placed && files[k].owner >= 0 && top_of(k) == i)
				members[n++] = k;
		decide(members, n, true);
	}
	for (int i = 0; i < file_count; i++) {
		if (files[i].placed || IS_INDEX(files[i].name) ||
		    (files[i].owner >= 0 && IS_INDEX(files[top_of(i)].name)))
			continue;
		members[0] = i;
		decide(members, 1, false);
	}
	free(members);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int plan(void)
{
	long long bytes = 0;
	int count = 0, per_sys[MAX_SYSTEMS] = { 0 };

	for (int i = 0; i < file_count; i++) {
		if (files[i].skipped)
			continue;
		count++;
		bytes += files[i].size;
		if (files[i].sys >= 0)
			per_sys[files[i].sys]++;
	}
	printf("files=%d bytes=%lld skipped=%d asks=%d\n", count, bytes, skipped_count, ask_count);
	for (int s = 0; s < system_count; s++)
		if (per_sys[s])
			printf("place\t%s\t%d\n", systems[s].id, per_sys[s]);
	for (int a = 0; a < ask_count; a++) {
		bool first = true;

		printf("ask\t%d\t%s\t%d\t", a, files[asks[a].first].name, asks[a].count);
		for (int s = 0; s < system_count; s++)
			if (asks[a].cand[s]) {
				printf("%s%s", first ? "" : ",", systems[s].id);
				first = false;
			}
		printf("\n");
		for (int s = 0; s < system_count; s++)
			if (asks[a].cand[s])
				printf("cand\t%d\t%s\t%s\n", a, systems[s].id, systems[s].name);
	}
	return 0;
}

/* One request to libraryd; the reply is always read, so the request is
 * handled before the connection goes away. */
static int library_request(const char *line, char *reply, size_t size)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	struct timeval tv = { 5, 0 };
	char scratch[256];
	size_t got = 0;
	ssize_t n;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (!reply) {
		reply = scratch;
		size = sizeof(scratch);
	}

	if (fd < 0)
		return -1;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LIBRARY_SOCKET);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    write(fd, line, strlen(line)) != (ssize_t)strlen(line)) {
		close(fd);
		return -1;
	}
	while (got + 1 < size && (n = read(fd, reply + got, size - got - 1)) > 0) {
		got += (size_t)n;
		reply[got] = '\0';
		/* STATUS ends with "end=1", a command with its one-line answer. */
		if (strstr(reply, "end=1\n") || (strncmp(line, "STATUS", 6) && strchr(reply, '\n')))
			break;
	}
	reply[got] = '\0';
	close(fd);
	return 0;
}

/* libraryd's games_root (the SD2 games card or /userdata/roms). */
static bool games_root(char *out, size_t size)
{
	const char *env = getenv("NUUBOS_GAMES_ROOT");
	static char reply[65536];
	char *p;

	if (env && *env) {
		snprintf(out, size, "%s", env);
		return true;
	}
	if (library_request("STATUS\n", reply, sizeof(reply)) != 0)
		return false;
	for (p = reply; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
		if (!strncmp(p, "games_root=", 11)) {
			snprintf(out, size, "%.*s", (int)strcspn(p + 11, "\n"), p + 11);
			return out[0] == '/';
		}
	return false;
}

static int mkdirs(char *path)
{
	for (char *p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) != 0 && errno != EEXIST) {
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	return mkdir(path, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

/* Where a file goes; false when the path does not fit. */
static bool destination(char *out, size_t size, const char *root, const struct file *f)
{
	return snprintf(out, size, "%s/%s%s%s/%s", root, systems[f->sys].folder, f->rel[0] ? "/" : "",
			f->rel, f->name) < (int)size;
}

static int copy_file(const struct file *f, const char *dst, long long *done, long long total, int *last)
{
	char part[PATH_MAX + 8], dir[PATH_MAX];
	static char buf[1 << 20];
	int in, out;
	ssize_t n = 0;

	if (snprintf(dir, sizeof(dir), "%s", dst) >= (int)sizeof(dir) ||
	    snprintf(part, sizeof(part), "%s.part", dst) >= (int)sizeof(part))
		return -1;
	*strrchr(dir, '/') = '\0';
	if (mkdirs(dir) != 0)
		return -1;
	in = open(f->abs, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	out = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		close(in);
		return -1;
	}
	while (!cancelled && (n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) {
			n = -1;
			break;
		}
		*done += n;
		int pct = total > 0 ? (int)(*done * 100 / total) : 100;
		if (pct != *last) {
			*last = pct;
			printf("@progress %d %s\n", pct, f->name);
			fflush(stdout);
		}
	}
	close(in);
	if (n < 0 || cancelled || close(out) != 0 || rename(part, dst) != 0) {
		unlink(part);
		return -1;
	}
	return 0;
}

static int run(const char *answers)
{
	char root[PATH_MAX], dst[PATH_MAX];
	long long total = 0, done = 0;
	int copied = 0, present = 0, skipped = skipped_count, last = -1;
	struct statvfs vfs;

	/* "<n>=<system>,..." */
	for (const char *p = answers; p && *p;) {
		char id[32] = "";
		int a = -1, used = 0;

		if (sscanf(p, "%d=%31[^,]%n", &a, id, &used) >= 2 && a >= 0 && a < ask_count) {
			int s = system_by_id(id);

			if (s >= 0 && asks[a].cand[s])
				for (int i = 0; i < file_count; i++)
					if (files[i].ask == a)
						files[i].sys = s;
		}
		p = strchr(p, ',');
		if (p)
			p++;
	}
	if (!games_root(root, sizeof(root))) {
		printf("@error library\n");
		return 1;
	}
	for (int i = 0; i < file_count; i++) {
		struct stat st;

		if (files[i].skipped)
			continue;
		if (files[i].sys < 0) {
			files[i].skipped = true;
			skipped++;
			continue;
		}
		if (!destination(dst, sizeof(dst), root, &files[i])) {
			files[i].skipped = true;
			skipped++;
			continue;
		}
		if (stat(dst, &st) == 0) {
			files[i].skipped = true;
			present++;
			continue;
		}
		total += files[i].size;
	}
	if (mkdirs(root) != 0 || statvfs(root, &vfs) != 0) {
		printf("@error storage\n");
		return 1;
	}
	if ((long long)vfs.f_bavail * (long long)vfs.f_frsize < total + SPACE_MARGIN) {
		printf("@error space\n");
		return 1;
	}
	printf("@progress 0\n");
	fflush(stdout);
	for (int i = 0; i < file_count && !cancelled; i++) {
		if (files[i].skipped)
			continue;
		destination(dst, sizeof(dst), root, &files[i]);
		if (copy_file(&files[i], dst, &done, total, &last) != 0) {
			if (cancelled)
				break;
			printf("@error copy\n");
			sync();
			library_request("SCAN\n", NULL, 0);
			return 1;
		}
		copied++;
	}
	sync();
	if (copied)
		library_request("SCAN\n", NULL, 0);
	if (cancelled)
		return 1;
	printf("@progress 100\n@result %d %d %d\n", copied, present, skipped);
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_term };

	if (argc < 3 || (strcmp(argv[1], "plan") && strcmp(argv[1], "run"))) {
		fprintf(stderr, "Usage: nuubos-library-import plan SRC | run SRC [N=SYSTEM,...]\n");
		return 2;
	}
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (!load_registry()) {
		printf("@error registry\n");
		return 1;
	}
	if (!collect(argv[2])) {
		printf("@error source\n");
		return 1;
	}
	plan_files();
	if (!strcmp(argv[1], "plan"))
		return plan();
	return run(argc > 3 ? argv[3] : "");
}
