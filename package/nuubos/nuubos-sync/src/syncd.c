/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-syncd — the Syncthing capability of the active user (EPIC-034).
 *
 * Owner of synchronization: it runs Syncthing for the active user (only
 * when that user turned it on), applies the nuubOS policy to it, follows
 * its events and publishes one state snapshot to its clients.
 *
 * Per user, in /userdata/users/<id>/appdata/syncthing: Syncthing's own home
 * (identity, devices, folders, database) and nuubos.conf:
 *   ENABLED=0|1  APIKEY=<uuid>  INTERNET=0|1  RELAYS=0|1  FOLDERS=saves,...
 *   ID_saves=<folder id>  ID_states=  ID_screenshots=  ID_roms=  ID_roms-sd2=
 *   (optional: an existing share of the user's other devices used instead
 *   of nuubOS's own folder)
 * Switching users never exposes another user's configuration.
 *
 * Syncthing is reached only through a root-only unix socket
 * (/run/nuubos/syncthing/gui.sock, REST with the user's API key); there is
 * no network GUI. It runs without its monitor process (STMONITORED: one
 * process, ~44 MB less), nice 10. Policy applied at every start:
 *   - device name "<profile> · <model>" (all consoles were "nuubos");
 *   - local network only unless INTERNET=1: global discovery, NAT
 *     traversal and STUN (public Syncthing servers) find the devices
 *     over the Internet, the data still goes directly between them;
 *     only with RELAYS=1 too may encrypted data pass through the public
 *     relays when no direct connection works; no crash or usage
 *     reports, ever;
 *   - folders exactly as FOLDERS says, shared with every accepted device:
 *     the user's own "nuubos-<profile key>-<kind>" (key from the profile
 *     name, folder_ids(): the same person on two consoles meets in the same
 *     folder, two people never do, even through a PC or NAS they share),
 *     the console's library store "nuubos-covers" / "nuubos-metadata";
 *     the console's ROM folders "nuubos-roms" (/userdata/roms) and
 *     "nuubos-roms-sd2" (the SD2 card's roms folder, paused while the card
 *     is not ready: never moved to SD1; user request 2026-10-28, off by
 *     default); saves, states, screenshots and both ROM folders may use an
 *     existing folder id instead (ID_<kind>, user report 2026-10-21: other
 *     devices already share them); any other folder on one of these paths
 *     is removed; ignorePerms (exFAT); saves keep replaced versions in
 *     .stversions for 30 days (trash can). Conflicts stay as Syncthing
 *     ".sync-conflict" copies. ROMs that arrive start a Library scan.
 *
 * Socket /run/nuubos/syncd.sock (0660), one command per line, tab separated:
 *   STATUS | SUBSCRIBE           snapshot (below); SUBSCRIBE then gets a
 *                                new snapshot after every change
 *   ENABLE | DISABLE
 *   FOLDER <kind> 0|1            kind = saves | states | screenshots | library
 *                                (covers + metadata) | history (play activity)
 *                                | roms | roms-sd2
 *   INTERNET 0|1
 *   RELAYS 0|1                   relays (used only with INTERNET 1)
 *   ADD <device> [<name>]        accept a request or add a nearby device
 *   IGNORE <device>              refuse a request (it does not come back)
 *   REMOVE <device>
 *   FOLDER_ID <kind> <id>|-      saves | states | screenshots | roms |
 *                                roms-sd2: use that
 *                                existing folder id (- = nuubOS's own)
 *   USE <folder> <kind>          take a folder another device offers as
 *                                that kind (FOLDER_ID + FOLDER on)
 *   DECLINE <folder>             refuse an offered folder (does not come back)
 * Replies: OK | ERR <reason> (no-user, not-running, device, syncthing,
 * storage, value, unknown).
 *
 * Snapshot (key=value, ends with end=1):
 *   enabled= running= ready= failed= internet= relays= device_id= device_name=
 *   folder=<kind>\t<on>\t<state>      kind: saves states screenshots library
 *                                     history roms roms-sd2; state: Syncthing's (idle, scanning,
 *                                     syncing, sync-preparing, error...)
 *   device=<id>\t<name>\t<connected>\t<completion 0..100, -1 unknown>\t<address>
 *   pending=<id>\t<name>\t<address>   asked to connect
 *   nearby=<id>\t<address>            Syncthing seen on the local network
 *   folderid=<kind>\t<id>\t<custom 0|1>  saves, states, screenshots, roms,
 *                                     roms-sd2
 *   offer=<folder>\t<label>\t<device>\t<device name>  a configured device
 *                                     shares a folder this console does not use
 *
 * Live Notification "sync.request" (name=) when a device asks to connect;
 * dismissed once it is accepted, ignored or gone.
 *
 * Events: inotify on /run/nuubos/user (user switch), on the Syncthing
 * socket directory (ready) and on /run/nuubos/storage (SD2 in or out), SIGHUP, SIGCHLD, client sockets, and a thread
 * that long-polls Syncthing's event stream while it runs. A Syncthing that
 * dies on its own is not restarted (no restart storm): failed=1 until the
 * next user switch or ENABLE. Idle: poll() without timeout.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <nuubos/notify.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#define UI_READY RUN_ROOT "/ui-ready"
#ifndef USERDATA_USERS
#define USERDATA_USERS "/userdata/users"
#endif
#ifndef USERDATA_ROOT
#define USERDATA_ROOT "/userdata"
#endif
#ifndef LIBRARY_SOCKET
#define LIBRARY_SOCKET RUN_ROOT "/libraryd.sock"
#endif
#ifndef STATE_USERS
#define STATE_USERS "/state/users"
#endif
#ifndef SYNCTHING_BIN
#define SYNCTHING_BIN "/usr/bin/syncthing"
#endif
#ifndef MODEL_FILE
#define MODEL_FILE "/proc/device-tree/model"
#endif
#define PIDFILE RUN_ROOT "/syncd.pid"
#define SOCKET_PATH RUN_ROOT "/syncd.sock"
#define GUI_DIR RUN_ROOT "/syncthing"
#define GUI_NAME "gui.sock"
#define GUI_SOCKET GUI_DIR "/" GUI_NAME
#define USER_DIR RUN_ROOT "/user"
#ifndef STORAGE_DIR
#define STORAGE_DIR RUN_ROOT "/storage"
#endif
#define SD2_STATUS STORAGE_DIR "/sd2.status"
#define ACTIVE_FILE USER_DIR "/active"

#define KINDS 7
#define FOLDERS 8
#define MAX_CLIENTS 12
#define MAX_DEVICES 32
#define ID_MAX 72

/* What the user turns on (FOLDERS=) and the Syncthing folders behind it.
 * Saves, states, screenshots and the play activity are the user's;
 * covers and metadata are the console's library store, shared by its
 * profiles (one file per game: safe to synchronize). Play activity is
 * library/consoles, where libraryd keeps one file per console and adds
 * the others up (never one file written by two consoles). ROMs are the
 * console's too: the internal card's folder and the SD2 card's. */
static const char *const KIND[KINDS] = { "saves", "states", "screenshots", "library", "history", "roms", "roms-sd2" };
/* id: the console's own folders' id, or the kind part of a per-user id
 * ("nuubos-<key>-<id>"); per-user folders were "nuubos-<id>" before
 * 2026-10-16 and are removed from Syncthing under that name. */
static const struct {
	int kind;
	const char *id, *label, *path;
	bool per_user, versioned;
} FOLDER[FOLDERS] = {
	{ 0, "saves", "nuubOS Saves", "saves", true, true },
	{ 1, "states", "nuubOS Save States", "states", true, false },
	{ 2, "screenshots", "nuubOS Screenshots", "screenshots", true, false },
	{ 3, "nuubos-covers", "nuubOS Covers", "library/covers", false, false },
	{ 3, "nuubos-metadata", "nuubOS Metadata", "library/metadata", false, false },
	{ 4, "history", "nuubOS Play Activity", "library/consoles", true, false },
	{ 5, "nuubos-roms", "nuubOS ROMs", "roms", false, false },
	{ 6, "nuubos-roms-sd2", "nuubOS ROMs (SD2)", NULL, false, false },
};

#define KIND_LIBRARY 3
#define KIND_ROMS 5
#define KIND_ROMS_SD2 6

/* Kinds that may use an existing folder id (ID_<kind>): plain files any
 * RetroArch writes, and ROMs. Covers/metadata and play activity are
 * nuubOS's own. */
static bool custom_kind(int kind)
{
	return (kind >= 0 && kind <= 2) || kind == KIND_ROMS || kind == KIND_ROMS_SD2;
}

#define FOLDER_ID_MAX 200

struct conf {
	bool enabled, internet, relays, folder[KINDS];
	char key[64];
	char custom[KINDS][FOLDER_ID_MAX];
};

struct client {
	int fd;
	bool sub;
	size_t used;
	char buf[512];
};

static volatile sig_atomic_t stop_requested, reload_requested;
static int wake_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];

