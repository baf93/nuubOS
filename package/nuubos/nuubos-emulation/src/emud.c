/* SPDX-License-Identifier: MIT */
/*
 * nuubos-emud — Emulation Service (EPIC-013 RetroArch Integration,
 * EPIC-019 Saves & Save States, EPIC-020 sessions, EPIC-005 game actions).
 *
 * Owns the running game session. A Library game is launched in RetroArch
 * (the emulation backend) with:
 *   - the qualified core for its system (cores.conf: bundled cores first,
 *     then cores the user downloaded with the RetroArch Online Updater);
 *   - the active user's RetroArch configuration, seeded once from the
 *     nuubOS H700 defaults and freely editable in RetroArch Advanced;
 *   - a session configuration written at every launch with what nuubOS
 *     must control: per-user/per-system save and state directories, shared
 *     BIOS and cores, drivers, the nuubOS game gamepads, the stdin command
 *     interface and the UI language.
 *
 * The session is controlled through RetroArch's stdin command interface
 * (never the UDP network interface). Every client (Quick Menu, controller
 * hotkeys, lifecycle hooks) uses this service, so pause, save/load state,
 * slot, reset, quit and RetroArch Advanced have a single implementation.
 * Controllers reach the game only through nuubos-inputd game gamepads,
 * switched on for the lifetime of the session connection.
 *
 * Protocol (/run/nuubos/emud.sock, one command per line, tab separated
 * arguments): STATUS and SUBSCRIBE return the session snapshot (SUBSCRIBE
 * pushes a new one on every change); LAUNCH <game> [resume|new|slot:N],
 * STATES [game], SETTINGS, SET <key> <value>, PAUSE, RESUME,
 * SAVE_STATE, LOAD_STATE, SLOT <n|+1|-1>, RESET, FAST_FORWARD, ADVANCED,
 * QUIT, SCREENSHOT, OVERLAY_TEXT <line> reply OK or ERR <reason>; PRE_POWER <sleep|restart|poweroff>
 * replies once the game state is safe.
 *
 * Per-game configuration (EPIC-018): GAME_SETTINGS <game> <system> lists the
 * effective values and the user's explicit overrides; GAME_SET <game>
 * <system> <key> <value> (empty value = inherit) and GAME_RESET <game>
 * change them. Only overrides are stored, per user, in
 * /userdata/users/<id>/appdata/nuubos-emulation/games/<game>.conf. They
 * apply at the next launch: core choice, aspect ratio and video filter on
 * top of the user's RetroArch configuration. RetroArch saves its whole
 * configuration on exit, so the keys a session overrode are put back to
 * the user's own values when it ends.
 *
 * RetroAchievements (EPIC-017): a user logged in with
 * nuubos-achievementsctl has /state/users/<id>/secrets/retroachievements.conf
 * (user name and API token, never the password); the session enables
 * RetroArch's achievements with that token, and hardcore mode when chosen. Outcomes the user must see (state saved,
 * empty slot, launch failure...) are typed notifications (EPIC-006).
 *
 * Idle: poll() without timeout. A timeout is armed only while a save or a
 * quit is pending.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

/* Roots are overridable at build time for host tests. */
#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#define SOCKET_PATH RUN_ROOT "/emud.sock"
#define PIDFILE RUN_ROOT "/emud.pid"
#define SESSION_DIR RUN_ROOT "/emulation"
#define SESSION_CFG SESSION_DIR "/session.cfg"
#define SESSION_SHADER SESSION_DIR "/session.glslp"
#define RETROARCH_LOG RUN_ROOT "/retroarch.log"
#define LIBRARY_SOCKET RUN_ROOT "/libraryd.sock"
#define INPUT_SOCKET RUN_ROOT "/inputd.sock"
#define ACTIVE_USER_FILE RUN_ROOT "/user/active"
#define WAYLAND_RUNTIME RUN_ROOT "/wayland-runtime"
#define PIPEWIRE_RUNTIME RUN_ROOT "/pipewire"

#ifndef RETROARCH_BIN
#define RETROARCH_BIN "/usr/bin/retroarch"
#endif
#ifndef SHARE_DIR
#define SHARE_DIR "/usr/share/nuubos/emulation"
#endif
#define CORES_CONF SHARE_DIR "/cores.conf"
#define DEFAULT_RA_CFG SHARE_DIR "/retroarch.cfg"
#define DEFAULT_CORE_OPTIONS SHARE_DIR "/retroarch-core-options.cfg"
#define DEFAULT_SHADER_PRESET SHARE_DIR "/shaders/global.glslp"
#define SHADER_DIR SHARE_DIR "/shaders"
/* Performance overlay font (RetroArch patch 0004 draws with the OSD font). */
#define OSD_FONT "/usr/share/fonts/cantarell/Cantarell-Bold.otf"
#define DISPLAY_CONF SHARE_DIR "/display.conf"
/* Game render height on HDMI: RetroArch (patch 0005) draws a buffer of
 * this many lines and the display engine scales it to the 1080p mode on
 * its plane (kernel patch 0031), so the frontend's per-pixel cost stays
 * near the handheld panel's (N64 at a 1080p buffer: ~1/3 of full speed).
 * 360 keeps Majora's Mask PAL at full speed; 240p content then fits at
 * 1.5x, below the CRT look's 2x scanlines. display.conf field 3 overrides
 * it per system (0 = native resolution). The panel never uses it. */
#define GAME_HDMI_LINES 360
#ifndef HDMI_ENABLED
#define HDMI_ENABLED "/sys/class/drm/card0-HDMI-A-1/enabled"
#endif
#ifndef SYSTEM_ICONS
#define SYSTEM_ICONS "/usr/share/nuubos/systems"
#endif
#ifndef SYSTEMS_CONF
#define SYSTEMS_CONF "/usr/share/nuubos/systems.conf"
#endif
/* Support files some cores load from the system directory (PPSSPP fonts
 * and UI atlas, ScummVM engine data and themes). */
#define SYSTEM_FILES SHARE_DIR "/system"
#ifndef BUNDLED_CORES
#define BUNDLED_CORES "/usr/lib/libretro"
#endif
#define CORE_INFO_DIR "/usr/share/libretro/info"
#define RA_ASSETS_DIR "/usr/share/retroarch/assets"
#define RA_AUTOCONFIG_DIR "/usr/share/retroarch/autoconfig"

#ifndef USERDATA
#define USERDATA "/userdata"
#endif
#define DOWNLOADED_CORES USERDATA "/retroarch/cores"
#define BIOS_DIR USERDATA "/bios"
#define CHEATS_DIR USERDATA "/cheats"
#define CACHE_DIR USERDATA "/cache/retroarch"
#define USERS_DIR USERDATA "/users"
#ifndef STATE_USERS
#define STATE_USERS "/state/users"
#endif
#define GAME_SETTINGS_SUBDIR "appdata/nuubos-emulation/games"
#define EMU_SETTINGS_FILE "appdata/nuubos-emulation/settings.conf"
#define ACHIEVEMENTS_FILE "secrets/retroachievements.conf"

#define MAX_CLIENTS 16
#define MAX_LINE 1024
#define MAX_PLAYERS 8
/* Save state slots per game: user setting (default 10, at most 99). */
#define DEFAULT_SLOTS 10
#define MAX_SLOTS 99
/* RetroArch's automatic slot (.state.auto), used before power actions. */
#define AUTO_SLOT -1
#define SAVE_TIMEOUT_MS 10000
#define QUIT_TIMEOUT_MS 10000
#define KILL_TIMEOUT_MS 4000
/* A session ending this early with an error never really started. */
#define START_FAILURE_MS 5000
#define LIBRARY_TIMEOUT_MS 3000
#define SCREENSHOT_TIMEOUT_MS 5000
#define MAX_RESTORE_KEYS 24
/* Performance overlay: RetroArch frame rate sampled once a second. */
#define OVERLAY_TICK_MS 1000

enum session_state {
	SESSION_IDLE = 0,
	SESSION_RUNNING,
	SESSION_EXITING,
};

enum pending_kind {
	PENDING_NONE = 0,
	PENDING_SAVE,
	PENDING_LOAD,
};

/*
 * The user's emulation settings (Settings -> Gaming), stored in
 * /userdata/users/<id>/appdata/nuubos-emulation/settings.conf:
 *   boot_screens  start with the console's own boot sequence (BIOS logo)
 *   slots         save state slots per game
 *   slot_rotation each save goes to the next slot, wrapping to 0
 *   bezels, crt   picture presentation (bezel.glsl, crt.glsl)
 *   overlay       performance overlay over games, overlay_items its fields
 *   hotkey_<action> chord control, held with the Quick Menu button
 */
enum hotkey_index {
	HK_SAVE,
	HK_LOAD,
	HK_SLOT_NEXT,
	HK_SLOT_PREV,
	HK_FAST_FORWARD,
	HK_QUIT,
	HK_SCREENSHOT,
	HK_COUNT,
};

static const char *const hotkey_actions[HK_COUNT] = {
	"save_state", "load_state", "slot_next", "slot_prev",
	"fast_forward", "quit", "screenshot",
};

/* nuubos-controllersd control ids (the user's mapping applies). */
static const char *const hotkey_defaults[HK_COUNT] = {
	"r1", "l1", "menu_up", "menu_down", "face_north", "settings", "l2",
};

static const char *const hotkey_controls[] = {
	"menu_confirm", "menu_back", "face_north", "face_west",
	"menu_up", "menu_down", "menu_left", "menu_right",
	"l1", "r1", "l2", "r2", "l3", "r3", "settings", "select", NULL,
};

static const char *const overlay_fields[] = {
	"fps", "cpu", "gpu", "ram", "temp", "power", "battery", "clock", NULL,
};
#define DEFAULT_OVERLAY_ITEMS "fps,cpu,ram,temp,battery,clock"

struct emu_settings {
	bool boot_screens;
	int slots;
	bool slot_rotation;
	bool bezels;
	bool crt;
	bool overlay;
	char overlay_items[96];
	char hotkey[HK_COUNT][16];
};

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

struct session {
	enum session_state state;
	pid_t pid;
	int stdin_fd;
	int stdout_fd;
	int input_fd;
	char out_buf[MAX_LINE];
	size_t out_used;
	char user[64];
	char game[32];
	char system[64];
	char core[64];
	char content[PATH_MAX];
	char states_dir[PATH_MAX];
	char state_base[PATH_MAX];
	int slot;
	bool paused;
	bool fast_forward;
	long long started_ms;
	long long quit_deadline_ms;
	bool term_sent;
	bool kill_sent;
	/* One state operation at a time: RetroArch replies once the state is
	 * serialized; a save is complete when its file is closed. */
	enum pending_kind pending;
	int pending_slot;
	bool pending_replied;
	bool pending_quiet;
	char pending_name[NAME_MAX + 1];
	long long pending_deadline_ms;
	int inotify_fd;
	int inotify_wd;
	char input_buf[128];
	size_t input_used;
	/* A lifecycle hook waiting for PRE_POWER completion. */
	int power_client;
	char power_action[16];
	/* Screenshot confirmation: a new PNG closed in the screenshot folder. */
	int shot_fd;
	long long shot_deadline_ms;
	char screenshots_dir[PATH_MAX];
	/* User RetroArch keys this session overrode, restored at exit
	 * (value NULL = the key was absent). */
	char ra_cfg[PATH_MAX];
	int restore_count;
	char restore_key[MAX_RESTORE_KEYS][48];
	char *restore_value[MAX_RESTORE_KEYS];
	/* The user's settings when the game started (hotkeys and overlay
	 * follow later changes). */
	struct emu_settings settings;
	/* Performance overlay: last RetroArch frame rate, next sample. */
	double fps;
	long long overlay_tick_ms;
};