/* Main thread only. */
static pid_t child = -1;
static char child_user[64], my_id[ID_MAX];
static bool failed, ready;
static char *snapshot;
static char notified[MAX_DEVICES][ID_MAX];

/* Shared with the event thread. */
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;
static unsigned ev_gen;
static bool ev_active;
/* Syncthing ids and labels of FOLDER for the running user (folder_ids). */
static char folder_id[FOLDERS][FOLDER_ID_MAX], folder_label[FOLDERS][FOLDER_ID_MAX];
static char ev_key[64];
static char folder_state[FOLDERS][24];
/* Covers or metadata arrived: libraryd rescans (its metadata index). */
static bool library_changed;
static struct {
	char id[ID_MAX];
	int pct[FOLDERS];
} completion[MAX_DEVICES];

static void wake(void)
{
	int saved = errno;
	char b = 1;

	(void)!write(wake_pipe[1], &b, 1);
	errno = saved;
}

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		reload_requested = 1;
	else if (sig != SIGCHLD)
		stop_requested = 1;
	wake();
}

/* ---------------------------------------------------------------- */
/* Files                                                            */
/* ---------------------------------------------------------------- */

static bool read_line(const char *path, char *out, size_t size)
{
	FILE *fp = fopen(path, "r");

	out[0] = '\0';
	if (!fp)
		return false;
	if (fgets(out, (int)size, fp))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(fp);
	return out[0] != '\0';
}

/* key=value of a small config file. */
static bool file_value(const char *path, const char *key, char *out, size_t size)
{
	char line[512];
	size_t klen = strlen(key);
	FILE *fp = fopen(path, "r");
	bool found = false;

	out[0] = '\0';
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			snprintf(out, size, "%s", line + klen + 1);
			found = true;
		}
	}
	fclose(fp);
	return found;
}

static bool valid_user(const char *u)
{
	if (strlen(u) != 36)
		return false;
	for (; *u; u++)
		if (!((*u >= '0' && *u <= '9') || (*u >= 'a' && *u <= 'f') || *u == '-'))
			return false;
	return true;
}

static bool active_user(char *out, size_t size)
{
	return read_line(ACTIVE_FILE, out, size) && valid_user(out);
}

static void conf_path(const char *user, char *out, size_t size)
{
	snprintf(out, size, USERDATA_USERS "/%s/appdata/syncthing/nuubos.conf", user);
}

static void conf_load(const char *user, struct conf *c)
{
	char path[PATH_MAX], v[256];

	memset(c, 0, sizeof(*c));
	conf_path(user, path, sizeof(path));
	c->enabled = file_value(path, "ENABLED", v, sizeof(v)) && !strcmp(v, "1");
	c->internet = file_value(path, "INTERNET", v, sizeof(v)) && !strcmp(v, "1");
	c->relays = file_value(path, "RELAYS", v, sizeof(v)) && !strcmp(v, "1");
	if (file_value(path, "APIKEY", v, sizeof(v)) && !strpbrk(v, " \t"))
		snprintf(c->key, sizeof(c->key), "%.63s", v);
	if (file_value(path, "FOLDERS", v, sizeof(v))) {
		for (int i = 0; i < KINDS; i++) {
			char *p = v;
			size_t n = strlen(KIND[i]);

			while ((p = strstr(p, KIND[i]))) {
				if ((p == v || p[-1] == ',') && (p[n] == ',' || !p[n]))
					c->folder[i] = true;
				p += n;
			}
		}
	} else {
		/* Before FOLDERS existed the folders lived only in Syncthing;
		 * a new configuration starts with the saves. */
		c->folder[0] = true;
	}
	for (int i = 0; i < KINDS; i++) {
		char k[32];

		snprintf(k, sizeof(k), "ID_%s", KIND[i]);
		if (custom_kind(i) && file_value(path, k, v, sizeof(v)))
			snprintf(c->custom[i], sizeof(c->custom[i]), "%s", v);
	}
}

static int conf_save(const char *user, const struct conf *c)
{
	char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX + 8], folders[96] = "";
	FILE *fp;

	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/appdata", user);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), USERDATA_USERS "/%s/appdata/syncthing", user);
	mkdir(dir, 0700);
	for (int i = 0; i < KINDS; i++)
		if (c->folder[i])
			snprintf(folders + strlen(folders), sizeof(folders) - strlen(folders), "%s%s",
				 folders[0] ? "," : "", KIND[i]);
	conf_path(user, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "ENABLED=%d\nAPIKEY=%s\nINTERNET=%d\nRELAYS=%d\nFOLDERS=%s\n", c->enabled, c->key, c->internet,
		c->relays, folders);
	for (int i = 0; i < KINDS; i++)
		if (c->custom[i][0])
			fprintf(fp, "ID_%s=%s\n", KIND[i], c->custom[i]);
	if (fflush(fp) || fsync(fileno(fp))) {
		fclose(fp);
		unlink(tmp);
		return -1;
	}
	fclose(fp);
	return rename(tmp, path) ? -1 : 0;
}

/* "<profile name> · <model>", the name other devices show. */
static void device_name(const char *user, char *out, size_t size)
{
	char path[PATH_MAX], name[96] = "", model[96] = "";
	FILE *fp;

	snprintf(path, sizeof(path), STATE_USERS "/%s/profile.conf", user);
	file_value(path, "USER_NAME", name, sizeof(name));
	fp = fopen(MODEL_FILE, "r");
	if (fp) {
		size_t n = fread(model, 1, sizeof(model) - 1, fp);

		model[n] = '\0';
		fclose(fp);
	}
	model[strcspn(model, "\n")] = '\0';
	for (size_t n = strlen(model); n && isspace((unsigned char)model[n - 1]);)
		model[--n] = '\0';
	{
		/* "Anbernic RG CubeXX" -> "RG CubeXX" */
		const char *m = strncmp(model, "Anbernic ", 9) ? model : model + 9;

		if (name[0] && m[0])
			snprintf(out, size, "%s · %s", name, m);
		else
			snprintf(out, size, "%s", name[0] ? name : m[0] ? m : "nuubOS");
	}
}

/* ---------------------------------------------------------------- */
/* Syncthing REST                                                   */
/* ---------------------------------------------------------------- */

struct buffer {
	char *data;
	size_t len;
};

static size_t collect(char *ptr, size_t size, size_t n, void *userdata)
{
	struct buffer *b = userdata;
	size_t len = size * n;
	char *p = realloc(b->data, b->len + len + 1);

	if (!p)
		return 0;
	b->data = p;
	memcpy(b->data + b->len, ptr, len);
	b->len += len;
	b->data[b->len] = '\0';
	return len;
}

/* One REST call on the unix socket: the parsed JSON (or NULL), the HTTP
 * status in *status (-1 when Syncthing did not answer). */
static cJSON *rest_with(const char *key, const char *method, const char *path, const char *body,
			long *status, long timeout)
{
	char url[512], header[128];
	struct buffer b = { 0 };
	struct curl_slist *h = NULL;
	CURL *curl;
	cJSON *json = NULL;

	*status = -1;
	if (!key[0] || !(curl = curl_easy_init()))
		return NULL;
	snprintf(url, sizeof(url), "http://localhost%s", path);
	snprintf(header, sizeof(header), "X-API-Key: %s", key);
	h = curl_slist_append(h, header);
	h = curl_slist_append(h, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, GUI_SOCKET);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	if (body)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	if (curl_easy_perform(curl) == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
	curl_slist_free_all(h);
	curl_easy_cleanup(curl);
	if (b.data)
		json = cJSON_Parse(b.data);
	free(b.data);
	return json;
}

static struct conf cur; /* the running user's configuration */

static cJSON *rest(const char *method, const char *path, const char *body, long *status)
{
	return rest_with(cur.key, method, path, body, status, 10L);
}

static bool rest_ok(const char *method, const char *path, cJSON *body)
{
	char *text = body ? cJSON_PrintUnformatted(body) : NULL;
	long status;

	cJSON_Delete(rest(method, path, text, &status));
	free(text);
	return status == 200;
}

static const char *jstr(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

	return cJSON_IsString(v) ? v->valuestring : "";
}

static bool valid_device(const char *id)
{
	size_t len = strlen(id);

	if (len < 50 || len >= ID_MAX)
		return false;
	for (; *id; id++)
		if (!isupper((unsigned char)*id) && !isdigit((unsigned char)*id) && *id != '-')
			return false;
	return true;
}

/* Field text without the line/field separators of the snapshot. */
static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

/* "192.168.1.20:22000" -> "192.168.1.20", "[fe80::1]:22000" -> "fe80::1". */
static void strip_port(const char *a, char *out, size_t size)
{
	size_t n;

	if (a[0] == '[') {
		a++;
		n = strcspn(a, "]");
	} else {
		n = strcspn(a, ":");
	}
	snprintf(out, size, "%.*s", (int)n, a);
	clean(out);
}

/* The host of a discovery entry ("tcp://192.168.1.20:22000"), IPv4 first. */
static void host_of(const cJSON *addresses, char *out, size_t size)
{
	const cJSON *a;

	out[0] = '\0';
	cJSON_ArrayForEach(a, addresses) {
		const char *s = cJSON_IsString(a) ? a->valuestring : "", *p = strstr(s, "://");
		char host[96];

		if (!p)
			continue;
		strip_port(p + 3, host, sizeof(host));
		if (!host[0] || (out[0] && strchr(host, ':')))
			continue;
		snprintf(out, size, "%s", host);
		if (!strchr(host, ':'))
			return;
	}
}

/* ---------------------------------------------------------------- */
/* Policy                                                           */
/* ---------------------------------------------------------------- */

static void mkdir_parents(const char *dir)
{
	char p[PATH_MAX];

	snprintf(p, sizeof(p), "%s", dir);
	for (char *q = p + 1; *q; q++)
		if (*q == '/') {
			*q = '\0';
			mkdir(p, 0755);
			*q = '/';
		}
	mkdir(p, 0755);
}

static bool apply_options(void)
{
	cJSON *o = cJSON_CreateObject();
	bool ok;

	cJSON_AddBoolToObject(o, "globalAnnounceEnabled", cur.internet);
	/* Relays carry the (encrypted) data: only when the user allows it. */
	cJSON_AddBoolToObject(o, "relaysEnabled", cur.internet && cur.relays);
	cJSON_AddBoolToObject(o, "natEnabled", cur.internet);
	cJSON_AddNumberToObject(o, "stunKeepaliveStartS", cur.internet ? 180 : 0);
	cJSON_AddBoolToObject(o, "localAnnounceEnabled", true);
	cJSON_AddBoolToObject(o, "crashReportingEnabled", false);
	cJSON_AddNumberToObject(o, "urAccepted", -1);
	cJSON_AddBoolToObject(o, "startBrowser", false);
	ok = rest_ok("PATCH", "/rest/config/options", o);
	cJSON_Delete(o);
	return ok;
}

/* Per-user folder ids carry a key of the profile name, so a PC or NAS that
 * several profiles sync with keeps each person's saves apart (with fixed
 * ids every profile of every console met in one folder; user report
 * 2026-10-16) while the same name on two consoles still meets. Key: ASCII
 * letters and digits lowercased, other ASCII runs as one "-", each
 * non-ASCII byte as two hex digits ("José" -> "josc3a9"), at most 64
 * bytes of name: URL-safe and the same on every console. A renamed profile
 * gets new folders (the other devices accept them again). An existing
 * folder id chosen for saves, states or screenshots replaces nuubOS's. */
static void compute_ids(const char *user, const struct conf *c, char ids[FOLDERS][FOLDER_ID_MAX],
			char labels[FOLDERS][FOLDER_ID_MAX])
{
	char path[PATH_MAX], name[96] = "", key[160] = "";
	size_t n = 0;

	snprintf(path, sizeof(path), STATE_USERS "/%s/profile.conf", user);
	file_value(path, "USER_NAME", name, sizeof(name));
	for (const unsigned char *c = (const unsigned char *)name; *c && n + 3 < sizeof(key); c++) {
		if (isalnum(*c) && *c < 0x80)
			key[n++] = (char)tolower(*c);
		else if (*c >= 0x80)
			n += (size_t)snprintf(key + n, sizeof(key) - n, "%02x", *c);
		else if (n && key[n - 1] != '-')
			key[n++] = '-';
	}
	while (n && key[n - 1] == '-')
		n--;
	key[n] = '\0';
	if (!n)
		snprintf(key, sizeof(key), "%.36s", user);
	for (int i = 0; i < FOLDERS; i++) {
		if (c->custom[FOLDER[i].kind][0])
			snprintf(ids[i], FOLDER_ID_MAX, "%s", c->custom[FOLDER[i].kind]);
		else if (FOLDER[i].per_user)
			snprintf(ids[i], FOLDER_ID_MAX, "nuubos-%s-%s", key, FOLDER[i].id);
		else
			snprintf(ids[i], FOLDER_ID_MAX, "%s", FOLDER[i].id);
		if (FOLDER[i].per_user)
			snprintf(labels[i], FOLDER_ID_MAX, "%s · %s", FOLDER[i].label, name[0] ? name : key);
		else
			snprintf(labels[i], FOLDER_ID_MAX, "%s", FOLDER[i].label);
	}
}

/* Syncthing ids and labels of the running user. Caller holds ev_lock. */
static void folder_ids(const char *user)
{
	compute_ids(user, &cur, folder_id, folder_label);
}

/* "<prefix><id escaped for a URL>": folder ids chosen elsewhere may hold
 * any character. */
static void folder_url(char *out, size_t size, const char *prefix, const char *id)
{
	char *e = curl_easy_escape(NULL, id, 0);

	snprintf(out, size, "%s%s", prefix, e ? e : "");
	curl_free(e);
}

/* The SD2 card's roms folder (storagectl's mount point; its usual place
 * while no card is in) and whether the card can be written now. */
static bool sd2_roms(char *dir, size_t size)
{
	char state[32] = "", mount[PATH_MAX] = "";

	file_value(SD2_STATUS, "SD2_STATE", state, sizeof(state));
	file_value(SD2_STATUS, "SD2_PATH", mount, sizeof(mount));
	snprintf(dir, size, "%s/roms", mount[0] ? mount : "/media/sd2");
	return !strcmp(state, "ready");
}

/* Where FOLDER[i] lives for the running user. */
static void folder_dir(int i, char *dir, size_t size)
{
	if (FOLDER[i].kind == KIND_ROMS_SD2)
		sd2_roms(dir, size);
	else if (FOLDER[i].per_user)
		snprintf(dir, size, USERDATA_USERS "/%s/%s", child_user, FOLDER[i].path);
	else
		snprintf(dir, size, USERDATA_ROOT "/%s", FOLDER[i].path);
}

/* An id another device may have chosen: printable, no separators of the
 * socket protocol or the snapshot. */
static bool valid_folder_id(const char *id)
{
	size_t len = strlen(id);

	if (!len || len >= FOLDER_ID_MAX)
		return false;
	for (; *id; id++)
		if ((unsigned char)*id < 0x20 || *id == 0x7f)
			return false;
	return true;
}

/* Every nuubOS folder exactly as the user's FOLDERS says, shared with all
 * accepted devices. */
static bool apply_folders(void)
{
	long status;
	cJSON *devices = rest("GET", "/rest/config/devices", NULL, &status), *d;
	bool ok = status == 200;

	cJSON *folders = rest("GET", "/rest/config/folders", NULL, &status), *x;
	cJSON *listed = status == 200 ? folders : NULL;

	/* nuubOS folders that are not the user's current ones stop syncing
	 * (files stay): the shared ids of before 2026-10-16, those of an
	 * earlier profile name and an existing folder used before, which
	 * would overlap the current ones' paths. */
	cJSON_ArrayForEach(x, listed) {
		const char *id = jstr(x, "id"), *where = jstr(x, "path");
		bool current = false, ours = !strncmp(id, "nuubos-", 7);
		char path[FOLDER_ID_MAX * 3 + 32], dir[PATH_MAX];

		for (int i = 0; i < FOLDERS; i++) {
			current = current || !strcmp(id, folder_id[i]);
			folder_dir(i, dir, sizeof(dir));
			ours = ours || !strcmp(where, dir);
		}
		if (current || !ours || !id[0])
			continue;
		folder_url(path, sizeof(path), "/rest/config/folders/", id);
		cJSON_Delete(rest("DELETE", path, NULL, &status));
		fprintf(stderr, "syncd: folder %s removed (replaced)\n", id);
	}
	cJSON_Delete(folders);
	for (int i = 0; ok && i < FOLDERS; i++) {
		char path[FOLDER_ID_MAX * 3 + 32], dir[PATH_MAX];

		folder_url(path, sizeof(path), "/rest/config/folders/", folder_id[i]);
		if (!cur.folder[FOLDER[i].kind]) {
			/* The folder stops syncing; its files stay on the console. */
			cJSON_Delete(rest("DELETE", path, NULL, &status));
			ok = status == 200 || status == 404;
			continue;
		}
		cJSON *f = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(f, "devices");
		bool usable = true;

		folder_dir(i, dir, sizeof(dir));
		if (FOLDER[i].kind == KIND_ROMS_SD2)
			usable = sd2_roms(dir, sizeof(dir));
		/* No SD2 card: the folder waits (paused) at the card's path; it
		 * is never created on SD1's mount point. */
		if (usable)
			mkdir_parents(dir);
		cJSON_AddBoolToObject(f, "paused", !usable);
		cJSON_AddStringToObject(f, "id", folder_id[i]);
		cJSON_AddStringToObject(f, "label", folder_label[i]);
		cJSON_AddStringToObject(f, "path", dir);
		cJSON_AddStringToObject(f, "type", "sendreceive");
		/* exFAT has no permissions. */
		cJSON_AddBoolToObject(f, "ignorePerms", true);
		cJSON_ArrayForEach(d, devices) {
			cJSON *e = cJSON_CreateObject();

			cJSON_AddStringToObject(e, "deviceID", jstr(d, "deviceID"));
			cJSON_AddItemToArray(list, e);
		}
		if (FOLDER[i].versioned) {
			/* A save another device replaced stays recoverable. */
			cJSON *v = cJSON_AddObjectToObject(f, "versioning");

			cJSON_AddStringToObject(v, "type", "trashcan");
			cJSON_AddStringToObject(cJSON_AddObjectToObject(v, "params"), "cleanoutDays", "30");
		}
		ok = rest_ok("PUT", path, f);
		cJSON_Delete(f);
	}
	cJSON_Delete(devices);
	return ok;
}

/* Syncthing answers: identity, policy, then follow its events. */
static void on_ready(void)
{
	long status;
	cJSON *sys = rest("GET", "/rest/system/status", NULL, &status);
	char name[160], path[ID_MAX + 32];

	if (status != 200) {
		cJSON_Delete(sys);
		return;
	}
	snprintf(my_id, sizeof(my_id), "%s", jstr(sys, "myID"));
	cJSON_Delete(sys);
	ready = true;
	/* Follow the events first: what the policy below changes is seen. */
	pthread_mutex_lock(&ev_lock);
	memset(folder_state, 0, sizeof(folder_state));
	memset(completion, 0, sizeof(completion));
	folder_ids(child_user);
	snprintf(ev_key, sizeof(ev_key), "%s", cur.key);
	ev_active = true;
	ev_gen++;
	pthread_cond_signal(&ev_cond);
	pthread_mutex_unlock(&ev_lock);
	device_name(child_user, name, sizeof(name));
	{
		cJSON *d = cJSON_CreateObject();

		cJSON_AddStringToObject(d, "name", name);
		snprintf(path, sizeof(path), "/rest/config/devices/%s", my_id);
		if (!rest_ok("PATCH", path, d))
			fprintf(stderr, "syncd: device name not set\n");
		cJSON_Delete(d);
	}
	if (!apply_options() || !apply_folders())
		fprintf(stderr, "syncd: policy not fully applied\n");
	/* Where each folder stands now; StateChanged events follow it. */
	for (int i = 0; i < FOLDERS; i++) {
		char q[FOLDER_ID_MAX * 3 + 32];
		cJSON *st;

		if (!cur.folder[FOLDER[i].kind])
			continue;
		folder_url(q, sizeof(q), "/rest/db/status?folder=", folder_id[i]);
		st = rest("GET", q, NULL, &status);
		pthread_mutex_lock(&ev_lock);
		if (status == 200 && !folder_state[i][0])
			snprintf(folder_state[i], sizeof(folder_state[i]), "%s", jstr(st, "state"));
		pthread_mutex_unlock(&ev_lock);
		cJSON_Delete(st);
	}
	fprintf(stderr, "syncd: syncthing ready (%s)\n", my_id);
}

/* ---------------------------------------------------------------- */
/* Syncthing events (own thread)                                    */
/* ---------------------------------------------------------------- */

static int folder_of(const char *id)
{
	for (int i = 0; i < FOLDERS; i++)
		if (folder_id[i][0] && !strcmp(id, folder_id[i]))
			return i;
	return -1;
}

static void record_event(const cJSON *e)
{
	const char *type = jstr(e, "type");
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(e, "data");
	int k = folder_of(jstr(data, "folder"));

	if (!strcmp(type, "StateChanged") && k >= 0) {
		/* Covers, metadata or ROMs pulled from another device. */
		if ((FOLDER[k].kind == KIND_LIBRARY || FOLDER[k].kind == KIND_ROMS || FOLDER[k].kind == KIND_ROMS_SD2) &&
		    !strcmp(jstr(data, "from"), "syncing") && !strcmp(jstr(data, "to"), "idle"))
			library_changed = true;
		snprintf(folder_state[k], sizeof(folder_state[k]), "%s", jstr(data, "to"));
	} else if (!strcmp(type, "FolderCompletion") && k >= 0) {
		const cJSON *c = cJSON_GetObjectItemCaseSensitive(data, "completion");
		const char *dev = jstr(data, "device");
		int slot = -1;

		for (int i = 0; i < MAX_DEVICES && slot < 0; i++)
			if (!strcmp(completion[i].id, dev))
				slot = i;
		for (int i = 0; i < MAX_DEVICES && slot < 0; i++)
			if (!completion[i].id[0]) {
				slot = i;
				snprintf(completion[i].id, sizeof(completion[i].id), "%s", dev);
				for (int j = 0; j < FOLDERS; j++)
					completion[i].pct[j] = 100;
			}
		if (slot >= 0 && cJSON_IsNumber(c))
			completion[slot].pct[k] = (int)c->valuedouble;
	}
}

static void *event_thread(void *arg)
{
	(void)arg;
	for (;;) {
		char key[64], path[384];
		unsigned gen;
		long since = 0;
		int failures = 0;

		pthread_mutex_lock(&ev_lock);
		while (!ev_active)
			pthread_cond_wait(&ev_cond, &ev_lock);
		gen = ev_gen;
		snprintf(key, sizeof(key), "%s", ev_key);
		pthread_mutex_unlock(&ev_lock);

		for (;;) {
			long status;
			cJSON *list, *e;
			bool current;

			/* Long poll: Syncthing answers when something happens
			 * (or after 5 min with nothing). */
			snprintf(path, sizeof(path),
				 "/rest/events?since=%ld&timeout=300&events=DeviceConnected,DeviceDisconnected,"
				 "DeviceDiscovered,DevicePaused,DeviceResumed,PendingDevicesChanged,ConfigSaved,"
				 "PendingFoldersChanged,StateChanged,FolderCompletion,FolderErrors", since);
			list = rest_with(key, "GET", path, NULL, &status, 330L);
			pthread_mutex_lock(&ev_lock);
			current = ev_active && ev_gen == gen;
			if (current && status == 200) {
				cJSON_ArrayForEach(e, list) {
					const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");

					if (cJSON_IsNumber(id) && id->valuedouble > since)
						since = (long)id->valuedouble;
					record_event(e);
				}
			}
			pthread_mutex_unlock(&ev_lock);
			if (status == 200 && cJSON_GetArraySize(list) > 0)
				wake();
			cJSON_Delete(list);
			if (!current)
				break;
			if (status != 200) {
				/* Syncthing stopping: the main loop clears ev_active.
				 * Otherwise a short pause before asking again. */
				if (++failures > 5)
					break;
				sleep(1);
			} else {
				failures = 0;
			}
		}
		pthread_mutex_lock(&ev_lock);
		if (ev_gen == gen)
			ev_active = false;
		pthread_mutex_unlock(&ev_lock);
	}
	return NULL;
}

/* ---------------------------------------------------------------- */
/* Snapshot                                                         */
/* ---------------------------------------------------------------- */

static bool is_configured(const cJSON *devices, const char *id)
{
	const cJSON *d;

	if (!strcmp(id, my_id))
		return true;
	cJSON_ArrayForEach(d, devices)
		if (!strcmp(jstr(d, "deviceID"), id))
			return true;
	return false;
}

/* A request Syncthing has not seen again for this long is stale: a device
 * that wants to connect retries about every minute and each refused
 * attempt renews its time, but Syncthing keeps the record in its database
 * forever (a device that tried once, days ago, came back at every start
 * with a notification). */
#define REQUEST_STALE_S (5 * 60)

/* "2026-10-12T15:59:27Z" or with fractions / an offset; 0 if unreadable. */
static time_t parse_time(const char *s)
{
	struct tm tm;
	const char *rest;
	time_t t;

	memset(&tm, 0, sizeof(tm));
	rest = strptime(s, "%Y-%m-%dT%H:%M:%S", &tm);
	if (!rest)
		return 0;
	t = timegm(&tm);
	if (*rest == '.')
		while (isdigit((unsigned char)*++rest))
			;
	if ((*rest == '+' || *rest == '-') && strlen(rest) >= 6) {
		int off = ((rest[1] - '0') * 10 + (rest[2] - '0')) * 3600 + ((rest[4] - '0') * 10 + (rest[5] - '0')) * 60;

		t -= *rest == '+' ? off : -off;
	}
	return t;
}

/* Drops the stale requests from Syncthing and from the list. */
static void prune_requests(cJSON *pending)
{
	cJSON *p = pending ? pending->child : NULL;
	time_t now = time(NULL);

	while (p) {
		cJSON *next = p->next;
		time_t when = parse_time(jstr(p, "time"));

		if (p->string && valid_device(p->string) && when > 0 && now - when > REQUEST_STALE_S) {
			char q[ID_MAX + 48];
			long status;

			snprintf(q, sizeof(q), "/rest/cluster/pending/devices?device=%s", p->string);
			cJSON_Delete(rest("DELETE", q, NULL, &status));
			fprintf(stderr, "syncd: stale request from %.7s dropped\n", p->string);
			cJSON_Delete(cJSON_DetachItemViaPointer(pending, p));
		}
		p = next;
	}
}

static void notify_requests(const cJSON *pending)
{
	const cJSON *p;

	/* New requests. */
	cJSON_ArrayForEach(p, pending) {
		bool known = false;
		int free_slot = -1;

		if (!p->string || !valid_device(p->string))
			continue;
		for (int i = 0; i < MAX_DEVICES; i++) {
			if (!strcmp(notified[i], p->string))
				known = true;
			else if (!notified[i][0] && free_slot < 0)
				free_slot = i;
		}
		if (!known && free_slot >= 0) {
			struct nuubos_notify n;
			char id[32], name[128];

			snprintf(notified[free_slot], ID_MAX, "%s", p->string);
			snprintf(id, sizeof(id), "sync.%.7s", p->string);
			snprintf(name, sizeof(name), "%s", jstr(p, "name"));
			nuubos_notify_begin(&n, "POST", id, "sync.request");
			nuubos_notify_str(&n, "name", name[0] ? name : id + 5);
			(void)nuubos_notify_send(&n);
		}
	}
	/* Requests that were accepted, ignored or withdrawn. */
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (!notified[i][0] || (pending && cJSON_GetObjectItemCaseSensitive(pending, notified[i])))
			continue;
		char id[32];

		snprintf(id, sizeof(id), "sync.%.7s", notified[i]);
		(void)nuubos_notify_dismiss(id);
		notified[i][0] = '\0';
	}
}