/* Explicit per-game overrides; empty = inherited. */
struct game_overrides {
	char core[64];
	char aspect[16];
	char filter[16];
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];
static struct session session;

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void handle_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void handle_sigchld(int sig)
{
	int saved = errno;

	(void)sig;
	if (sigchld_pipe[1] >= 0 && write(sigchld_pipe[1], "c", 1) < 0) {
		/* Pipe full: a wakeup is already pending. */
	}
	errno = saved;
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static void trim(char *text)
{
	size_t len = strlen(text);

	while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
			   isspace((unsigned char)text[len - 1])))
		text[--len] = '\0';
}

static int write_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, data, len, MSG_NOSIGNAL);

		if (n < 0 && errno == ENOTSOCK)
			n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	char *p;

	copy_text(tmp, sizeof(tmp), path);
	for (p = tmp + 1; *p; p++) {
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

static bool file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Copies a nuubOS default into the user's configuration once; the user's
 * own changes (RetroArch Advanced) are never overwritten. */
static int seed_file(const char *src, const char *dst)
{
	char tmp[PATH_MAX];
	char buf[8192];
	int in;
	int out;
	ssize_t n;
	int rc = 0;

	if (file_exists(dst))
		return 0;
	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
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
	if (n < 0 || fsync(out) != 0)
		rc = -1;
	close(in);
	if (close(out) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, dst) != 0)
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

/* Appends to dst the "key = value" lines of src whose key dst does not
 * have yet: options of cores added by a newer image reach existing users,
 * values the user already has are never changed. */
static void merge_missing_keys(const char *src, const char *dst)
{
	char line[512];
	char key[128];
	FILE *in = fopen(src, "r");
	FILE *cur;
	FILE *out;

	if (!in)
		return;
	while (fgets(line, sizeof(line), in)) {
		bool present = false;
		char other[512];

		if (sscanf(line, " %127[^ =#] =", key) != 1)
			continue;
		cur = fopen(dst, "r");
		while (cur && !present && fgets(other, sizeof(other), cur)) {
			char k[128];

			present = sscanf(other, " %127[^ =#] =", k) == 1 && !strcmp(k, key);
		}
		if (cur)
			fclose(cur);
		if (present)
			continue;
		out = fopen(dst, "a");
		if (!out)
			break;
		fputs(line, out);
		fclose(out);
	}
	fclose(in);
}

/* Copies every file of src missing in dst (recursively). Existing files are
 * the user's and are never replaced. */
static void seed_tree(const char *src, const char *dst)
{
	DIR *dir = opendir(src);
	struct dirent *entry;

	if (!dir)
		return;
	if (mkdir_p(dst) != 0) {
		closedir(dir);
		return;
	}
	while ((entry = readdir(dir)) != NULL) {
		char from[PATH_MAX];
		char to[PATH_MAX];
		struct stat st;

		if (entry->d_name[0] == '.')
			continue;
		snprintf(from, sizeof(from), "%s/%s", src, entry->d_name);
		snprintf(to, sizeof(to), "%s/%s", dst, entry->d_name);
		if (stat(from, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			seed_tree(from, to);
		else if (S_ISREG(st.st_mode) && seed_file(from, to) != 0)
			log_msg("emud: could not provide %s", to);
	}
	closedir(dir);
}

/* ------------------------------------------------------------------ */
/* Notifications (EPIC-006): typed events, the renderer localizes.     */
/* ------------------------------------------------------------------ */

static void notify_state(const char *event, int slot, const char *op)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.state", event);
	if (slot != AUTO_SLOT)
		nuubos_notify_int(&n, "slot", slot);
	if (op)
		nuubos_notify_str(&n, "op", op);
	(void)nuubos_notify_send(&n);
}

static void notify_failed(const char *reason)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.session", "game.failed");
	nuubos_notify_str(&n, "reason", reason);
	(void)nuubos_notify_send(&n);
}

static void notify_slot(int slot)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.state", "game.slot");
	nuubos_notify_int(&n, "slot", slot);
	(void)nuubos_notify_send(&n);
}

static void notify_screenshot(bool ok)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.screenshot",
			    ok ? "game.screenshot.saved" : "game.screenshot.failed");
	(void)nuubos_notify_send(&n);
}

static void notify_fast_forward(bool on)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.fastforward", "game.fastforward");
	nuubos_notify_str(&n, "state", on ? "on" : "off");
	(void)nuubos_notify_send(&n);
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

static const char *state_name(void)
{
	switch (session.state) {
	case SESSION_RUNNING:
		return "running";
	case SESSION_EXITING:
		return "exiting";
	default:
		return "idle";
	}
}

static size_t build_status(char *out, size_t size)
{
	int n = snprintf(out, size,
			 "state=%s\n"
			 "game=%s\n"
			 "system=%s\n"
			 "core=%s\n"
			 "paused=%d\n"
			 "slot=%d\n"
			 "fast_forward=%d\n"
			 "state_busy=%d\n"
			 "slots=%d\n"
			 "overlay=%d\n"
			 "overlay_items=%s\n"
			 "fps=%.1f\n"
			 "end=1\n",
			 state_name(),
			 session.state == SESSION_IDLE ? "" : session.game,
			 session.state == SESSION_IDLE ? "" : session.system,
			 session.state == SESSION_IDLE ? "" : session.core,
			 session.paused ? 1 : 0,
			 session.slot,
			 session.fast_forward ? 1 : 0,
			 session.pending != PENDING_NONE ? 1 : 0,
			 session.state == SESSION_IDLE ? DEFAULT_SLOTS : session.settings.slots,
			 session.state != SESSION_IDLE && session.settings.overlay ? 1 : 0,
			 session.state == SESSION_IDLE ? "" : session.settings.overlay_items,
			 session.fps);

	return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->used = 0;
	if (session.power_client >= 0 && &clients[session.power_client] == c)
		session.power_client = -1;
}

static void reply(struct client *c, const char *text)
{
	if (c->fd >= 0 && write_all(c->fd, text, strlen(text)) != 0)
		close_client(c);
}

static void notify_subscribers(void)
{
	char status[512];
	size_t len = build_status(status, sizeof(status));
	int i;

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd < 0 || !clients[i].subscribed)
			continue;
		if (write_all(clients[i].fd, status, len) != 0)
			close_client(&clients[i]);
	}
}

/* ------------------------------------------------------------------ */
/* Other services                                                      */
/* ------------------------------------------------------------------ */