/* One state per kind (covers + metadata are one): the most telling of
 * its folders. Caller holds ev_lock. */
static const char *kind_state(int kind)
{
	static const char *const rank[] = { "error", "syncing", "sync-preparing", "sync-waiting", "scanning",
					    "scan-waiting", "idle" };

	for (size_t r = 0; r < sizeof(rank) / sizeof(rank[0]); r++)
		for (int i = 0; i < FOLDERS; i++)
			if (FOLDER[i].kind == kind && !strcmp(folder_state[i], rank[r]))
				return rank[r];
	for (int i = 0; i < FOLDERS; i++)
		if (FOLDER[i].kind == kind && folder_state[i][0])
			return folder_state[i];
	return "";
}

/* The folder ids of saves, states and screenshots: nuubOS's own or the
 * existing one chosen (custom 1). */
static void print_folder_ids(FILE *out, const char *user, const struct conf *c)
{
	char ids[FOLDERS][FOLDER_ID_MAX], labels[FOLDERS][FOLDER_ID_MAX];

	compute_ids(user, c, ids, labels);
	for (int i = 0; i < FOLDERS; i++) {
		int k = FOLDER[i].kind;
		char id[FOLDER_ID_MAX];

		if (!custom_kind(k))
			continue;
		snprintf(id, sizeof(id), "%s", ids[i]);
		clean(id);
		fprintf(out, "folderid=%s\t%s\t%d\n", KIND[k], id, c->custom[k][0] != '\0');
	}
}

/* Folders configured devices share and this console does not use yet
 * (Syncthing's pending folders), one line per offering device. Folders
 * meant to arrive encrypted (untrusted device) are not offered. */
static void print_offers(FILE *out, const cJSON *offers, const cJSON *devices)
{
	const cJSON *f, *by, *d;

	for (f = offers ? offers->child : NULL; f; f = f->next) {
		bool used = false;

		if (!f->string || !valid_folder_id(f->string))
			continue;
		for (int i = 0; i < FOLDERS; i++)
			used = used || !strcmp(f->string, folder_id[i]);
		if (used)
			continue;
		by = cJSON_GetObjectItemCaseSensitive(f, "offeredBy");
		cJSON_ArrayForEach(d, by) {
			char id[FOLDER_ID_MAX], label[FOLDER_ID_MAX], name[160] = "";
			const cJSON *x;

			if (!d->string || !valid_device(d->string) ||
			    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "receiveEncrypted")))
				continue;
			cJSON_ArrayForEach(x, devices)
				if (!strcmp(jstr(x, "deviceID"), d->string))
					snprintf(name, sizeof(name), "%s", jstr(x, "name"));
			snprintf(id, sizeof(id), "%s", f->string);
			snprintf(label, sizeof(label), "%s", jstr(d, "label"));
			clean(id);
			clean(label);
			clean(name);
			fprintf(out, "offer=%s\t%s\t%s\t%s\n", id, label, d->string, name);
		}
	}
}