static int connect_socket(const char *path, int timeout_ms)
{
	struct sockaddr_un addr;
	struct timeval tv;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	copy_text(addr.sun_path, sizeof(addr.sun_path), path);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int read_line_fd(int fd, char *out, size_t size)
{
	size_t used = 0;

	while (used + 1 < size) {
		char c;
		ssize_t n = read(fd, &c, 1);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		if (c == '\n')
			break;
		out[used++] = c;
	}
	out[used] = '\0';
	return 0;
}

/* One request/one line reply to nuubos-libraryd. */
static int library_request(const char *command, char *reply_out, size_t size)
{
	int fd = connect_socket(LIBRARY_SOCKET, LIBRARY_TIMEOUT_MS);
	int rc;

	if (fd < 0)
		return -1;
	if (write_all(fd, command, strlen(command)) != 0 ||
	    write_all(fd, "\n", 1) != 0) {
		close(fd);
		return -1;
	}
	rc = read_line_fd(fd, reply_out, size);
	close(fd);
	return rc;
}

static void library_session(const char *verb, const char *game)
{
	char command[128];
	char answer[128];

	snprintf(command, sizeof(command), "%s\t%s", verb, game);
	if (library_request(command, answer, sizeof(answer)) != 0 ||
	    strcmp(answer, "OK") != 0)
		log_msg("emud: libraryd %s %s failed: %s", verb, game,
			answer[0] ? answer : "no reply");
}

/* Game gamepads stay on for as long as this connection is open: if emud
 * dies, inputd gives the controllers back to the nuubUI by itself. */
static int input_gamepads_on(void)
{
	char answer[64];
	int fd = connect_socket(INPUT_SOCKET, 1000);

	if (fd < 0)
		return -1;
	if (write_all(fd, "GAMEPADS ON\n", 12) != 0 ||
	    read_line_fd(fd, answer, sizeof(answer)) != 0 ||
	    strcmp(answer, "OK") != 0) {
		close(fd);
		return -1;
	}
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

static bool read_active_user(char *out, size_t size)
{
	FILE *fp = fopen(ACTIVE_USER_FILE, "r");

	out[0] = '\0';
	if (!fp)
		return false;
	if (!fgets(out, (int)size, fp))
		out[0] = '\0';
	fclose(fp);
	trim(out);
	/* Users are UUIDs: never build paths from anything else. */
	for (const char *p = out; *p; p++)
		if (!isxdigit((unsigned char)*p) && *p != '-')
			return false;
	return out[0] != '\0';
}

/* RetroArch user_language for the user's nuubOS language. */
static int retroarch_language(const char *user)
{
	static const struct {
		const char *code;
		int retro;
	} langs[] = {
		{ "en", 0 }, { "fr", 2 }, { "es", 3 }, { "de", 4 },
		{ "it", 5 }, { "nl", 6 }, { "pt", 8 },
	};
	char path[PATH_MAX];
	char line[128];
	FILE *fp;
	int result = 0;

	snprintf(path, sizeof(path), STATE_USERS "/%s/localization.conf", user);
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (strncmp(line, "LANGUAGE=", 9) != 0)
			continue;
		for (size_t i = 0; i < sizeof(langs) / sizeof(langs[0]); i++)
			if (strcmp(line + 9, langs[i].code) == 0)
				result = langs[i].retro;
	}
	fclose(fp);
	return result;
}

/*
 * cores.conf: "<system>|<core>[,<core>...]" in order of preference. A
 * bundled (qualified) core wins over a downloaded one; a system without a
 * bundled core uses a core the user downloaded from RetroArch.
 */
static void content_base_name(const char *content, char *out, size_t size);

/* MAME machine and media switches of a system ("apple2e",
 * "dsk:-flop1,woz:-flop1"), from the optional cores.conf fields 3-4. */
static char resolved_machine[64];
static char resolved_media[256];

static bool resolve_core(const char *system, char *core, size_t core_size,
			 char *path, size_t path_size)
{
	static const char *const roots[] = { BUNDLED_CORES, DOWNLOADED_CORES };
	char line[512];
	FILE *fp = fopen(CORES_CONF, "r");
	bool found = false;

	if (!fp)
		return false;
	while (!found && fgets(line, sizeof(line), fp)) {
		char *sep;
		char *save = NULL;
		char *name;
		char *extra;
		char list[512];

		trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		sep = strchr(line, '|');
		if (!sep)
			continue;
		*sep = '\0';
		if (strcmp(line, system) != 0)
			continue;
		resolved_machine[0] = '\0';
		resolved_media[0] = '\0';
		extra = strchr(sep + 1, '|');
		if (extra) {
			char *media;

			*extra++ = '\0';
			media = strchr(extra, '|');
			if (media)
				*media++ = '\0';
			copy_text(resolved_machine, sizeof(resolved_machine), extra);
			copy_text(resolved_media, sizeof(resolved_media), media ? media : "");
		}
		for (size_t r = 0; r < 2 && !found; r++) {
			copy_text(list, sizeof(list), sep + 1);
			for (name = strtok_r(list, ",", &save); name && !found;
			     name = strtok_r(NULL, ",", &save)) {
				snprintf(path, path_size, "%s/%s_libretro.so",
					 roots[r], name);
				if (file_exists(path)) {
					copy_text(core, core_size, name);
					found = true;
				}
			}
		}
	}
	fclose(fp);
	return found;
}

/* ------------------------------------------------------------------ */
/* Per-game overrides (EPIC-018)                                       */
/* ------------------------------------------------------------------ */

static bool valid_game(const char *game)
{
	size_t len = strlen(game);

	if (!len || len >= 32)
		return false;
	for (const char *p = game; *p; p++)
		if (!isxdigit((unsigned char)*p))
			return false;
	return true;
}

static void overrides_path(const char *user, const char *game, char *out, size_t size)
{
	snprintf(out, size, USERS_DIR "/%s/" GAME_SETTINGS_SUBDIR "/%s.conf", user, game);
}

static void load_overrides(const char *user, const char *game, struct game_overrides *o)
{
	char path[PATH_MAX];
	char line[256];
	FILE *fp;

	memset(o, 0, sizeof(*o));
	overrides_path(user, game, path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		trim(line);
		if (!(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "core"))
			copy_text(o->core, sizeof(o->core), eq);
		else if (!strcmp(line, "aspect"))
			copy_text(o->aspect, sizeof(o->aspect), eq);
		else if (!strcmp(line, "filter"))
			copy_text(o->filter, sizeof(o->filter), eq);
	}
	fclose(fp);
}

static int save_overrides(const char *user, const char *game, const struct game_overrides *o)
{
	char path[PATH_MAX];
	char tmp[PATH_MAX + 8];
	char dir[PATH_MAX];
	FILE *fp;
	int rc = 0;

	overrides_path(user, game, path, sizeof(path));
	/* Nothing overridden: no file, the game inherits everything. */
	if (!o->core[0] && !o->aspect[0] && !o->filter[0])
		return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
	snprintf(dir, sizeof(dir), USERS_DIR "/%s/" GAME_SETTINGS_SUBDIR, user);
	if (mkdir_p(dir) != 0)
		return -1;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	if (o->core[0])
		fprintf(fp, "core=%s\n", o->core);
	if (o->aspect[0])
		fprintf(fp, "aspect=%s\n", o->aspect);
	if (o->filter[0])
		fprintf(fp, "filter=%s\n", o->filter);
	if (ferror(fp))
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

static bool valid_aspect(const char *v)
{
	return !strcmp(v, "core") || !strcmp(v, "4:3") || !strcmp(v, "16:9") ||
	       !strcmp(v, "full") || !strcmp(v, "integer");
}

static bool valid_filter(const char *v)
{
	return !strcmp(v, "sharp") || !strcmp(v, "smooth") || !strcmp(v, "pixel");
}

/* Cores of a system that can be loaded (bundled or downloaded), in
 * cores.conf preference order, as "a,b,c". */
static void available_cores(const char *system, char *out, size_t size)
{
	static const char *const roots[] = { BUNDLED_CORES, DOWNLOADED_CORES };
	char line[512];
	FILE *fp = fopen(CORES_CONF, "r");

	out[0] = '\0';
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *sep, *end, *save = NULL;

		trim(line);
		if (!line[0] || line[0] == '#' || !(sep = strchr(line, '|')))
			continue;
		*sep++ = '\0';
		if (strcmp(line, system) != 0)
			continue;
		if ((end = strchr(sep, '|')))
			*end = '\0';
		for (char *name = strtok_r(sep, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
			char path[PATH_MAX];
			bool ok = false;

			for (size_t r = 0; r < 2 && !ok; r++) {
				snprintf(path, sizeof(path), "%s/%s_libretro.so", roots[r], name);
				ok = file_exists(path);
			}
			if (ok && strlen(out) + strlen(name) + 2 < size) {
				if (out[0])
					strcat(out, ",");
				strcat(out, name);
			}
		}
		break;
	}
	fclose(fp);
}

static bool list_has(const char *list, const char *name)
{
	size_t len = strlen(name);

	for (const char *p = list; *p;) {
		const char *comma = strchr(p, ',');
		size_t n = comma ? (size_t)(comma - p) : strlen(p);

		if (n == len && !strncmp(p, name, len))
			return true;
		if (!comma)
			break;
		p = comma + 1;
	}
	return false;
}

static void core_file(const char *core, char *out, size_t size)
{
	snprintf(out, size, BUNDLED_CORES "/%s_libretro.so", core);
	if (!file_exists(out))
		snprintf(out, size, DOWNLOADED_CORES "/%s_libretro.so", core);
}

/* ------------------------------------------------------------------ */
/* User emulation settings (Settings -> Gaming)                        */
/* ------------------------------------------------------------------ */

static bool word_in(const char *const *list, const char *word)
{
	for (; *list; list++)
		if (!strcmp(*list, word))
			return true;
	return false;
}

/* "a,b,c" made only of known overlay fields (empty allowed). */
static bool valid_overlay_items(const char *v)
{
	char copy[96];
	char *save = NULL;

	if (strlen(v) >= sizeof(copy))
		return false;
	copy_text(copy, sizeof(copy), v);
	for (char *w = strtok_r(copy, ",", &save); w; w = strtok_r(NULL, ",", &save))
		if (!word_in(overlay_fields, w))
			return false;
	return true;
}

static void default_settings(struct emu_settings *e)
{
	memset(e, 0, sizeof(*e));
	e->slots = DEFAULT_SLOTS;
	e->slot_rotation = true;
	e->crt = true;
	copy_text(e->overlay_items, sizeof(e->overlay_items), DEFAULT_OVERLAY_ITEMS);
	for (int i = 0; i < HK_COUNT; i++)
		copy_text(e->hotkey[i], sizeof(e->hotkey[i]), hotkey_defaults[i]);
}

/* Applies key=value to e; false when the key or value is not valid. */
static bool settings_set(struct emu_settings *e, const char *key, const char *v)
{
	bool flag = !strcmp(v, "1");

	if (!strcmp(key, "boot_screens") || !strcmp(key, "slot_rotation") ||
	    !strcmp(key, "bezels") || !strcmp(key, "crt") || !strcmp(key, "overlay")) {
		if (strcmp(v, "0") && strcmp(v, "1"))
			return false;
		if (!strcmp(key, "boot_screens"))
			e->boot_screens = flag;
		else if (!strcmp(key, "slot_rotation"))
			e->slot_rotation = flag;
		else if (!strcmp(key, "bezels"))
			e->bezels = flag;
		else if (!strcmp(key, "crt"))
			e->crt = flag;
		else
			e->overlay = flag;
		return true;
	}
	if (!strcmp(key, "slots")) {
		char *end;
		long n = strtol(v, &end, 10);

		if (*end || n < 1 || n > MAX_SLOTS)
			return false;
		e->slots = (int)n;
		return true;
	}
	if (!strcmp(key, "overlay_items")) {
		if (!valid_overlay_items(v))
			return false;
		copy_text(e->overlay_items, sizeof(e->overlay_items), v);
		return true;
	}
	if (!strncmp(key, "hotkey_", 7)) {
		for (int i = 0; i < HK_COUNT; i++) {
			if (strcmp(key + 7, hotkey_actions[i]))
				continue;
			if (!word_in(hotkey_controls, v))
				return false;
			copy_text(e->hotkey[i], sizeof(e->hotkey[i]), v);
			return true;
		}
	}
	return false;
}

static void settings_path(const char *user, char *out, size_t size)
{
	snprintf(out, size, USERS_DIR "/%s/" EMU_SETTINGS_FILE, user);
}

static void load_settings(const char *user, struct emu_settings *e)
{
	char path[PATH_MAX];
	char line[256];
	FILE *fp;

	default_settings(e);
	settings_path(user, path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		trim(line);
		if (!(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		(void)settings_set(e, line, eq);
	}
	fclose(fp);
}

static size_t format_settings(const struct emu_settings *e, char *out, size_t size)
{
	size_t len;
	int n = snprintf(out, size,
			 "boot_screens=%d\nslots=%d\nslot_rotation=%d\nbezels=%d\ncrt=%d\n"
			 "overlay=%d\noverlay_items=%s\n",
			 e->boot_screens, e->slots, e->slot_rotation, e->bezels, e->crt,
			 e->overlay, e->overlay_items);

	len = n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
	for (int i = 0; i < HK_COUNT && len < size; i++) {
		n = snprintf(out + len, size - len, "hotkey_%s=%s\n",
			     hotkey_actions[i], e->hotkey[i]);
		if (n > 0)
			len = (size_t)n < size - len ? len + (size_t)n : size - 1;
	}
	return len;
}

static int save_settings(const char *user, const struct emu_settings *e)
{
	char path[PATH_MAX];
	char tmp[PATH_MAX + 8];
	char dir[PATH_MAX];
	char text[1024];
	size_t len = format_settings(e, text, sizeof(text));
	FILE *fp;
	int rc = 0;

	snprintf(dir, sizeof(dir), USERS_DIR "/%s/appdata/nuubos-emulation", user);
	if (mkdir_p(dir) != 0)
		return -1;
	settings_path(user, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	if (fwrite(text, 1, len, fp) != len)
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

/* The chords inputd applies while this session holds the gamepads. */
static void send_hotkeys(void)
{
	char line[512] = "HOTKEYS";
	size_t len = strlen(line);

	if (session.input_fd < 0)
		return;
	for (int i = 0; i < HK_COUNT; i++) {
		int n = snprintf(line + len, sizeof(line) - len, " %s=%s",
				 hotkey_actions[i], session.settings.hotkey[i]);

		if (n > 0 && (size_t)n < sizeof(line) - len)
			len += (size_t)n;
	}
	line[len++] = '\n';
	if (write_all(session.input_fd, line, len) != 0)
		log_msg("emud: hotkeys to inputd failed");
}

/* ------------------------------------------------------------------ */
/* Display: screen type, bezel colour and the session shader           */
/* ------------------------------------------------------------------ */

/* display.conf field (1 = screen type, 2 = HDMI lines) of a system; ""
 * when the system or the field is missing. */
static void display_field(const char *system, int field, char *out, size_t size)
{
	char line[128];
	FILE *fp = fopen(DISPLAY_CONF, "r");

	copy_text(out, size, "");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *sep;

		trim(line);
		if (line[0] == '#' || !(sep = strchr(line, '|')))
			continue;
		*sep++ = '\0';
		if (strcmp(line, system))
			continue;
		for (int i = 1; i < field && sep; i++)
			if ((sep = strchr(sep, '|')))
				sep++;
		if (sep) {
			char *end = strchr(sep, '|');

			if (end)
				*end = '\0';
			copy_text(out, size, sep);
		}
		break;
	}
	fclose(fp);
}

/* display.conf screen type of a system: "crt", "lcd" or "monitor". */
static void system_screen(const char *system, char *out, size_t size)
{
	display_field(system, 1, out, size);
	if (!out[0])
		copy_text(out, size, "monitor");
}

/* Render height of a game of the system on the current output: the system's
 * HDMI lines while HDMI is the active output (the output policy turns the
 * panel off then), else 0 = native. */
static int game_render_height(const char *system)
{
	char value[16] = "";
	FILE *fp = fopen(HDMI_ENABLED, "r");

	if (!fp)
		return 0;
	if (!fgets(value, sizeof(value), fp))
		value[0] = '\0';
	fclose(fp);
	trim(value);
	if (strcmp(value, "enabled") != 0)
		return 0;
	display_field(system, 2, value, sizeof(value));
	return value[0] ? atoi(value) : GAME_HDMI_LINES;
}

/* systems.conf colour (field 6, #rrggbb) as 0..1 components. */
static void system_colour(const char *system, float rgb[3])
{
	char line[512];
	FILE *fp = fopen(SYSTEMS_CONF, "r");

	rgb[0] = 0.25f;
	rgb[1] = 0.45f;
	rgb[2] = 0.90f;
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *field[7] = { line };
		int n = 1;
		unsigned int r, g, b;

		if (line[0] == '#')
			continue;
		for (char *p = line; *p && n < 7; p++)
			if (*p == '|') {
				*p = '\0';
				field[n++] = p + 1;
			}
		if (n < 6 || strcmp(field[0], system))
			continue;
		if (sscanf(field[5], "#%02x%02x%02x", &r, &g, &b) == 3) {
			rgb[0] = (float)r / 255.0f;
			rgb[1] = (float)g / 255.0f;
			rgb[2] = (float)b / 255.0f;
		}
		break;
	}
	fclose(fp);
}

/* Boot sequence: the core options that skip or show the console's own
 * start-up (BIOS logo), set at every launch from the user's setting. */
static const struct {
	const char *key;
	const char *on;
	const char *off;
} boot_options[] = {
	{ "pcsx_rearmed_show_bios_bootlogo", "enabled", "disabled" },
	{ "mgba_skip_bios", "OFF", "ON" },
	{ "gambatte_gb_bootloader", "enabled", "disabled" },
	{ "genesis_plus_gx_bios", "enabled", "disabled" },
	{ "melonds_boot_directly", "disabled", "enabled" },
};

/* Rewrites the boot keys of the user's core options file. */
static void apply_boot_options(const char *path, bool boot)
{
	char tmp[PATH_MAX + 8];
	char line[1024];
	bool done[sizeof(boot_options) / sizeof(boot_options[0])] = { false };
	FILE *in = fopen(path, "r");
	FILE *out;

	if (!in)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return;
	}
	while (fgets(line, sizeof(line), in)) {
		char key[128];
		size_t i;

		for (i = 0; i < sizeof(boot_options) / sizeof(boot_options[0]); i++)
			if (sscanf(line, " %127[^ =#] =", key) == 1 &&
			    !strcmp(key, boot_options[i].key))
				break;
		if (i == sizeof(boot_options) / sizeof(boot_options[0])) {
			fputs(line, out);
			continue;
		}
		if (!done[i])
			fprintf(out, "%s = \"%s\"\n", boot_options[i].key,
				boot ? boot_options[i].on : boot_options[i].off);
		done[i] = true;
	}
	for (size_t i = 0; i < sizeof(boot_options) / sizeof(boot_options[0]); i++)
		if (!done[i])
			fprintf(out, "%s = \"%s\"\n", boot_options[i].key,
				boot ? boot_options[i].on : boot_options[i].off);
	fclose(in);
	if (fclose(out) != 0 || rename(tmp, path) != 0)
		unlink(tmp);
}

/* ------------------------------------------------------------------ */
/* RetroAchievements (EPIC-017)                                        */
/* ------------------------------------------------------------------ */

struct achievements {
	bool enabled;
	bool hardcore;
	char username[64];
	char token[128];
};

static bool cfg_value_safe(const char *v)
{
	return strpbrk(v, "\"\\\r\n") == NULL;
}

static void load_achievements(const char *user, struct achievements *a)
{
	char path[PATH_MAX];
	char line[256];
	FILE *fp;

	memset(a, 0, sizeof(*a));
	snprintf(path, sizeof(path), STATE_USERS "/%s/" ACHIEVEMENTS_FILE, user);
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		trim(line);
		if (!(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "USERNAME"))
			copy_text(a->username, sizeof(a->username), eq);
		else if (!strcmp(line, "TOKEN"))
			copy_text(a->token, sizeof(a->token), eq);
		else if (!strcmp(line, "ENABLED"))
			a->enabled = !strcmp(eq, "1");
		else if (!strcmp(line, "HARDCORE"))
			a->hardcore = !strcmp(eq, "1");
	}
	fclose(fp);
	if (!a->username[0] || !a->token[0] || !cfg_value_safe(a->username) ||
	    !cfg_value_safe(a->token))
		a->enabled = false;
}

/* ------------------------------------------------------------------ */
/* User RetroArch keys restored after a session                         */
/* ------------------------------------------------------------------ */

static void free_restore(void)
{
	for (int i = 0; i < session.restore_count; i++)
		free(session.restore_value[i]);
	session.restore_count = 0;
}

/* Remember the user's value of key before the session overrides it. */
static void remember_key(const char *key)
{
	char line[1024];
	FILE *fp;
	size_t klen = strlen(key);
	int i;

	for (i = 0; i < session.restore_count; i++)
		if (!strcmp(session.restore_key[i], key))
			return;
	if (session.restore_count >= MAX_RESTORE_KEYS)
		return;
	i = session.restore_count++;
	copy_text(session.restore_key[i], sizeof(session.restore_key[i]), key);
	session.restore_value[i] = NULL;
	fp = fopen(session.ra_cfg, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;

		if (strncmp(p, key, klen) != 0)
			continue;
		p += klen;
		while (*p == ' ')
			p++;
		if (*p != '=')
			continue;
		free(session.restore_value[i]);
		session.restore_value[i] = strdup(line);
	}
	fclose(fp);
}

/* Put the remembered keys back into the user's retroarch.cfg. */
static void restore_user_keys(void)
{
	char tmp[PATH_MAX + 8];
	char line[2048];
	bool written[MAX_RESTORE_KEYS] = { false };
	FILE *in, *out;

	if (!session.restore_count || !session.ra_cfg[0])
		return;
	in = fopen(session.ra_cfg, "r");
	if (!in) {
		free_restore();
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", session.ra_cfg);
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		free_restore();
		return;
	}
	while (fgets(line, sizeof(line), in)) {
		int match = -1;

		for (int i = 0; i < session.restore_count && match < 0; i++) {
			size_t klen = strlen(session.restore_key[i]);
			const char *p = line + klen;

			if (strncmp(line, session.restore_key[i], klen) != 0)
				continue;
			while (*p == ' ')
				p++;
			if (*p == '=')
				match = i;
		}
		if (match < 0) {
			fputs(line, out);
		} else if (session.restore_value[match] && !written[match]) {
			fputs(session.restore_value[match], out);
			written[match] = true;
		}
	}
	fclose(in);
	if (fclose(out) == 0 && rename(tmp, session.ra_cfg) == 0)
		log_msg("emud: restored %d user RetroArch keys", session.restore_count);
	else
		unlink(tmp);
	free_restore();
}

/* Shader preset of the session: one pass, its parameters. */
static int write_session_shader(const char *shader, const char *params)
{
	char tmp[] = SESSION_SHADER ".tmp";
	FILE *fp = fopen(tmp, "w");
	int rc;

	if (!fp)
		return -1;
	fprintf(fp, "shaders = \"1\"\nshader0 = \"%s\"\nfilter_linear0 = \"false\"\n%s",
		shader, params);
	rc = ferror(fp) ? -1 : 0;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, SESSION_SHADER) != 0)
		rc = -1;
	return rc;
}

/*
 * Picture presentation (A, B, I, L). Without a per-game override the game
 * gets integer scaling with the core's aspect (a PS1 game is 4:3 whatever
 * its resolution): whole vertical scale, width from the aspect; content
 * that would leave more than 12 % of the screen unused (GBA or PSP on
 * 640x480) is scaled to fit with its aspect instead (stock smart mode),
 * through sharp bilinear so no pixel is uneven. The picture is centred
 * horizontally and at the top of the screen (RetroArch patch 0003); the
 * per-game "integer" aspect is the same picture without bezels. Bezels
 * make the viewport the whole screen and bezel.glsl places the picture
 * the same way; the CRT look applies to TV systems (display.conf) unless
 * the game has its own filter. Returns the --set-shader argument ("" =
 * none).
 */
static const char *write_presentation(FILE *fp, const struct game_overrides *o)
{
	static char shader[PATH_MAX];
	static const char *const keys[] = {
		"aspect_ratio_index", "video_scale_integer",
		"video_scale_integer_scaling", "video_scale_integer_axis",
		"video_viewport_bias_x", "video_viewport_bias_y",
		"video_viewport_bias_portrait_x", "video_viewport_bias_portrait_y",
		"video_shader_enable", "video_smooth", NULL,
	};
	char screen[16];
	bool crt;
	bool bezel = false;
	bool integer = true;
	int index = 22; /* the core's aspect */

	for (int i = 0; keys[i]; i++)
		remember_key(keys[i]);
	system_screen(session.system, screen, sizeof(screen));
	crt = session.settings.crt && !strcmp(screen, "crt") && !o->filter[0];
	if (o->aspect[0] && strcmp(o->aspect, "integer")) {
		index = !strcmp(o->aspect, "4:3") ? 0
		      : !strcmp(o->aspect, "16:9") ? 1
		      : !strcmp(o->aspect, "full") ? 24
		      : 22; /* core */
		integer = false;
	} else if (!strcmp(o->aspect, "integer")) {
		/* the default presentation, without bezels */
	} else if (session.settings.bezels) {
		index = 24;
		integer = false;
		bezel = true;
	}
	fprintf(fp,
		"aspect_ratio_index = \"%d\"\n"
		"video_scale_integer = \"%s\"\n"
		"video_scale_integer_scaling = \"2\"\n"
		"video_scale_integer_axis = \"0\"\n"
		"video_viewport_bias_x = \"0.500000\"\n"
		"video_viewport_bias_y = \"0.000000\"\n"
		"video_viewport_bias_portrait_x = \"0.500000\"\n"
		"video_viewport_bias_portrait_y = \"0.000000\"\n"
		"video_smooth = \"%s\"\n",
		index, integer ? "true" : "false",
		!strcmp(o->filter, "smooth") ? "true" : "false");

	shader[0] = '\0';
	if (bezel) {
		char params[1536];
		char icon[PATH_MAX];
		float rgb[3];
		bool has_icon;

		system_colour(session.system, rgb);
		snprintf(icon, sizeof(icon), SYSTEM_ICONS "/%s.png", session.system);
		has_icon = file_exists(icon);
		/* An unused sampler still needs an image: the wordmark. */
		if (!has_icon)
			copy_text(icon, sizeof(icon), SHADER_DIR "/bezel-wordmark.png");
		snprintf(params, sizeof(params),
			 "textures = \"SYSICON;BRAND\"\n"
			 "SYSICON = \"%s\"\nSYSICON_linear = \"true\"\n"
			 "SYSICON_mipmap = \"true\"\n"
			 "BRAND = \"" SHADER_DIR "/bezel-wordmark.png\"\n"
			 "BRAND_linear = \"false\"\n"
			 "parameters = \"BEZEL_R;BEZEL_G;BEZEL_B;BEZEL_STYLE;HAS_ICON;CRT\"\n"
			 "BEZEL_R = \"%.3f\"\nBEZEL_G = \"%.3f\"\nBEZEL_B = \"%.3f\"\n"
			 "BEZEL_STYLE = \"%d\"\nHAS_ICON = \"%d\"\nCRT = \"%d\"\n",
			 icon, rgb[0], rgb[1], rgb[2], !strcmp(screen, "lcd") ? 1 : 0,
			 has_icon ? 1 : 0, crt ? 1 : 0);
		if (write_session_shader(SHADER_DIR "/bezel.glsl", params) == 0)
			copy_text(shader, sizeof(shader), SESSION_SHADER);
	} else if (crt) {
		if (write_session_shader(SHADER_DIR "/crt.glsl", "") == 0)
			copy_text(shader, sizeof(shader), SESSION_SHADER);
	} else if (!o->filter[0] || !strcmp(o->filter, "sharp")) {
		copy_text(shader, sizeof(shader), DEFAULT_SHADER_PRESET);
	}
	fprintf(fp, "video_shader_enable = \"%s\"\n", shader[0] ? "true" : "false");
	return shader;
}

/* Achievements, appended to the session config. */
static void write_session_overrides(FILE *fp, const struct achievements *a)
{
	/* The token is written for every session of a logged-in user and
	 * removed from the user's file afterwards: USERDATA is exFAT and has
	 * no permissions, the token lives only in STATE. */
	remember_key("cheevos_enable");
	remember_key("cheevos_username");
	remember_key("cheevos_token");
	remember_key("cheevos_password");
	remember_key("cheevos_hardcore_mode_enable");
	if (a->enabled) {
		fprintf(fp,
			"cheevos_enable = \"true\"\n"
			"cheevos_username = \"%s\"\n"
			"cheevos_token = \"%s\"\n"
			"cheevos_password = \"\"\n"
			"cheevos_hardcore_mode_enable = \"%s\"\n",
			a->username, a->token, a->hardcore ? "true" : "false");
	} else {
		fprintf(fp, "cheevos_enable = \"false\"\n"
			    "cheevos_token = \"\"\n"
			    "cheevos_password = \"\"\n");
	}
}