static char *build_snapshot(void)
{
	char *text = NULL, user[64];
	size_t len = 0;
	FILE *out = open_memstream(&text, &len);
	struct conf c;
	bool running = child > 0;

	if (!out)
		return NULL;
	if (active_user(user, sizeof(user)))
		conf_load(user, &c);
	else
		memset(&c, 0, sizeof(c));
	fprintf(out, "enabled=%d\nrunning=%d\nready=%d\nfailed=%d\ninternet=%d\nrelays=%d\n",
		c.enabled, running, running && ready, failed, c.internet, c.relays);
	if (running && ready) {
		long status;
		cJSON *devices = rest("GET", "/rest/config/devices", NULL, &status);
		cJSON *conns = rest("GET", "/rest/system/connections", NULL, &status);
		cJSON *pending = rest("GET", "/rest/cluster/pending/devices", NULL, &status);
		cJSON *nearby = rest("GET", "/rest/system/discovery", NULL, &status);
		cJSON *offers = rest("GET", "/rest/cluster/pending/folders", NULL, &status);
		const cJSON *d, *map = cJSON_GetObjectItemCaseSensitive(conns, "connections");

		prune_requests(pending);
		fprintf(out, "device_id=%s\n", my_id);
		cJSON_ArrayForEach(d, devices)
			if (!strcmp(jstr(d, "deviceID"), my_id)) {
				char name[160];

				snprintf(name, sizeof(name), "%s", jstr(d, "name"));
				clean(name);
				fprintf(out, "device_name=%s\n", name);
			}
		pthread_mutex_lock(&ev_lock);
		for (int i = 0; i < KINDS; i++)
			fprintf(out, "folder=%s\t%d\t%s\n", KIND[i], c.folder[i], c.folder[i] ? kind_state(i) : "");
		print_folder_ids(out, user, &c);
		cJSON_ArrayForEach(d, devices) {
			const char *id = jstr(d, "deviceID");
			const cJSON *conn = cJSON_GetObjectItemCaseSensitive(map, id);
			bool connected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(conn, "connected"));
			char name[160], host[96] = "";
			int pct = -1;

			if (!strcmp(id, my_id))
				continue;
			for (int i = 0; i < MAX_DEVICES; i++)
				if (!strcmp(completion[i].id, id))
					for (int k = 0; k < FOLDERS; k++)
						if (c.folder[FOLDER[k].kind] && (pct < 0 || completion[i].pct[k] < pct))
							pct = completion[i].pct[k];
			if (connected)
				strip_port(jstr(conn, "address"), host, sizeof(host));
			snprintf(name, sizeof(name), "%s", jstr(d, "name"));
			clean(name);
			fprintf(out, "device=%s\t%s\t%d\t%d\t%s\n", id, name, connected, connected ? pct : -1, host);
		}
		pthread_mutex_unlock(&ev_lock);
		for (d = pending ? pending->child : NULL; d; d = d->next) {
			char name[160], addr[96];

			if (!d->string || !valid_device(d->string))
				continue;
			snprintf(name, sizeof(name), "%s", jstr(d, "name"));
			strip_port(jstr(d, "address"), addr, sizeof(addr));
			clean(name);
			fprintf(out, "pending=%s\t%s\t%s\n", d->string, name, addr);
		}
		for (d = nearby ? nearby->child : NULL; d; d = d->next) {
			char host[96];

			if (!d->string || !valid_device(d->string) || is_configured(devices, d->string) ||
			    (pending && cJSON_GetObjectItemCaseSensitive(pending, d->string)))
				continue;
			host_of(cJSON_GetObjectItemCaseSensitive(d, "addresses"), host, sizeof(host));
			fprintf(out, "nearby=%s\t%s\n", d->string, host);
		}
		print_offers(out, offers, devices);
		notify_requests(pending);
		cJSON_Delete(devices);
		cJSON_Delete(conns);
		cJSON_Delete(pending);
		cJSON_Delete(nearby);
		cJSON_Delete(offers);
	} else {
		for (int i = 0; i < KINDS; i++)
			fprintf(out, "folder=%s\t%d\t\n", KIND[i], c.folder[i]);
		if (user[0] && valid_user(user))
			print_folder_ids(out, user, &c);
		if (!running)
			notify_requests(NULL);
	}
	fputs("end=1\n", out);
	fclose(out);
	return text;
}

static int write_all(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(fd, s, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		s += w;
		n -= (size_t)w;
	}
	return 0;
}

static void drop_client(struct client *c)
{
	close(c->fd);
	c->fd = -1;
	c->sub = false;
	c->used = 0;
}

/* New snapshot; subscribers get it only when it changed. */
static void publish(void)
{
	char *next = build_snapshot();

	if (!next)
		return;
	if (snapshot && !strcmp(snapshot, next)) {
		free(next);
		return;
	}
	free(snapshot);
	snapshot = next;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].sub && write_all(clients[i].fd, snapshot, strlen(snapshot)))
			drop_client(&clients[i]);
}

/* ---------------------------------------------------------------- */
/* Syncthing process                                                */
/* ---------------------------------------------------------------- */

static void on_alarm(int sig)
{
	(void)sig;
}

static void stop_events(void)
{
	pthread_mutex_lock(&ev_lock);
	ev_active = false;
	ev_gen++;
	pthread_mutex_unlock(&ev_lock);
}

/* Syncthing closes its database on SIGTERM: wait for it, at most 10 s. */
static void stop_child(void)
{
	struct sigaction sa, old;

	if (child <= 0)
		return;
	stop_events();
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm; /* no SA_RESTART: interrupts waitpid */
	sigaction(SIGALRM, &sa, &old);
	kill(child, SIGTERM);
	alarm(10);
	if (waitpid(child, NULL, 0) != child) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
	alarm(0);
	sigaction(SIGALRM, &old, NULL);
	child = -1;
	ready = false;
	my_id[0] = '\0';
	unlink(GUI_SOCKET);
	child_user[0] = '\0';
}

static void start_child(const char *user)
{
	char home[PATH_MAX], arg_home[PATH_MAX + 16], arg_key[160];
	pid_t pid;

	if (!cur.key[0])
		return;
	snprintf(home, sizeof(home), USERDATA_USERS "/%s/appdata/syncthing", user);
	snprintf(arg_home, sizeof(arg_home), "--home=%s", home);
	snprintf(arg_key, sizeof(arg_key), "--gui-apikey=%s", cur.key);
	mkdir(GUI_DIR, 0700);
	unlink(GUI_SOCKET);
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		int log = open(RUN_ROOT "/syncthing.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		char *argv[] = {
			(char *)SYNCTHING_BIN, (char *)"serve", arg_home, arg_key,
			(char *)"--no-browser", (char *)"--no-restart", (char *)"--no-upgrade",
			(char *)"--gui-address=unix://" GUI_SOCKET, NULL,
		};
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		clearenv();
		setenv("PATH", "/usr/bin:/bin", 1);
		setenv("HOME", home, 1);
		/* No ~/Sync default folder: nuubOS shares the user's folders. */
		setenv("STNODEFAULTFOLDER", "1", 1);
		setenv("STNOUPGRADE", "1", 1);
		/* Run the server itself, not its monitor (a second ~44 MB
		 * process that only restarts it): syncd supervises. */
		setenv("STMONITORED", "yes", 1);
		/* Less heap kept between Go collections (1 GB device, games
		 * next to it): collect at +50 % instead of +100 % and work
		 * harder above 32 MiB (a soft limit, never an allocation
		 * failure). */
		setenv("GOGC", "50", 1);
		setenv("GOMEMLIMIT", "32MiB", 1);
		/* Background work must not slow a game down. */
		if (nice(10) < 0)
			perror("nice"); /* keeps the default priority */
		execv(SYNCTHING_BIN, argv);
		_exit(127);
	}
	child = pid;
	ready = false;
	snprintf(child_user, sizeof(child_user), "%s", user);
	failed = false;
	fprintf(stderr, "syncd: syncthing started for %s (pid %d)\n", user, (int)pid);
}

static void reconcile(void)
{
	static char last_user[64];
	char user[64];
	bool want;

	if (!active_user(user, sizeof(user)))
		user[0] = '\0';
	if (strcmp(user, last_user)) {
		/* Another user: a failure of the previous session does not carry over. */
		failed = false;
		snprintf(last_user, sizeof(last_user), "%s", user);
	}
	if (user[0])
		conf_load(user, &cur);
	else
		memset(&cur, 0, sizeof(cur));
	want = user[0] && cur.enabled && cur.key[0];
	if (child > 0 && (!want || strcmp(child_user, user))) {
		stop_child();
		failed = false;
	}
	/* Not before nuubUI is ready: its start indexes the folders, and at
	 * boot it slowed the UI coming up (boot audit 2026-10-22). */
	if (want && child <= 0 && !failed && access(UI_READY, F_OK) == 0)
		start_child(user);
	/* The socket may already be there (an inotify event coalesced). */
	if (child > 0 && !ready && access(GUI_SOCKET, F_OK) == 0)
		on_ready();
}

/* ---------------------------------------------------------------- */
/* Commands                                                         */
/* ---------------------------------------------------------------- */

static const char *cmd_enable(bool on)
{
	char user[64];
	struct conf c;

	if (!active_user(user, sizeof(user)))
		return "no-user";
	conf_load(user, &c);
	if (!c.key[0]) {
		/* A per-user API key, created once. */
		if (!read_line("/proc/sys/kernel/random/uuid", c.key, sizeof(c.key)))
			return "storage";
	}
	c.enabled = on;
	if (conf_save(user, &c))
		return "storage";
	if (on)
		failed = false;
	reconcile();
	return NULL;
}

static const char *cmd_conf(const char *what, const char *arg, const char *value)
{
	char user[64];
	struct conf c;
	int k = -1;

	if (strcmp(value, "0") && strcmp(value, "1"))
		return "value";
	if (!active_user(user, sizeof(user)))
		return "no-user";
	conf_load(user, &c);
	if (!strcmp(what, "FOLDER")) {
		for (int i = 0; i < KINDS; i++)
			if (!strcmp(arg, KIND[i]))
				k = i;
		if (k < 0)
			return "value";
		c.folder[k] = value[0] == '1';
	} else if (!strcmp(what, "RELAYS")) {
		c.relays = value[0] == '1';
	} else {
		c.internet = value[0] == '1';
	}
	if (conf_save(user, &c))
		return "storage";
	if (child > 0 && !strcmp(child_user, user)) {
		cur = c;
		if (ready && !(k >= 0 ? apply_folders() : apply_options()))
			return "syncthing";
	}
	return NULL;
}

/* FOLDER_ID / USE: saves, states or screenshots on an existing folder id
 * ("-" = back to nuubOS's own); USE also turns the kind on. The folder
 * that had that path before stops syncing (apply_folders), files stay. */
static const char *cmd_folder_id(const char *kind, const char *id, bool use)
{
	char user[64], ids[FOLDERS][FOLDER_ID_MAX], labels[FOLDERS][FOLDER_ID_MAX];
	struct conf c;
	int k = -1;
	bool reset = !strcmp(id, "-");

	for (int i = 0; i < KINDS; i++)
		if (!strcmp(kind, KIND[i]))
			k = i;
	if (!custom_kind(k) || (!reset && !valid_folder_id(id)) || (use && reset))
		return "value";
	if (!active_user(user, sizeof(user)))
		return "no-user";
	conf_load(user, &c);
	snprintf(c.custom[k], sizeof(c.custom[k]), "%s", reset ? "" : id);
	/* One folder per kind: never the id of another kind. */
	compute_ids(user, &c, ids, labels);
	for (int i = 0; i < FOLDERS; i++)
		for (int j = i + 1; j < FOLDERS; j++)
			if (!strcmp(ids[i], ids[j]))
				return "value";
	if (use)
		c.folder[k] = true;
	if (conf_save(user, &c))
		return "storage";
	if (child > 0 && !strcmp(child_user, user)) {
		cur = c;
		pthread_mutex_lock(&ev_lock);
		folder_ids(user);
		for (int i = 0; i < FOLDERS; i++)
			if (FOLDER[i].kind == k)
				folder_state[i][0] = '\0';
		pthread_mutex_unlock(&ev_lock);
		if (ready && !apply_folders())
			return "syncthing";
	}
	return NULL;
}

/* DECLINE: into the offering devices' ignoredFolders, so the offer does not
 * come back, then the pending entry is dropped. */
static const char *cmd_decline(const char *folder)
{
	char q[FOLDER_ID_MAX * 3 + 48];
	long status;
	cJSON *offers, *by, *d;
	bool ok = true;

	if (!valid_folder_id(folder))
		return "value";
	if (child <= 0 || !ready)
		return "not-running";
	offers = rest("GET", "/rest/cluster/pending/folders", NULL, &status);
	by = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(offers, folder), "offeredBy");
	cJSON_ArrayForEach(d, by) {
		char path[ID_MAX + 32];
		cJSON *dev, *list, *e;

		if (!d->string || !valid_device(d->string))
			continue;
		snprintf(path, sizeof(path), "/rest/config/devices/%s", d->string);
		dev = rest("GET", path, NULL, &status);
		if (status != 200 || !cJSON_IsObject(dev)) {
			cJSON_Delete(dev);
			ok = false;
			continue;
		}
		list = cJSON_GetObjectItemCaseSensitive(dev, "ignoredFolders");
		if (!cJSON_IsArray(list)) {
			cJSON_DeleteItemFromObject(dev, "ignoredFolders");
			list = cJSON_AddArrayToObject(dev, "ignoredFolders");
		}
		e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "id", folder);
		cJSON_AddStringToObject(e, "label", jstr(d, "label"));
		cJSON_AddStringToObject(e, "time", jstr(d, "time"));
		cJSON_AddItemToArray(list, e);
		ok = rest_ok("PUT", path, dev) && ok;
		cJSON_Delete(dev);
	}
	cJSON_Delete(offers);
	folder_url(q, sizeof(q), "/rest/cluster/pending/folders?folder=", folder);
	cJSON_Delete(rest("DELETE", q, NULL, &status));
	return ok ? NULL : "syncthing";
}

static const char *cmd_device(const char *verb, const char *id, const char *name)
{
	char path[ID_MAX + 32];

	if (!valid_device(id) || !strcmp(id, my_id))
		return "device";
	if (child <= 0 || !ready)
		return "not-running";
	snprintf(path, sizeof(path), "/rest/config/devices/%s", id);
	if (!strcmp(verb, "ADD")) {
		cJSON *d = cJSON_CreateObject();
		bool ok;

		cJSON_AddStringToObject(d, "deviceID", id);
		if (name && name[0])
			cJSON_AddStringToObject(d, "name", name);
		cJSON_AddItemToObject(d, "addresses", cJSON_CreateStringArray((const char *[]){ "dynamic" }, 1));
		ok = rest_ok("PUT", path, d);
		cJSON_Delete(d);
		/* Share the user's folders with it. */
		return ok && apply_folders() ? NULL : "syncthing";
	}
	if (!strcmp(verb, "REMOVE")) {
		long status;

		cJSON_Delete(rest("DELETE", path, NULL, &status));
		return status == 200 || status == 404 ? NULL : "syncthing";
	}
	/* IGNORE: into remoteIgnoredDevices, so it stops asking. */
	{
		long status;
		cJSON *cfg = rest("GET", "/rest/config", NULL, &status), *list, *e;
		bool ok;

		if (status != 200 || !cJSON_IsObject(cfg)) {
			cJSON_Delete(cfg);
			return "syncthing";
		}
		list = cJSON_GetObjectItemCaseSensitive(cfg, "remoteIgnoredDevices");
		if (!cJSON_IsArray(list)) {
			cJSON_DeleteItemFromObject(cfg, "remoteIgnoredDevices");
			list = cJSON_AddArrayToObject(cfg, "remoteIgnoredDevices");
		}
		e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "deviceID", id);
		cJSON_AddItemToArray(list, e);
		ok = rest_ok("PUT", "/rest/config", cfg);
		cJSON_Delete(cfg);
		if (ok) {
			char q[ID_MAX + 48];

			snprintf(q, sizeof(q), "/rest/cluster/pending/devices?device=%s", id);
			cJSON_Delete(rest("DELETE", q, NULL, &status));
		}
		return ok ? NULL : "syncthing";
	}
}