/* ------------------------------------------------------------------ */
/* RetroArch session                                                   */
/* ------------------------------------------------------------------ */

static void ra_command(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	int n;

	if (session.stdin_fd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((size_t)n > sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	if (write(session.stdin_fd, line, (size_t)n) != n)
		log_msg("emud: command to RetroArch failed: %s", strerror(errno));
}

/* RetroArch's state file names: <base>.state, .stateN, .state.auto. */
static void state_path(const char *base, int slot, char *out, size_t size)
{
	if (slot == AUTO_SLOT)
		snprintf(out, size, "%s.state.auto", base);
	else if (slot == 0)
		snprintf(out, size, "%s.state", base);
	else
		snprintf(out, size, "%s.state%d", base, slot);
}

static void state_file_name(int slot, char *out, size_t size)
{
	state_path(session.state_base, slot, out, size);
}

/* Modification time of a state, -1 when there is none. */
static long long state_mtime(const char *base, int slot)
{
	char path[PATH_MAX];
	struct stat st;

	state_path(base, slot, path, sizeof(path));
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return -1;
	return (long long)st.st_mtime;
}

/* The most recent state (a slot or AUTO_SLOT), NO_STATE when none; the
 * most recent numbered slot in *manual (0 when none). */
#define NO_STATE -2
static int newest_state(const char *base, int slots, int *manual)
{
	long long best = -1;
	long long best_manual = -1;
	int newest = NO_STATE;

	*manual = 0;
	for (int slot = AUTO_SLOT; slot < slots; slot++) {
		long long t = state_mtime(base, slot);

		if (t < 0)
			continue;
		if (t > best) {
			best = t;
			newest = slot;
		}
		if (slot != AUTO_SLOT && t > best_manual) {
			best_manual = t;
			*manual = slot;
		}
	}
	return newest;
}

/* Where the next save goes: the selected slot, or with rotation the slot
 * after it once it holds a state (wrapping to 0 after the last). */
static int save_target(void)
{
	if (!session.settings.slot_rotation || state_mtime(session.state_base, session.slot) < 0)
		return session.slot;
	return (session.slot + 1) % session.settings.slots;
}

/* Saves into save_target() and selects that slot. */
static bool begin_save(int slot, bool quiet);
static bool save_next(void)
{
	int target = save_target();

	if (!begin_save(target, false))
		return false;
	session.slot = target;
	return true;
}

static void stop_watch(void)
{
	if (session.inotify_fd >= 0)
		close(session.inotify_fd);
	session.inotify_fd = -1;
	session.inotify_wd = -1;
}

static void finish_power_wait(void);
static void finish_screenshot(bool ok);

static void finish_pending(bool ok)
{
	enum pending_kind kind = session.pending;
	int slot = session.pending_slot;
	bool quiet = session.pending_quiet;

	session.pending = PENDING_NONE;
	stop_watch();
	if (!quiet) {
		if (kind == PENDING_SAVE)
			notify_state(ok ? "game.state.saved" : "game.state.failed",
				     slot, ok ? NULL : "save");
		else if (kind == PENDING_LOAD)
			notify_state(ok ? "game.state.loaded" : "game.state.failed",
				     slot, ok ? NULL : "load");
	}
	if (kind == PENDING_SAVE)
		log_msg("emud: save slot %d %s", slot, ok ? "written" : "failed");
	finish_power_wait();
	notify_subscribers();
}

static bool begin_save(int slot, bool quiet)
{
	char path[PATH_MAX];

	if (session.state != SESSION_RUNNING || session.pending != PENDING_NONE)
		return false;
	state_file_name(slot, path, sizeof(path));
	session.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (session.inotify_fd >= 0)
		session.inotify_wd = inotify_add_watch(session.inotify_fd,
						       session.states_dir,
						       IN_CLOSE_WRITE | IN_MOVED_TO);
	if (session.inotify_fd < 0 || session.inotify_wd < 0) {
		stop_watch();
		return false;
	}
	copy_text(session.pending_name, sizeof(session.pending_name),
		  strrchr(path, '/') + 1);
	session.pending = PENDING_SAVE;
	session.pending_slot = slot;
	session.pending_quiet = quiet;
	session.pending_replied = false;
	session.pending_deadline_ms = monotonic_ms() + SAVE_TIMEOUT_MS;
	ra_command("SAVE_STATE_SLOT %d", slot);
	notify_subscribers();
	return true;
}

static void begin_load(int slot)
{
	char path[PATH_MAX];

	state_file_name(slot, path, sizeof(path));
	if (!file_exists(path)) {
		notify_state("game.state.empty", slot, NULL);
		return;
	}
	session.pending = PENDING_LOAD;
	session.pending_slot = slot;
	session.pending_quiet = false;
	session.pending_replied = false;
	session.pending_deadline_ms = monotonic_ms() + SAVE_TIMEOUT_MS;
	ra_command("LOAD_STATE_SLOT %d", slot);
	notify_subscribers();
}

static void handle_ra_reply(char *line)
{
	int slot;
	char result[16];

	if (sscanf(line, "SAVE_STATE_SLOT %d %15s", &slot, result) == 2) {
		if (session.pending != PENDING_SAVE || slot != session.pending_slot)
			return;
		if (strcmp(result, "OK") != 0)
			finish_pending(false);
		else
			session.pending_replied = true;
		return;
	}
	if (sscanf(line, "LOAD_STATE_SLOT %d %15s", &slot, result) == 2) {
		if (session.pending == PENDING_LOAD && slot == session.pending_slot)
			finish_pending(strcmp(result, "OK") == 0);
		return;
	}
	if (!strncmp(line, "GET_FPS ", 8)) {
		session.fps = atof(line + 8);
		notify_subscribers();
		return;
	}
	if (sscanf(line, "SET_PAUSED %d", &slot) == 1) {
		bool paused = slot != 0;

		if (paused != session.paused) {
			session.paused = paused;
			notify_subscribers();
		}
	}
}

static void process_ra_output(void)
{
	char buf[512];
	ssize_t n = read(session.stdout_fd, buf, sizeof(buf));

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close(session.stdout_fd);
		session.stdout_fd = -1;
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			session.out_buf[session.out_used] = '\0';
			handle_ra_reply(session.out_buf);
			session.out_used = 0;
		} else if (session.out_used + 1 < sizeof(session.out_buf)) {
			session.out_buf[session.out_used++] = buf[i];
		} else {
			session.out_used = 0;
		}
	}
}

static void process_watch(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	bool done = false;

	while ((n = read(session.inotify_fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;

			if (ev->len && strcmp(ev->name, session.pending_name) == 0)
				done = true;
			p += sizeof(*ev) + ev->len;
		}
	}
	if (done && session.pending == PENDING_SAVE)
		finish_pending(true);
}

/* Writes the settings nuubOS enforces at every launch; RetroArch appends
 * them over the user's configuration (--appendconfig). */
static int write_session_cfg(const char *ra_dir, const char *saves,
			     const char *states, const char *screenshots,
			     const struct game_overrides *o, const struct achievements *a,
			     bool auto_load, const char **shader)
{
	char tmp[] = SESSION_CFG ".tmp";
	FILE *fp;
	int player;
	int rc;

	if (mkdir_p(SESSION_DIR) != 0)
		return -1;
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp,
		"# Written by nuubos-emud for the running session.\n"
		"libretro_directory = \"" DOWNLOADED_CORES "\"\n"
		"libretro_info_path = \"" CORE_INFO_DIR "\"\n"
		"assets_directory = \"" RA_ASSETS_DIR "\"\n"
		"joypad_autoconfig_dir = \"" RA_AUTOCONFIG_DIR "\"\n"
		"system_directory = \"" BIOS_DIR "\"\n"
		"cheat_database_path = \"" CHEATS_DIR "\"\n"
		"cache_directory = \"" CACHE_DIR "\"\n"
		"video_shader_dir = \"" SHADER_DIR "\"\n"
		"savefile_directory = \"%s\"\n"
		"savestate_directory = \"%s\"\n"
		"screenshot_directory = \"%s\"\n"
		"sort_savefiles_enable = \"false\"\n"
		"sort_savestates_enable = \"false\"\n"
		"sort_savefiles_by_content_enable = \"false\"\n"
		"sort_savestates_by_content_enable = \"false\"\n"
		"savefiles_in_content_dir = \"false\"\n"
		"savestates_in_content_dir = \"false\"\n"
		"systemfiles_in_content_dir = \"false\"\n"
		"global_core_options = \"true\"\n"
		"core_options_path = \"%s/retroarch-core-options.cfg\"\n"
		"rgui_config_directory = \"%s/config\"\n"
		"input_remapping_directory = \"%s/config/remaps\"\n"
		"playlist_directory = \"%s/playlists\"\n"
		"content_database_path = \"%s/database\"\n"
		"thumbnails_directory = \"%s/thumbnails\"\n"
		"log_dir = \"%s/logs\"\n"
		"video_driver = \"gl\"\n"
		"video_fullscreen = \"true\"\n"
		"input_driver = \"wayland\"\n"
		"input_joypad_driver = \"udev\"\n"
		"input_autodetect_enable = \"true\"\n"
		"stdin_cmd_enable = \"true\"\n"
		"network_cmd_enable = \"false\"\n"
		"pause_nonactive = \"false\"\n"
		"quit_on_close_content = \"1\"\n"
		"input_menu_toggle_gamepad_combo = \"0\"\n"
		"input_quit_gamepad_combo = \"0\"\n"
		"quit_press_twice = \"false\"\n"
		"menu_swap_ok_cancel_buttons = \"false\"\n"
		"config_save_on_exit = \"true\"\n"
		"user_language = \"%d\"\n",
		saves, states, screenshots, ra_dir, ra_dir, ra_dir, ra_dir,
		ra_dir, ra_dir, ra_dir, retroarch_language(session.user));
	/* Port N is reserved for nuubOS game gamepad N (product id N), so
	 * player order always follows nuubOS player assignment. */
	for (player = 1; player <= MAX_PLAYERS; player++)
		fprintf(fp,
			"input_player%d_device_reservation_type = \"2\"\n"
			"input_player%d_reserved_device = \"0000:%04x\"\n",
			player, player, player);
	*shader = write_presentation(fp, o);
	/* The performance overlay line is drawn into the frame with the OSD
	 * font (RetroArch notifications use widgets, not this font). */
	remember_key("video_font_enable");
	remember_key("video_font_path");
	remember_key("video_font_size");
	fprintf(fp, "video_font_enable = \"true\"\nvideo_font_path = \"%s\"\n"
		"video_font_size = \"32.000000\"\n", OSD_FONT);
	/* Progress is kept when the game closes (.state.auto); auto_load
	 * resumes from it when it is the most recent state. */
	remember_key("savestate_auto_save");
	remember_key("savestate_auto_load");
	fprintf(fp, "savestate_auto_save = \"true\"\nsavestate_auto_load = \"%s\"\n",
		auto_load ? "true" : "false");
	write_session_overrides(fp, a);
	rc = ferror(fp) ? -1 : 0;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, SESSION_CFG) != 0)
		rc = -1;
	return rc;
}

/*
 * MAME computers/consoles: the core starts "<machine> <media switch> file"
 * from a .cmd file. It is named after the game so states and saves stay
 * per game. The switch is picked by extension from the system's media map
 * ("dsk:-flop1,cas:-cass"); the first entry is the default.
 */