static void command(struct client *c, char *line)
{
	char *f[4] = { 0 };
	int n = 0;
	const char *err = "unknown";

	line[strcspn(line, "\r")] = '\0';
	for (char *p = line; n < 4; n++) {
		f[n] = p;
		p = strchr(p, '\t');
		if (!p) {
			n++;
			break;
		}
		*p++ = '\0';
	}
	if (n == 1 && (!strcmp(f[0], "STATUS") || !strcmp(f[0], "SUBSCRIBE"))) {
		if (!snapshot)
			publish();
		c->sub = c->sub || !strcmp(f[0], "SUBSCRIBE");
		if (snapshot && write_all(c->fd, snapshot, strlen(snapshot)))
			drop_client(c);
		return;
	}
	if (n == 1 && (!strcmp(f[0], "ENABLE") || !strcmp(f[0], "DISABLE")))
		err = cmd_enable(f[0][0] == 'E');
	else if (n == 3 && !strcmp(f[0], "FOLDER"))
		err = cmd_conf(f[0], f[1], f[2]);
	else if (n == 2 && (!strcmp(f[0], "INTERNET") || !strcmp(f[0], "RELAYS")))
		err = cmd_conf(f[0], "", f[1]);
	else if ((n == 2 || n == 3) && !strcmp(f[0], "ADD"))
		err = cmd_device(f[0], f[1], n == 3 ? f[2] : NULL);
	else if (n == 2 && (!strcmp(f[0], "REMOVE") || !strcmp(f[0], "IGNORE")))
		err = cmd_device(f[0], f[1], NULL);
	else if (n == 3 && !strcmp(f[0], "FOLDER_ID"))
		err = cmd_folder_id(f[1], f[2], false);
	else if (n == 3 && !strcmp(f[0], "USE"))
		err = cmd_folder_id(f[2], f[1], true);
	else if (n == 2 && !strcmp(f[0], "DECLINE"))
		err = cmd_decline(f[1]);
	if (err) {
		char reply[64];

		snprintf(reply, sizeof(reply), "ERR %s\n", err);
		if (write_all(c->fd, reply, strlen(reply)))
			drop_client(c);
		return;
	}
	if (write_all(c->fd, "OK\n", 3))
		drop_client(c);
	publish();
}

static void service(struct client *c)
{
	char b[512];
	ssize_t n = read(c->fd, b, sizeof(b));

	if (n <= 0) {
		drop_client(c);
		return;
	}
	for (ssize_t i = 0; i < n && c->fd >= 0; i++) {
		if (b[i] == '\n') {
			c->buf[c->used] = '\0';
			c->used = 0;
			command(c, c->buf);
		} else if (c->used + 1 < sizeof(c->buf)) {
			c->buf[c->used++] = b[i];
		} else {
			c->used = 0;
		}
	}
}

/* New covers/metadata from another device: libraryd's scan refreshes
 * its metadata index and the covers. Fire and forget. */
static void library_scan(void)
{
	struct sockaddr_un a;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", LIBRARY_SOCKET);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && send(fd, "SCAN\n", 5, MSG_NOSIGNAL) == 5)
		fprintf(stderr, "syncd: library rescan requested\n");
	close(fd);
}

static int listener(void)
{
	struct sockaddr_un a;
	int fd;

	unlink(SOCKET_PATH);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", SOCKET_PATH);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 8)) {
		close(fd);
		return -1;
	}
	chmod(SOCKET_PATH, 0660);
	return fd;
}

static void accept_clients(int l)
{
	for (;;) {
		int fd = accept4(l, NULL, NULL, SOCK_CLOEXEC), i;

		if (fd < 0)
			return;
		for (i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd < 0)
				break;
		if (i == MAX_CLIENTS) {
			close(fd);
			continue;
		}
		clients[i] = (struct client){ .fd = fd };
	}
}

int main(void)
{
	struct sigaction sa;
	pthread_t thread;
	sigset_t block, old;
	int in, l, gui_wd = -1, ui_wd = -1, storage_wd = -1;
	FILE *fp;

	if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;
	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	signal(SIGPIPE, SIG_IGN);
	/* The event thread never takes the signals. */
	sigemptyset(&block);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGCHLD);
	sigaddset(&block, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &block, &old);
	if (pthread_create(&thread, NULL, event_thread, NULL) != 0)
		return 1;
	pthread_detach(thread);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);
	mkdir(RUN_ROOT, 0755);
	mkdir(USER_DIR, 0755);
	mkdir(GUI_DIR, 0700);
	in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (in < 0 || inotify_add_watch(in, USER_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE) < 0 ||
	    (gui_wd = inotify_add_watch(in, GUI_DIR, IN_CREATE)) < 0)
		fprintf(stderr, "syncd: inotify unavailable\n");
	/* SD2 in or out: the SD2 ROM folder pauses or resumes. */
	if (in >= 0)
		storage_wd = inotify_add_watch(in, STORAGE_DIR, IN_CLOSE_WRITE | IN_MOVED_TO);
	/* Until nuubUI is ready (UI_READY): then the watch goes. */
	if (in >= 0 && access(UI_READY, F_OK) != 0)
		ui_wd = inotify_add_watch(in, RUN_ROOT, IN_CREATE | IN_MOVED_TO);
	l = listener();
	if (l < 0)
		return 1;
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}
	reconcile();
	publish();
	while (!stop_requested) {
		struct pollfd p[3 + MAX_CLIENTS];
		int map[3 + MAX_CLIENTS], n = 0, status;
		bool changed = false, sd2_changed = false;
		char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
		ssize_t len;
		pid_t pid;

		p[n] = (struct pollfd){ wake_pipe[0], POLLIN, 0 };
		map[n++] = -1;
		p[n] = (struct pollfd){ l, POLLIN, 0 };
		map[n++] = -2;
		if (in >= 0) {
			p[n] = (struct pollfd){ in, POLLIN, 0 };
			map[n++] = -3;
		}
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0) {
				p[n] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
				map[n++] = i;
			}
		if (poll(p, (nfds_t)n, -1) < 0 && errno != EINTR)
			break;
		for (int j = 0; j < n; j++) {
			if (!p[j].revents)
				continue;
			if (map[j] == -1) {
				while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
					;
				/* Syncthing events, a signal or a child. */
				changed = true;
			} else if (map[j] == -2) {
				accept_clients(l);
			} else if (map[j] == -3) {
				while ((len = read(in, buf, sizeof(buf))) > 0)
					for (char *q = buf; q < buf + len;) {
						const struct inotify_event *ev = (const struct inotify_event *)q;

						if (ev->wd == storage_wd) {
							if (ev->len && !strcmp(ev->name, "sd2.status"))
								sd2_changed = true;
						} else if (ev->wd == ui_wd) {
							if (ev->len && !strcmp(ev->name, "ui-ready")) {
								inotify_rm_watch(in, ui_wd);
								ui_wd = -1;
								changed = true;
							}
						} else if (ev->wd != gui_wd || (ev->len && !strcmp(ev->name, GUI_NAME)))
							changed = true;
						q += sizeof(*ev) + ev->len;
					}
			} else if (p[j].revents & (POLLHUP | POLLERR)) {
				drop_client(&clients[map[j]]);
			} else {
				service(&clients[map[j]]);
			}
		}
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
			if (pid == child) {
				fprintf(stderr, "syncd: syncthing exited status=%d\n", status);
				stop_events();
				child = -1;
				ready = false;
				my_id[0] = '\0';
				child_user[0] = '\0';
				failed = true;
				changed = true;
			}
		if (reload_requested) {
			reload_requested = 0;
			failed = false;
			changed = true;
		}
		if (sd2_changed && ready && cur.folder[KIND_ROMS_SD2]) {
			if (!apply_folders())
				fprintf(stderr, "syncd: SD2 ROM folder not updated\n");
			changed = true;
		}
		if (changed) {
			bool rescan;

			reconcile();
			publish();
			pthread_mutex_lock(&ev_lock);
			rescan = library_changed;
			library_changed = false;
			pthread_mutex_unlock(&ev_lock);
			if (rescan)
				library_scan();
		}
	}
	stop_child();
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0)
			close(clients[i].fd);
	close(l);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