static int write_mame_cmd(const char *content, char *cmd_path, size_t size)
{
	char base[NAME_MAX + 1];
	char media[256];
	char chosen[32] = "";
	const char *ext = strrchr(content, '.');
	char *save = NULL;
	char *item;
	FILE *fp;

	content_base_name(content, base, sizeof(base));
	copy_text(media, sizeof(media), resolved_media);
	for (item = strtok_r(media, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(item, ':');

		if (!colon)
			continue;
		*colon = '\0';
		if (!chosen[0])
			copy_text(chosen, sizeof(chosen), colon + 1);
		if (ext && !strcasecmp(ext + 1, item)) {
			copy_text(chosen, sizeof(chosen), colon + 1);
			break;
		}
	}
	if (!chosen[0] || strchr(content, '"'))
		return -1;
	snprintf(cmd_path, size, SESSION_DIR "/%s.cmd", base);
	fp = fopen(cmd_path, "w");
	if (!fp)
		return -1;
	fprintf(fp, "%s %s \"%s\"\n", resolved_machine, chosen, content);
	return fclose(fp) == 0 ? 0 : -1;
}

static void content_base_name(const char *content, char *out, size_t size)
{
	const char *base = strrchr(content, '/');
	char *dot;

	copy_text(out, size, base ? base + 1 : content);
	dot = strrchr(out, '.');
	if (dot && dot != out)
		*dot = '\0';
}

static void reset_session(void)
{
	stop_watch();
	if (session.shot_fd > 0)
		close(session.shot_fd);
	free_restore();
	if (session.stdin_fd >= 0)
		close(session.stdin_fd);
	if (session.stdout_fd >= 0)
		close(session.stdout_fd);
	if (session.input_fd >= 0)
		close(session.input_fd);
	memset(&session, 0, sizeof(session));
	session.state = SESSION_IDLE;
	session.pid = -1;
	session.stdin_fd = -1;
	session.stdout_fd = -1;
	session.input_fd = -1;
	session.inotify_fd = -1;
	session.inotify_wd = -1;
	session.power_client = -1;
	session.shot_fd = -1;
}

/*
 * mode: "resume" (default) starts from the most recent state (a slot or the
 * automatic state written when the game last closed), "new" from the
 * beginning, "slot:N" from slot N ("slot:-1" = the automatic state).
 */
static const char *launch(const char *game, const char *mode)
{
	char answer[PATH_MAX + 128];
	char command[64];
	char core_path[PATH_MAX];
	char base[160];
	char ra_dir[256];
	char saves[PATH_MAX];
	char screenshots[PATH_MAX];
	char path[PATH_MAX];
	char *tab;
	int in_pipe[2];
	int out_pipe[2];
	pid_t pid;
	struct game_overrides overrides;
	struct achievements achievements;
	const char *shader = "";
	char entry_arg[16] = "";
	bool auto_load = false;
	int manual = 0;
	int start = NO_STATE;
	int render_height;

	if (session.state != SESSION_IDLE)
		return "busy";
	if (!mode || !mode[0])
		mode = "resume";
	if (strcmp(mode, "resume") && strcmp(mode, "new") && strncmp(mode, "slot:", 5))
		return "mode";
	for (const char *p = game; *p; p++)
		if (!isxdigit((unsigned char)*p))
			return "game";
	if (!game[0] || strlen(game) >= sizeof(session.game))
		return "game";
	if (!read_active_user(session.user, sizeof(session.user)))
		return "no-user";
	load_settings(session.user, &session.settings);

	snprintf(command, sizeof(command), "RESOLVE\t%s", game);
	if (library_request(command, answer, sizeof(answer)) != 0)
		return "library";
	if (strncmp(answer, "ERR ", 4) == 0)
		return strcmp(answer + 4, "unavailable") == 0 ? "unavailable" : "game";
	if (strncmp(answer, "OK ", 3) != 0 || !(tab = strchr(answer + 3, '\t')))
		return "library";
	*tab = '\0';
	copy_text(session.game, sizeof(session.game), game);
	copy_text(session.system, sizeof(session.system), answer + 3);
	copy_text(session.content, sizeof(session.content), tab + 1);
	if (!file_exists(session.content))
		return "unavailable";
	if (!resolve_core(session.system, session.core, sizeof(session.core),
			  core_path, sizeof(core_path)))
		return "no-core";
	load_overrides(session.user, game, &overrides);
	load_achievements(session.user, &achievements);
	if (overrides.core[0] && strcmp(overrides.core, session.core)) {
		char cores[512];

		/* An override naming a core that is gone falls back to the
		 * system default instead of failing the launch. */
		available_cores(session.system, cores, sizeof(cores));
		if (list_has(cores, overrides.core)) {
			copy_text(session.core, sizeof(session.core), overrides.core);
			core_file(session.core, core_path, sizeof(core_path));
		} else {
			log_msg("emud: core override %s unavailable", overrides.core);
		}
	}
	if (!strcmp(session.core, "mame") && resolved_machine[0]) {
		char cmd[PATH_MAX];

		if (mkdir_p(SESSION_DIR) != 0 ||
		    write_mame_cmd(session.content, cmd, sizeof(cmd)) != 0)
			return "storage";
		copy_text(session.content, sizeof(session.content), cmd);
	}

	/* Per-user configuration and progress; device-global cores, BIOS
	 * and cache (EPIC-003 ownership model). */
	snprintf(base, sizeof(base), USERS_DIR "/%s", session.user);
	snprintf(ra_dir, sizeof(ra_dir), "%s/appdata/retroarch", base);
	snprintf(saves, sizeof(saves), "%s/saves/%s", base, session.system);
	snprintf(session.states_dir, sizeof(session.states_dir), "%s/states/%s",
		 base, session.system);
	snprintf(screenshots, sizeof(screenshots), "%s/screenshots", base);
	snprintf(path, sizeof(path), "%s/config/remaps", ra_dir);
	if (mkdir_p(path) != 0 || mkdir_p(saves) != 0 ||
	    mkdir_p(session.states_dir) != 0 || mkdir_p(screenshots) != 0 ||
	    mkdir_p(DOWNLOADED_CORES) != 0 || mkdir_p(CACHE_DIR) != 0 ||
	    mkdir_p(BIOS_DIR) != 0)
		return "storage";
	/* Once per service start: a new image brings new support files. */
	{
		static bool system_seeded;

		if (!system_seeded) {
			seed_tree(SYSTEM_FILES, BIOS_DIR);
			system_seeded = true;
		}
	}
	snprintf(path, sizeof(path), "%s/retroarch.cfg", ra_dir);
	if (seed_file(DEFAULT_RA_CFG, path) != 0)
		return "storage";
	copy_text(session.ra_cfg, sizeof(session.ra_cfg), path);
	copy_text(session.screenshots_dir, sizeof(session.screenshots_dir), screenshots);
	snprintf(path, sizeof(path), "%s/retroarch-core-options.cfg", ra_dir);
	if (seed_file(DEFAULT_CORE_OPTIONS, path) == 0)
		merge_missing_keys(DEFAULT_CORE_OPTIONS, path);
	apply_boot_options(path, session.settings.boot_screens);
	snprintf(path, sizeof(path), "%s/config/global.glslp", ra_dir);
	(void)seed_file(DEFAULT_SHADER_PRESET, path);
	content_base_name(session.content, path, sizeof(path));
	snprintf(session.state_base, sizeof(session.state_base), "%s/%s",
		 session.states_dir, path);

	/* Starting point: RetroArch loads an entry slot (--entryslot) or the
	 * automatic state (savestate_auto_load). */
	if (!strcmp(mode, "resume")) {
		start = newest_state(session.state_base, session.settings.slots, &manual);
	} else if (!strncmp(mode, "slot:", 5)) {
		start = atoi(mode + 5);
		if (start < AUTO_SLOT || start >= session.settings.slots ||
		    state_mtime(session.state_base, start) < 0)
			return "state";
		(void)newest_state(session.state_base, session.settings.slots, &manual);
	}
	if (start == AUTO_SLOT)
		auto_load = true;
	else if (start >= 0)
		snprintf(entry_arg, sizeof(entry_arg), "%d", start);
	if (write_session_cfg(ra_dir, saves, session.states_dir, screenshots,
			      &overrides, &achievements, auto_load, &shader) != 0)
		return "storage";

	session.input_fd = input_gamepads_on();
	if (session.input_fd < 0)
		log_msg("emud: inputd game gamepads unavailable");
	send_hotkeys();

	if (pipe2(in_pipe, O_CLOEXEC) != 0)
		return "spawn";
	if (pipe2(out_pipe, O_CLOEXEC) != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		return "spawn";
	}

	render_height = game_render_height(session.system);
	pid = fork();
	if (pid < 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		return "spawn";
	}
	if (pid == 0) {
		char cfg[PATH_MAX];
		char *argv[16] = {
			(char *)RETROARCH_BIN, (char *)"--config", cfg,
			(char *)"--appendconfig", (char *)SESSION_CFG,
			/* "" disables RetroArch's automatic shader presets. */
			(char *)"--set-shader", (char *)shader,
		};
		int argc = 7;

		if (entry_arg[0]) {
			argv[argc++] = (char *)"--entryslot";
			argv[argc++] = entry_arg;
		}
		argv[argc++] = (char *)"-L";
		argv[argc++] = core_path;
		argv[argc++] = session.content;
		argv[argc] = NULL;
		int log_fd;
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		dup2(in_pipe[0], STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		log_fd = open(RETROARCH_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (log_fd >= 0)
			dup2(log_fd, STDERR_FILENO);
		clearenv();
		setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
		setenv("HOME", ra_dir, 1);
		/* RetroArch's own default directories ($XDG_CONFIG_HOME/
		 * retroarch) are the user's RetroArch directory. */
		snprintf(cfg, sizeof(cfg), "%s/appdata", base);
		setenv("XDG_CONFIG_HOME", cfg, 1);
		snprintf(cfg, sizeof(cfg), "%s/retroarch.cfg", ra_dir);
		setenv("XDG_RUNTIME_DIR", WAYLAND_RUNTIME, 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		setenv("PIPEWIRE_RUNTIME_DIR", PIPEWIRE_RUNTIME, 1);
		setenv("LANG", "C.UTF-8", 1);
		if (render_height > 0) {
			char lines[16];

			snprintf(lines, sizeof(lines), "%d", render_height);
			setenv("NUUBOS_RA_RENDER_HEIGHT", lines, 1);
		}
		execv(RETROARCH_BIN, argv);
		_exit(127);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);
	session.pid = pid;
	session.stdin_fd = in_pipe[1];
	session.stdout_fd = out_pipe[0];
	(void)fcntl(session.stdin_fd, F_SETFL, O_NONBLOCK);
	(void)fcntl(session.stdout_fd, F_SETFL, O_NONBLOCK);
	session.state = SESSION_RUNNING;
	/* Load State and the slot shown follow the state the game started
	 * from, else the most recent numbered slot. */
	session.slot = start >= 0 ? start : manual;
	session.paused = false;
	session.fps = 0;
	session.overlay_tick_ms = monotonic_ms() + OVERLAY_TICK_MS;
	session.fast_forward = false;
	session.started_ms = monotonic_ms();
	log_msg("emud: launch game=%s system=%s core=%s pid=%d user=%s start=%d shader=%s render=%d",
		session.game, session.system, session.core, (int)pid,
		session.user, start, shader[0] ? shader : "none", render_height);
	library_session("SESSION_BEGIN", session.game);
	notify_subscribers();
	return NULL;
}

static void request_quit(void)
{
	if (session.state != SESSION_RUNNING)
		return;
	session.state = SESSION_EXITING;
	session.quit_deadline_ms = monotonic_ms() + QUIT_TIMEOUT_MS;
	/* Unpause first: a paused RetroArch still reads commands, but the
	 * quit must also flush SRAM through the normal content close. */
	ra_command("SET_PAUSED 0");
	ra_command("QUIT");
	notify_subscribers();
}

/* A PRE_POWER hook waits for the automatic state (and, before
 * restart/poweroff, for RetroArch to exit and write its save files). */
static void finish_power_wait(void)
{
	struct client *c;

	if (session.power_client < 0)
		return;
	if (session.pending != PENDING_NONE)
		return;
	if (strcmp(session.power_action, "sleep") != 0) {
		if (session.state == SESSION_RUNNING)
			request_quit();
		if (session.state != SESSION_IDLE)
			return;
	}
	c = &clients[session.power_client];
	session.power_client = -1;
	reply(c, "OK\n");
}

static void child_exited(int status)
{
	long long ran = monotonic_ms() - session.started_ms;
	bool failed = (WIFEXITED(status) && WEXITSTATUS(status) != 0) ||
		      WIFSIGNALED(status);
	int power_client = session.power_client;

	log_msg("emud: retroarch exited pid=%d status=%d after %lld ms",
		(int)session.pid, status, ran);
	if (session.pending != PENDING_NONE && !session.pending_quiet)
		notify_state("game.state.failed", session.pending_slot,
			     session.pending == PENDING_SAVE ? "save" : "load");
	library_session("SESSION_END", session.game);
	if (failed && session.state == SESSION_RUNNING)
		notify_failed(ran < START_FAILURE_MS ? "start" : "crash");
	if (session.shot_fd > 0)
		notify_screenshot(false);
	restore_user_keys();
	reset_session();
	notify_subscribers();
	if (power_client >= 0 && clients[power_client].fd >= 0)
		reply(&clients[power_client], "OK\n");
}

static void reap_children(void)
{
	char drain[32];
	int status;
	pid_t pid;

	while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
		;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == session.pid)
			child_exited(status);
	}
}

static int session_timeout_ms(void)
{
	long long now = monotonic_ms();
	long long deadline = -1;

	if (session.pending != PENDING_NONE)
		deadline = session.pending_deadline_ms;
	if (session.state == SESSION_EXITING &&
	    (deadline < 0 || session.quit_deadline_ms < deadline))
		deadline = session.quit_deadline_ms;
	if (session.shot_fd > 0 && (deadline < 0 || session.shot_deadline_ms < deadline))
		deadline = session.shot_deadline_ms;
	if (session.state == SESSION_RUNNING && session.settings.overlay &&
	    (deadline < 0 || session.overlay_tick_ms < deadline))
		deadline = session.overlay_tick_ms;
	if (deadline < 0)
		return -1;
	if (deadline <= now)
		return 0;
	return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}

static void session_timers(void)
{
	long long now = monotonic_ms();

	if (session.shot_fd > 0 && now >= session.shot_deadline_ms)
		finish_screenshot(false);
	/* Performance overlay: ask RetroArch for its frame rate; the reply
	 * updates the snapshot the Quick Menu draws from. Not while paused
	 * (the Quick Menu hides the overlay then). */
	if (session.state == SESSION_RUNNING && session.settings.overlay &&
	    now >= session.overlay_tick_ms) {
		if (!session.paused)
			ra_command("GET_FPS");
		session.overlay_tick_ms = now + OVERLAY_TICK_MS;
	}
	if (session.pending != PENDING_NONE && now >= session.pending_deadline_ms) {
		log_msg("emud: state operation slot %d timed out", session.pending_slot);
		finish_pending(false);
	}
	if (session.state == SESSION_EXITING && now >= session.quit_deadline_ms) {
		if (!session.term_sent) {
			log_msg("emud: retroarch did not quit, SIGTERM");
			kill(session.pid, SIGTERM);
			session.term_sent = true;
			session.quit_deadline_ms = now + KILL_TIMEOUT_MS;
		} else if (!session.kill_sent) {
			log_msg("emud: retroarch did not terminate, SIGKILL");
			kill(-session.pid, SIGKILL);
			session.kill_sent = true;
			session.quit_deadline_ms = now + KILL_TIMEOUT_MS;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Screenshots (EPIC-021)                                              */
/* ------------------------------------------------------------------ */

static void finish_screenshot(bool ok)
{
	if (session.shot_fd > 0)
		close(session.shot_fd);
	session.shot_fd = -1;
	notify_screenshot(ok);
	log_msg("emud: screenshot %s", ok ? "saved" : "failed");
}

/* RetroArch writes the PNG of its own rendering (never the composited
 * screen, so the Quick Menu is not in it); success is reported only once a
 * new PNG is closed in the user's screenshot folder. */
static bool begin_screenshot(void)
{
	if (session.state != SESSION_RUNNING || session.shot_fd > 0)
		return false;
	session.shot_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (session.shot_fd < 0 ||
	    inotify_add_watch(session.shot_fd, session.screenshots_dir,
			      IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
		if (session.shot_fd >= 0)
			close(session.shot_fd);
		session.shot_fd = -1;
		notify_screenshot(false);
		return false;
	}
	session.shot_deadline_ms = monotonic_ms() + SCREENSHOT_TIMEOUT_MS;
	ra_command("SCREENSHOT");
	return true;
}

static void process_shot_watch(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	bool done = false;

	while ((n = read(session.shot_fd, buf, sizeof(buf))) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;
			size_t len = ev->len ? strlen(ev->name) : 0;

			if (len > 4 && !strcasecmp(ev->name + len - 4, ".png"))
				done = true;
			p += sizeof(*ev) + ev->len;
		}
	}
	if (done)
		finish_screenshot(true);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int step_slot(int slot, int delta)
{
	int last = session.settings.slots - 1;

	slot += delta;
	if (slot > last)
		return 0;
	if (slot < 0)
		return last;
	return slot;
}

/* Controller hotkeys from nuubos-inputd (Quick Menu button + chord): the
 * same session actions as the Quick Menu, with the outcome as an OSD
 * notification. */
static void handle_hotkey(const char *action)
{
	if (session.state != SESSION_RUNNING)
		return;
	log_msg("emud: hotkey %s", action);
	if (!strcmp(action, "save_state")) {
		if (save_next())
			notify_subscribers();
	} else if (!strcmp(action, "quit")) {
		request_quit();
	} else if (!strcmp(action, "load_state")) {
		if (session.pending == PENDING_NONE)
			begin_load(session.slot);
	} else if (!strcmp(action, "slot_next") || !strcmp(action, "slot_prev")) {
		session.slot = step_slot(session.slot,
					 !strcmp(action, "slot_next") ? 1 : -1);
		notify_slot(session.slot);
		notify_subscribers();
	} else if (!strcmp(action, "screenshot")) {
		(void)begin_screenshot();
	} else if (!strcmp(action, "fast_forward")) {
		session.fast_forward = !session.fast_forward;
		ra_command("FAST_FORWARD");
		notify_fast_forward(session.fast_forward);
		notify_subscribers();
	}
}

static void process_input_link(void)
{
	char buf[128];
	ssize_t n = read(session.input_fd, buf, sizeof(buf));

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		/* inputd restarted: take the gamepads again. */
		close(session.input_fd);
		session.input_used = 0;
		session.input_fd = input_gamepads_on();
		send_hotkeys();
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			session.input_buf[session.input_used] = '\0';
			if (!strncmp(session.input_buf, "HOTKEY ", 7))
				handle_hotkey(session.input_buf + 7);
			session.input_used = 0;
		} else if (session.input_used + 1 < sizeof(session.input_buf)) {
			session.input_buf[session.input_used++] = buf[i];
		} else {
			session.input_used = 0;
		}
	}
}

static bool running(struct client *c)
{
	if (session.state == SESSION_RUNNING)
		return true;
	reply(c, "ERR no-session\n");
	return false;
}

/* GAME_SETTINGS <game> <system> | GAME_SET <game> <system> <key> <value> |
 * GAME_RESET <game>, for the active user. */
static void handle_game_settings(struct client *c, const char *verb, char *arg)
{
	char *fields[4] = { arg, NULL, NULL, NULL };
	char user[64];
	char cores[512];
	char def_core[64];
	char def_path[PATH_MAX];
	struct game_overrides o;
	int n = 1;

	for (char *p = arg; *p && n < 4; p++)
		if (*p == '\t') {
			*p = '\0';
			fields[n++] = p + 1;
		}
	if (!valid_game(fields[0])) {
		reply(c, "ERR game\n");
		return;
	}
	if (!read_active_user(user, sizeof(user))) {
		reply(c, "ERR no-user\n");
		return;
	}
	load_overrides(user, fields[0], &o);
	if (!strcmp(verb, "GAME_RESET")) {
		memset(&o, 0, sizeof(o));
		reply(c, save_overrides(user, fields[0], &o) == 0 ? "OK\n" : "ERR storage\n");
		return;
	}
	if (!fields[1] || !fields[1][0] || strchr(fields[1], '/')) {
		reply(c, "ERR system\n");
		return;
	}
	available_cores(fields[1], cores, sizeof(cores));
	if (!strcmp(verb, "GAME_SETTINGS")) {
		char out[1024];
		const char *effective;
		struct emu_settings settings;
		char screen[16];
		int len;

		/* filter_default: what an empty filter gives (write_presentation) */
		load_settings(user, &settings);
		system_screen(fields[1], screen, sizeof(screen));
		def_core[0] = '\0';
		(void)resolve_core(fields[1], def_core, sizeof(def_core), def_path, sizeof(def_path));
		effective = o.core[0] && list_has(cores, o.core) ? o.core : def_core;
		len = snprintf(out, sizeof(out),
			       "core=%s\ncore_default=%s\ncore_override=%s\ncores=%s\n"
			       "aspect=%s\nfilter=%s\nfilter_default=%s\nend=1\n",
			       effective, def_core, o.core, cores, o.aspect, o.filter,
			       settings.crt && !strcmp(screen, "crt") ? "crt" : "sharp");
		if (len > 0 && write_all(c->fd, out, (size_t)len) != 0)
			close_client(c);
		return;
	}
	/* GAME_SET */
	if (!fields[2] || !fields[3]) {
		reply(c, "ERR args\n");
		return;
	}
	if (!strcmp(fields[2], "core") && (!fields[3][0] || list_has(cores, fields[3])))
		copy_text(o.core, sizeof(o.core), fields[3]);
	else if (!strcmp(fields[2], "aspect") && (!fields[3][0] || valid_aspect(fields[3])))
		copy_text(o.aspect, sizeof(o.aspect), fields[3]);
	else if (!strcmp(fields[2], "filter") && (!fields[3][0] || valid_filter(fields[3])))
		copy_text(o.filter, sizeof(o.filter), fields[3]);
	else {
		reply(c, "ERR value\n");
		return;
	}
	reply(c, save_overrides(user, fields[0], &o) == 0 ? "OK\n" : "ERR storage\n");
}

/* SETTINGS (key, value NULL) lists the active user's settings; SET key
 * value changes one. Hotkeys and the overlay follow at once in a running
 * game of that user; the rest applies at the next launch. */
static void handle_settings(struct client *c, const char *key, const char *value)
{
	char user[64];
	char out[1100];
	struct emu_settings e;
	size_t len;

	if (!read_active_user(user, sizeof(user))) {
		reply(c, "ERR no-user\n");
		return;
	}
	load_settings(user, &e);
	if (!key) {
		len = format_settings(&e, out, sizeof(out) - 8);
		memcpy(out + len, "end=1\n", 7);
		if (write_all(c->fd, out, len + 6) != 0)
			close_client(c);
		return;
	}
	if (!settings_set(&e, key, value)) {
		reply(c, "ERR value\n");
		return;
	}
	if (save_settings(user, &e) != 0) {
		reply(c, "ERR storage\n");
		return;
	}
	reply(c, "OK\n");
	if (session.state != SESSION_RUNNING || strcmp(user, session.user))
		return;
	memcpy(session.settings.hotkey, e.hotkey, sizeof(e.hotkey));
	session.settings.overlay = e.overlay;
	copy_text(session.settings.overlay_items, sizeof(session.settings.overlay_items),
		  e.overlay_items);
	if (!strncmp(key, "hotkey_", 7))
		send_hotkeys();
	session.overlay_tick_ms = monotonic_ms();
	notify_subscribers();
}

/* STATES [game]: the save states of the running game, or of a game before
 * it starts (Home's start menu): "slots=N", then "state=<slot>\t<mtime>\t
 * <HH:MM local>" for each existing state (slot -1 = the automatic state),
 * "end=1". */
static void handle_states(struct client *c, const char *game)
{
	char base[PATH_MAX];
	char out[4096];
	int slots;
	size_t len;

	if (game && game[0]) {
		char user[64];
		char command[64];
		char answer[PATH_MAX + 128];
		char name[NAME_MAX + 1];
		char *tab;
		struct emu_settings e;

		if (!valid_game(game)) {
			reply(c, "ERR game\n");
			return;
		}
		if (!read_active_user(user, sizeof(user))) {
			reply(c, "ERR no-user\n");
			return;
		}
		snprintf(command, sizeof(command), "RESOLVE\t%s", game);
		if (library_request(command, answer, sizeof(answer)) != 0 ||
		    strncmp(answer, "OK ", 3) != 0 || !(tab = strchr(answer + 3, '\t'))) {
			reply(c, "ERR game\n");
			return;
		}
		*tab = '\0';
		load_settings(user, &e);
		slots = e.slots;
		content_base_name(tab + 1, name, sizeof(name));
		snprintf(base, sizeof(base), USERS_DIR "/%s/states/%s/%s", user, answer + 3, name);
	} else if (session.state == SESSION_RUNNING) {
		slots = session.settings.slots;
		copy_text(base, sizeof(base), session.state_base);
	} else {
		reply(c, "ERR no-session\n");
		return;
	}
	len = (size_t)snprintf(out, sizeof(out), "slots=%d\n", slots);
	for (int slot = AUTO_SLOT; slot < slots && len + 48 < sizeof(out); slot++) {
		long long t = state_mtime(base, slot);

		time_t tt = (time_t)t;
		struct tm tm;

		if (t < 0)
			continue;
		/* Local time of the save, for display. */
		if (!localtime_r(&tt, &tm))
			memset(&tm, 0, sizeof(tm));
		len += (size_t)snprintf(out + len, sizeof(out) - len, "state=%d\t%lld\t%02d:%02d\n",
					slot, t, tm.tm_hour, tm.tm_min);
	}
	len += (size_t)snprintf(out + len, sizeof(out) - len, "end=1\n");
	if (write_all(c->fd, out, len) != 0)
		close_client(c);
}

static void handle_command(struct client *c, char *line)
{
	char *arg = strchr(line, '\t');
	char status[512];

	trim(line);
	if (arg)
		*arg++ = '\0';

	if (!strcmp(line, "STATUS")) {
		size_t len = build_status(status, sizeof(status));

		if (write_all(c->fd, status, len) != 0)
			close_client(c);
	} else if (!strcmp(line, "SUBSCRIBE")) {
		size_t len = build_status(status, sizeof(status));

		c->subscribed = true;
		if (write_all(c->fd, status, len) != 0)
			close_client(c);
	} else if (!strcmp(line, "LAUNCH") && arg) {
		char *mode = strchr(arg, '\t');
		const char *error;

		if (mode)
			*mode++ = '\0';
		error = launch(arg, mode);

		if (error) {
			char text[64];

			log_msg("emud: launch %s refused: %s", arg, error);
			if (session.state == SESSION_IDLE)
				reset_session();
			snprintf(text, sizeof(text), "ERR %s\n", error);
			reply(c, text);
		} else {
			reply(c, "OK\n");
		}
	} else if (!strcmp(line, "OVERLAY_TEXT")) {
		/* The performance overlay line built by the Quick Menu, drawn by
		 * RetroArch into the frame ("" hides it). */
		if (!running(c))
			return;
		ra_command("OSD_TEXT %s", arg ? arg : "");
		reply(c, "OK\n");
	} else if (!strcmp(line, "PAUSE") || !strcmp(line, "RESUME")) {
		if (!running(c))
			return;
		ra_command("SET_PAUSED %d", !strcmp(line, "PAUSE") ? 1 : 0);
		reply(c, "OK\n");
	} else if (!strcmp(line, "SAVE_STATE")) {
		if (!running(c))
			return;
		if (!save_next()) {
			reply(c, "ERR busy\n");
			return;
		}
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "LOAD_STATE")) {
		if (!running(c))
			return;
		if (session.pending != PENDING_NONE) {
			reply(c, "ERR busy\n");
			return;
		}
		begin_load(session.slot);
		reply(c, "OK\n");
	} else if (!strcmp(line, "SLOT") && arg) {
		int slot = session.slot;

		if (!running(c))
			return;
		if (!strcmp(arg, "+1"))
			slot = step_slot(slot, 1);
		else if (!strcmp(arg, "-1"))
			slot = step_slot(slot, -1);
		else if (isdigit((unsigned char)arg[0]) && atoi(arg) < session.settings.slots)
			slot = atoi(arg);
		else {
			reply(c, "ERR slot\n");
			return;
		}
		session.slot = slot;
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "RESET")) {
		if (!running(c))
			return;
		ra_command("RESET");
		ra_command("SET_PAUSED 0");
		reply(c, "OK\n");
	} else if (!strcmp(line, "FAST_FORWARD")) {
		if (!running(c))
			return;
		handle_hotkey("fast_forward");
		reply(c, "OK\n");
	} else if (!strcmp(line, "ADVANCED")) {
		/* RetroArch Advanced: RetroArch's own menu over the game, for
		 * power users; it pauses the content itself. */
		if (!running(c))
			return;
		ra_command("SET_PAUSED 0");
		ra_command("MENU_TOGGLE");
		reply(c, "OK\n");
	} else if (!strcmp(line, "QUIT")) {
		if (!running(c))
			return;
		request_quit();
		reply(c, "OK\n");
	} else if (!strcmp(line, "SCREENSHOT")) {
		if (!running(c))
			return;
		reply(c, begin_screenshot() ? "OK\n" : "ERR busy\n");
	} else if ((!strcmp(line, "GAME_SETTINGS") || !strcmp(line, "GAME_SET") ||
		    !strcmp(line, "GAME_RESET")) && arg) {
		handle_game_settings(c, line, arg);
	} else if (!strcmp(line, "SETTINGS")) {
		handle_settings(c, NULL, NULL);
	} else if (!strcmp(line, "SET") && arg) {
		char *value = strchr(arg, '\t');

		if (value)
			*value++ = '\0';
		handle_settings(c, arg, value ? value : "");
	} else if (!strcmp(line, "STATES")) {
		handle_states(c, arg);
	} else if (!strcmp(line, "PRE_POWER") && arg) {
		if (strcmp(arg, "sleep") && strcmp(arg, "restart") &&
		    strcmp(arg, "poweroff")) {
			reply(c, "ERR action\n");
			return;
		}
		if (session.state == SESSION_IDLE) {
			reply(c, "OK\n");
			return;
		}
		if (session.power_client >= 0) {
			reply(c, "ERR busy\n");
			return;
		}
		session.power_client = (int)(c - clients);
		copy_text(session.power_action, sizeof(session.power_action), arg);
		/* Progress is safe in the automatic slot before every power
		 * action (CLAUDE §20.1); a state operation already running
		 * completes first. */
		if (session.state == SESSION_RUNNING &&
		    session.pending == PENDING_NONE)
			(void)begin_save(AUTO_SLOT, true);
		finish_power_wait();
	} else {
		reply(c, "ERR unknown\n");
	}
}

static void process_client(struct client *c)
{
	char buf[512];
	ssize_t n = read(c->fd, buf, sizeof(buf));

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close_client(c);
		return;
	}
	for (ssize_t i = 0; i < n && c->fd >= 0; i++) {
		if (buf[i] == '\n') {
			c->buf[c->used] = '\0';
			handle_command(c, c->buf);
			c->used = 0;
		} else if (c->used + 1 < sizeof(c->buf)) {
			c->buf[c->used++] = buf[i];
		} else {
			c->used = 0;
		}
	}
}

static int make_server(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	copy_text(addr.sun_path, sizeof(addr.sun_path), SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    chmod(SOCKET_PATH, 0660) != 0 || listen(fd, 8) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void accept_client(int server)
{
	int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

	if (fd < 0)
		return;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0)
			continue;
		clients[i].fd = fd;
		clients[i].subscribed = false;
		clients[i].used = 0;
		return;
	}
	close(fd);
}

int main(void)
{
	struct sigaction sa;
	int server;
	FILE *pid_fp;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	reset_session();

	if (mkdir_p(RUN_ROOT) != 0 || pipe2(sigchld_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		perror("nuubos-emud: init");
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = handle_stop;
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	server = make_server();
	if (server < 0) {
		perror("nuubos-emud: socket");
		return 1;
	}
	pid_fp = fopen(PIDFILE, "w");
	if (pid_fp) {
		fprintf(pid_fp, "%ld\n", (long)getpid());
		fclose(pid_fp);
	}
	log_msg("emud: started pid=%ld", (long)getpid());

	while (!stop_requested) {
		struct pollfd pfd[6 + MAX_CLIENTS];
		int who[6 + MAX_CLIENTS];
		nfds_t count = 0;
		int rc;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		if (session.stdout_fd >= 0) {
			pfd[count] = (struct pollfd){ session.stdout_fd, POLLIN, 0 };
			who[count++] = -3;
		}
		if (session.inotify_fd >= 0) {
			pfd[count] = (struct pollfd){ session.inotify_fd, POLLIN, 0 };
			who[count++] = -4;
		}
		if (session.input_fd >= 0) {
			pfd[count] = (struct pollfd){ session.input_fd, POLLIN, 0 };
			who[count++] = -5;
		}
		if (session.shot_fd > 0) {
			pfd[count] = (struct pollfd){ session.shot_fd, POLLIN, 0 };
			who[count++] = -6;
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			pfd[count] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
			who[count++] = i;
		}

		rc = poll(pfd, count, session_timeout_ms());
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		session_timers();
		for (nfds_t i = 0; i < count; i++) {
			if (!pfd[i].revents)
				continue;
			switch (who[i]) {
			case -1:
				accept_client(server);
				break;
			case -2:
				reap_children();
				break;
			case -3:
				if (session.stdout_fd >= 0)
					process_ra_output();
				break;
			case -4:
				if (session.inotify_fd >= 0)
					process_watch();
				break;
			case -5:
				if (session.input_fd >= 0)
					process_input_link();
				break;
			case -6:
				if (session.shot_fd > 0)
					process_shot_watch();
				break;
			default:
				if (clients[who[i]].fd >= 0)
					process_client(&clients[who[i]]);
				break;
			}
		}
	}

	if (session.state != SESSION_IDLE && session.pid > 0) {
		/* Service stop (shutdown): let RetroArch close the content and
		 * write its save files, woken by SIGCHLD, bounded. */
		long long deadline = monotonic_ms() + QUIT_TIMEOUT_MS;
		bool exited = false;

		ra_command("SET_PAUSED 0");
		ra_command("QUIT");
		while (!exited && monotonic_ms() < deadline) {
			struct pollfd p = { sigchld_pipe[0], POLLIN, 0 };
			char drain[32];

			(void)poll(&p, 1, (int)(deadline - monotonic_ms()));
			while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
				;
			exited = waitpid(session.pid, NULL, WNOHANG) == session.pid;
		}
		if (!exited)
			kill(-session.pid, SIGKILL);
		library_session("SESSION_END", session.game);
	}
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
