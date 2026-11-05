/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
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
 * replies once the game state is safe. MENU starts RetroArch's own menu
 * without a game (menu=1 in the snapshot; only QUIT, PAUSE and RESUME
 * apply), RESET_RETROARCH puts the user's RetroArch configuration back to
 * the defaults (no session running). DISC <index> changes the disc of a
 * multi-disc game (disc=, discs= in the snapshot; discs is read from
 * RetroArch at every PAUSE, i.e. when the Quick Menu opens). STORAGE_LOST <mount point> (from
 * nuubos-storagectl when the games card is pulled out): a game whose ROM
 * was on it is saved in the automatic state and closed, with a notice.
 *
 * LAUNCH takes a third argument for netplay on the local network: "host",
 * or "connect:<address>:<port>:<core>" (a host found by NETPLAY_HOSTS, the
 * LAN discovery of RetroArch hosts, with the host's emulator). Over a
 * running game NETPLAY_HOST restarts it as a host from its saved progress,
 * NETPLAY_STOP ends netplay (the game goes on); the snapshot follows
 * RetroArch's netplay events (netplay=off|starting|host|client,
 * netplay_peers=, netplay_peer=). CHEATS lists the running game's cheat
 * lists and cheats, CHEAT_SET <k> <0|1> and CHEAT_FILE <path> change them
 * (kept per user and game, applied at once; never in hardcore mode or with
 * netplay). Cores the user downloaded for a system (its libretro databases,
 * libretro.conf) are emulators of that system like the cores.conf ones.
 *
 * Per-game configuration (EPIC-018): GAME_SETTINGS <game> <system> lists the
 * effective values and the user's explicit overrides; GAME_SET <game>
 * <system> <key> <value> (empty value = inherit) and GAME_RESET <game>
 * change them. Only overrides are stored, per user, in
 * /userdata/users/<id>/appdata/nuubos-emulation/games/<game>.conf. They
 * apply at the next launch. System Settings (GitHub #12): the same keys
 * per system in .../nuubos-emulation/systems/<system>.conf; a key the game
 * leaves unset follows its system: core choice, aspect ratio and video filter on
 * top of the user's RetroArch configuration, and the emulation accuracy of
 * melonDS-nuubOS and Flycast (translucent sorting) and the 3D render resolution (resolution_options) in the
 * user's core options for the session. RetroArch saves its whole
 * configuration on exit, so the keys a session overrode are put back to
 * the user's own values when it ends.
 *
 * RetroAchievements (EPIC-017): a user logged in with
 * nuubos-achievementsctl has /state/users/<id>/secrets/retroachievements.conf
 * (user name and API token, never the password); the session enables
 * RetroArch's achievements with that token, and hardcore mode when chosen.
 * In hardcore mode (user request 2026-10-11) the session has no save
 * states at all: SAVE_STATE, LOAD_STATE, SLOT and FAST_FORWARD (and their
 * hotkeys) are refused with game.hardcore, the game starts from the
 * beginning and nothing is saved when it closes. Outcomes the user must
 * see (state saved, empty slot, launch failure...) are typed notifications
 * (EPIC-006).
 *
 * Idle: poll() without timeout. A timeout is armed only while a save or a
 * quit is pending.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/resource.h>
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
#define UI_READY RUN_ROOT "/ui-ready"
#define SOCKET_PATH RUN_ROOT "/emud.sock"
#define PIDFILE RUN_ROOT "/emud.pid"
#define SESSION_DIR RUN_ROOT "/emulation"
#define SESSION_CFG SESSION_DIR "/session.cfg"
#define SESSION_SHADER SESSION_DIR "/session.glslp"
#define RETROARCH_LOG RUN_ROOT "/retroarch.log"
#define DIAG_ACTIVE RUN_ROOT "/diagnostics/active"
#define DIAG_RETROARCH_LOG RUN_ROOT "/diagnostics/retroarch.log"
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
/* nuubOS systems in the libretro databases (core-info "database"). */
#define LIBRETRO_CONF SHARE_DIR "/libretro.conf"
#define DEFAULT_RA_CFG SHARE_DIR "/retroarch.cfg"
#define DEFAULT_CORE_OPTIONS SHARE_DIR "/retroarch-core-options.cfg"
#define CORE_OPTION_UPDATES SHARE_DIR "/core-option-updates.conf"
#define DEFAULT_SHADER_PRESET SHARE_DIR "/shaders/global.glslp"
#define SHADER_DIR SHARE_DIR "/shaders"
/* Performance overlay font (RetroArch patch 0004 draws with the OSD font). */
#define OSD_FONT "/usr/share/fonts/cantarell/Cantarell-Bold.otf"
#define DISPLAY_CONF SHARE_DIR "/display.conf"
/* Game render height on HDMI: RetroArch (patch 0005) draws a buffer of
 * this many lines, never more than its logical window (1280x720 on a
 * 1080p TV at the HDMI output scale 1.5), and the display engine scales it
 * to the mode on its plane (kernel patch 0031). 720: 240p content at an
 * integer 3x (GBA 4x) inside the buffer, full speed measured 2026-10-18 on
 * nes snes megadrive pcengine psx gba gbc n64 psp nds (GPU fragment
 * 36-60 %); a true 1920x1080 buffer saturates the GPU (CRT systems 38-42
 * fps). display.conf field 3 overrides it per system (0 = native
 * resolution, lower = cheaper for 3D-heavy systems). The panel never
 * uses it. */
#define GAME_HDMI_LINES 720
#ifndef HDMI_ENABLED
#define HDMI_ENABLED "/sys/class/drm/card0-HDMI-A-1/enabled"
#endif
#ifndef DRM_CLASS
#define DRM_CLASS "/sys/class/drm"
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
#ifndef CORE_INFO_DIR
#define CORE_INFO_DIR "/usr/share/libretro/info"
#endif
#define RA_ASSETS_DIR "/usr/share/retroarch/assets"
#define RA_AUTOCONFIG_DIR "/usr/share/retroarch/autoconfig"

#ifndef USERDATA
#define USERDATA "/userdata"
#endif
#define DOWNLOADED_CORES USERDATA "/retroarch/cores"
#define BIOS_DIR USERDATA "/bios"
#define CHEATS_DIR USERDATA "/cheats"
/* The libretro cheat collection (nuubos-libretroctl cheats update): an
 * archive with an index of its files; the files of a game played are
 * extracted into one folder per database. */
#define CHEATS_LIBRETRO CHEATS_DIR "/libretro"
#define CHEATS_ARCHIVE CHEATS_LIBRETRO "/cheats.zip"
#define CHEATS_INDEX CHEATS_LIBRETRO "/index.txt"
#define SESSION_CHEATS SESSION_DIR "/session.cht"
#define CACHE_DIR USERDATA "/cache/retroarch"
#define USERS_DIR USERDATA "/users"
#ifndef STATE_USERS
#define STATE_USERS "/state/users"
#endif
#define GAME_SETTINGS_SUBDIR "appdata/nuubos-emulation/games"
/* System Settings (GitHub #12): the same keys for every game of a system. */
#define SYSTEM_SETTINGS_SUBDIR "appdata/nuubos-emulation/systems"
#define EMU_SETTINGS_FILE "appdata/nuubos-emulation/settings.conf"
#define ACHIEVEMENTS_FILE "secrets/retroachievements.conf"
#define PROFILE_FILE "profile.conf"

#include "core-check.h"

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
#define MAX_RESTORE_KEYS 48
/* Performance overlay: RetroArch frame rate sampled once a second. */
#define OVERLAY_TICK_MS 1000
/* Disc change: how long the game runs with the tray open before the new
 * disc goes in (games notice a disc change by the open lid). */
#define DISC_INSERT_MS 1000
/* Netplay (RetroArch's protocol): TCP game port and LAN discovery port,
 * how long a discovery listens for hosts. */
#define NETPLAY_PORT 55435
#define DISCOVERY_MS 1500
/* Cheat lists offered for a game (its own files, the collection's). */
#define MAX_CHEAT_LISTS 12

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
 *   bezels, crt   picture presentation (RetroArch patch 0010, crt.glsl)
 *   position      top, center or bottom: where a picture lower than the
 *                 screen sits (GitHub #13, user decision 2026-10-28:
 *                 center by default)
 *   overlay       performance overlay over games, overlay_items its fields
 *   retroarch_menu the Quick Menu's RetroArch row in a game (hidden by
 *                 default, user decision 2026-10-27)
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
	HK_SCREEN_LAYOUT,
	HK_STYLUS,
	HK_COUNT,
};

static const char *const hotkey_actions[HK_COUNT] = {
	"save_state", "load_state", "slot_next", "slot_prev",
	"fast_forward", "quit", "screenshot", "screen_layout", "stylus",
};

/* nuubos-controllersd control ids (the user's mapping applies). */
static const char *const hotkey_defaults[HK_COUNT] = {
	"r1", "l1", "menu_up", "menu_down", "face_north", "settings", "l2",
	"select", "r2",
};

static const char *const hotkey_controls[] = {
	"menu_confirm", "menu_back", "face_north", "face_west",
	"menu_up", "menu_down", "menu_left", "menu_right",
	"l1", "r1", "l2", "r2", "l3", "r3", "settings", "select", NULL,
};

/* melonDS-nuubOS (patches 0010+): the screen layouts the hotkey cycles
 * through (core option melonds_screen_layout; R2 in the game swaps the
 * two screens of each) and their notification names. */
#define MELONDS_NUUBOS "melonds_nuubos"
static const struct {
	const char *value;
	const char *name;
} nds_layouts[] = {
	{ "Top/Bottom", "two" },
	{ "Large Top", "large" },
	{ "Picture in Picture Top", "pip" },
	{ "Top Only", "single" },
};
#define NDS_LAYOUTS (int)(sizeof(nds_layouts) / sizeof(nds_layouts[0]))
/* Picture in Picture: the small screen's corner (GitHub #5, core option
 * melonds_nuubos_pip_corner, patch 0023); the first is the default. */
static const char *const nds_corners[] = { "bottom-right", "bottom-left", "top-right", "top-left" };
#define NDS_CORNERS (int)(sizeof(nds_corners) / sizeof(nds_corners[0]))

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
	char position[8];
	bool overlay;
	char overlay_items[96];
	bool retroarch_menu;
	char hotkey[HK_COUNT][16];
};

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

/* Netplay of the session, from RetroArch's events (patch 0008). */
enum netplay_mode {
	NET_OFF = 0,
	NET_HOST,
	NET_CLIENT,
};

struct session {
	enum session_state state;
	pid_t pid;
	int stdin_fd;
	int stdout_fd;
	int input_fd;
	/* Controls of the game (REMAP to inputd), "control:target,...". */
	char remap[512];
	char out_buf[MAX_LINE];
	size_t out_used;
	char user[64];
	char game[32];
	char system[64];
	char core[64];
	/* RetroArch's own menu without a game (MENU, the RetroArch app). */
	bool menu;
	char content[PATH_MAX];
	/* The ROM file itself (content may be a generated MAME .cmd). */
	char rom[PATH_MAX];
	char states_dir[PATH_MAX];
	char state_base[PATH_MAX];
	/* A PSP game in a folder of its own: the folder and where RetroArch
	 * sees it on the memory stick (psp_folder_game). */
	char game_dir[PATH_MAX];
	char game_mount[PATH_MAX];
	int slot;
	bool paused;
	bool fast_forward;
	/* RetroAchievements hardcore: no save states, no fast-forward. */
	bool hardcore;
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
	/* Slot rotation: the progress of a closing game goes into the next
	 * slot, saved by emud before RetroArch is asked to quit. */
	bool quit_requested;
	bool quit_saved;
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
	/* Frame-rate aware CPU limit: step in game_cpu_caps, uncapped until
	 * cpu_grace_ms. */
	int cpu_level;
	long long cpu_grace_ms;
	/* Cheats turned on in the session (0 = none loaded). */
	int cheats_on;
	/* Netplay: what the launch asked for ("host", "connect"), the state
	 * RetroArch reports, the players connected to this host, the other
	 * side's nickname; host again once the game has closed (Host Game
	 * over a running game restarts it as a host from its saved
	 * progress). */
	char net_launch[16];
	enum netplay_mode netplay;
	int net_peers;
	char net_peer[40];
	bool relaunch_host;
	/* melonDS-nuubOS: the user's core options file, the screen layout the
	 * hotkey cycles through (nds_layouts), the stylus mode, and the
	 * user's accuracy to put back at exit when the game overrides it. */
	char core_options[PATH_MAX];
	/* NUUBOS_RA_BEZEL for RetroArch (patch 0010), "" = no bezel. */
	char bezel_env[1024];
	/* Per-game Square Pixels: NUUBOS_RA_PIXEL_CROP (patch 0014). */
	bool pixel_crop;
	int nds_layout;
	int nds_corner;
	bool stylus;
	char stylus_control[8];
	/* Disc control (multi-disc games, RetroArch patch 0013): the disc
	 * in use and how many (0 = no disc control), from GET_DISK; while a
	 * change is under way the tray closes at disc_insert_ms. */
	int disc;
	int discs;
	long long disc_insert_ms;
	/* Core options this session set in the user's file (per-game
	 * accuracy) and their values to put back at exit. */
	int opt_restore_count;
	char opt_restore_key[8][64];
	char opt_restore_value[8][128];
};

/* LAN discovery of netplay hosts (NETPLAY_HOSTS): one at a time, every
 * client asking meanwhile gets the same answer. */
struct discovery {
	int fd;
	long long deadline_ms;
	bool waiting[MAX_CLIENTS];
	int count;
	struct {
		char address[INET_ADDRSTRLEN];
		int port;
		bool password;
		char nick[32];
		char core[32];
		char content[256];
	} hosts[16];
};

/* Controls (user request 2026-10-29): what each control of the console
 * sends to the game's pad, per game over per system ("" = inherited, the
 * control itself; "none" = nothing). Ids are the Controllers service's, in
 * Game Hotkeys order; inputd applies them on the game gamepads (REMAP),
 * the hotkey chords stay on the physical controls. */
#define REMAP_COUNT 16
static const char *const remap_controls[REMAP_COUNT] = {
	"menu_confirm", "menu_back", "face_north", "face_west", "menu_up", "menu_down", "menu_left",
	"menu_right", "l1", "r1", "l2", "r2", "l3", "r3", "settings", "select",
};

/* The buttons of each system's pad, as RetroArch's pad controls the cores
 * map them to (their input descriptors, 2026-10-29): control:label, an
 * empty label = the control's own name (Start, Select), "#<n>" = string n.
 * The D-pad is every system's. Systems not listed use RetroArch's names. */
static const struct pad_layout {
	const char *systems; /* space separated, with a space at both ends */
	const char *buttons;
} pad_layouts[] = {
	{ " nes fds ", "menu_back:A,menu_confirm:B,select:,settings:" },
	{ " snes satellaview sufami nds ",
	  "menu_back:A,menu_confirm:B,face_north:X,face_west:Y,l1:L,r1:R,select:,settings:" },
	{ " gb gbc megaduck ", "menu_back:A,menu_confirm:B,select:,settings:" },
	{ " gba ", "menu_back:A,menu_confirm:B,l1:L,r1:R,select:,settings:" },
	{ " n64 ", "menu_confirm:A,face_west:B,l2:Z,l1:L,r1:R,settings:" },
	{ " megadrive segacd sega32x ",
	  "face_west:A,menu_confirm:B,menu_back:C,l1:X,face_north:Y,r1:Z,select:Mode,settings:" },
	{ " mastersystem gamegear sg1000 ", "menu_confirm:1,menu_back:2,settings:" },
	{ " psx ", "menu_confirm:#1459,menu_back:#1460,face_north:#1461,face_west:#1462,"
		   "l1:L1,r1:R1,l2:L2,r2:R2,l3:L3,r3:R3,select:,settings:" },
	{ " psp ", "menu_confirm:#1459,menu_back:#1460,face_north:#1461,face_west:#1462,l1:L,r1:R,select:,settings:" },
	{ " saturn ", "menu_confirm:A,menu_back:B,l1:C,face_west:X,face_north:Y,r1:Z,l2:L,r2:R,settings:" },
	{ " dreamcast ", "menu_confirm:A,menu_back:B,face_west:X,face_north:Y,l2:L,r2:R,settings:" },
	{ " pcengine pcecd ", "menu_back:I,menu_confirm:II,select:,settings:Run" },
};

static const char pad_generic[] =
	"menu_back:A,menu_confirm:B,face_north:X,face_west:Y,l1:L,r1:R,l2:L2,r2:R2,l3:L3,r3:R3,select:,settings:";

static const char *system_pad(const char *system)
{
	char word[40];

	snprintf(word, sizeof(word), " %s ", system);
	for (size_t i = 0; i < sizeof(pad_layouts) / sizeof(pad_layouts[0]); i++)
		if (strstr(pad_layouts[i].systems, word))
			return pad_layouts[i].buttons;
	return pad_generic;
}

static int remap_index(const char *control)
{
	for (int i = 0; i < REMAP_COUNT; i++)
		if (!strcmp(remap_controls[i], control))
			return i;
	return -1;
}

/* Explicit per-game overrides; empty = inherited. The cheats are the
 * user's choice for the game: the list (a .cht file) and the indices of
 * the cheats turned on in it ("0,4,7"). */
struct game_overrides {
	char core[64];
	char aspect[16];
	char filter[16];
	char accuracy[16];
	char cheat_file[PATH_MAX];
	char cheats[512];
	/* melonDS-nuubOS: the screen layout (nds_layouts name) and stylus
	 * (off|right|left|dpad) the user last chose in this game, restored at
	 * its next launch (user request 2026-10-18). */
	char nds_layout[16];
	char stylus[8];
	/* Picture in Picture corner (nds_corners), "" = bottom right. */
	char nds_corner[16];
	/* Nintendo 64: the accessory in the controllers' slot, rumble (the
	 * default, empty) or memory (Controller Pak saves). One slot, as on the
	 * console (user request 2026-10-21: "scelta per gioco"). */
	char n64_pak[8];
	/* 3D render resolution: "" (Auto), "1x" or "2x". */
	char resolution[8];
	/* Frameskip: "" (the user's core option), "off", "auto" or "max". */
	char frameskip[8];
	/* Frame-rate aware CPU limit: the step the game needed, its next
	 * launch starts there (no stutter at every launch while it climbs). */
	char cpu_level[4];
	/* Controls: the target of each of remap_controls. */
	char remap[REMAP_COUNT][16];
};

static bool remap_set(const struct game_overrides *o)
{
	for (int i = 0; i < REMAP_COUNT; i++)
		if (o->remap[i][0])
			return true;
	return false;
}

/* "control:target,..." of the controls set. */
static void remap_list(const struct game_overrides *o, char *out, size_t size)
{
	size_t len = 0;

	out[0] = '\0';
	for (int i = 0; i < REMAP_COUNT; i++) {
		int n;

		if (!o->remap[i][0])
			continue;
		n = snprintf(out + len, size - len, "%s%s:%s", len ? "," : "", remap_controls[i], o->remap[i]);
		if (n < 0 || (size_t)n >= size - len)
			break;
		len += (size_t)n;
	}
}

/* Cores whose 3D render resolution Game Settings chooses (user decision
 * 2026-10-27): the option and its 1x (native) and 2x values, matched by the
 * prefix of the core id (bundled and downloaded builds share the keys).
 * Auto keeps the user's core option, except PCSX ReARMed: 1x on the
 * handheld screen, 2x on HDMI (`auto_hdmi`). */
static const struct resolution_option {
	const char *core;
	const char *key;
	const char *x1;
	const char *x2;
	bool auto_hdmi;
} resolution_options[] = {
	{ "pcsx_rearmed", "pcsx_rearmed_neon_enhancement_enable", "disabled", "enabled", true },
	{ "ppsspp", "ppsspp_internal_resolution", "480x272", "960x544", false },
	{ "flycast", "reicast_internal_resolution", "640x480", "1280x960", false },
	{ "yabasanshiro", "yabasanshiro_resolution_mode", "original", "2x", false },
	{ "parallel_n64", "parallel-n64-screensize", "320x240", "640x480", false },
	{ "mupen64plus_next", "mupen64plus-43screensize", "320x240", "640x480", false },
};

/* Flycast's translucent polygon sorting is its emulation accuracy (user
 * decision 2026-10-27, GitHub #16): Fast = per strip (cheaper, smoke, glass
 * and water can be drawn in the wrong order), Accurate = per triangle, the
 * nuubOS default. */
#define FLYCAST_SORTING "reicast_alpha_sorting"
#define FLYCAST_SORTING_FAST "per-strip (fast, least accurate)"
#define FLYCAST_SORTING_ACCURATE "per-triangle (normal)"

static bool flycast_core(const char *core)
{
	return !strncmp(core, "flycast", 7);
}

static const struct resolution_option *resolution_option(const char *core)
{
	for (size_t i = 0; i < sizeof(resolution_options) / sizeof(resolution_options[0]); i++)
		if (!strncmp(core, resolution_options[i].core, strlen(resolution_options[i].core)))
			return &resolution_options[i];
	return NULL;
}

/* Cores whose frameskip Game Settings chooses (user decision 2026-10-29,
 * GitHub #23): Off, Auto (a frame skipped only while the emulation falls
 * behind, at most one in a row) or Max (the same, up to three in a row).
 * Up to two options per core, their values for off/auto/max, and the value
 * a key RetroArch has not written yet holds (the core's default). */
enum { FRAMESKIP_OFF, FRAMESKIP_AUTO, FRAMESKIP_MAX, FRAMESKIP_MODES };
static const char *const frameskip_modes[FRAMESKIP_MODES] = { "off", "auto", "max" };

static const struct frameskip_option {
	const char *core;
	const char *key[2];
	const char *value[2][FRAMESKIP_MODES];
	const char *initial[2];
} frameskip_options[] = {
	{ MELONDS_NUUBOS, { "melonds_frameskip", NULL },
	  { { "disabled", "auto", "auto_max" } }, { "disabled" } },
	{ "flycast", { "reicast_frame_skipping", NULL },
	  { { "disabled", "auto", "auto_max" } }, { "disabled" } },
	{ "ppsspp", { "ppsspp_auto_frameskip", "ppsspp_frameskip" },
	  { { "disabled", "enabled", "enabled" }, { "disabled", "1", "3" } }, { "disabled", "disabled" } },
	{ "pcsx_rearmed", { "pcsx_rearmed_frameskip_type", "pcsx_rearmed_frameskip_interval" },
	  { { "disabled", "auto", "auto" }, { NULL, "1", "3" } }, { "disabled", "3" } },
};

static const struct frameskip_option *frameskip_option(const char *core)
{
	for (size_t i = 0; i < sizeof(frameskip_options) / sizeof(frameskip_options[0]); i++)
		if (!strncmp(core, frameskip_options[i].core, strlen(frameskip_options[i].core)))
			return &frameskip_options[i];
	return NULL;
}

static int frameskip_mode(const char *name)
{
	for (int m = 0; m < FRAMESKIP_MODES; m++)
		if (!strcmp(name, frameskip_modes[m]))
			return m;
	return -1;
}



static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];
static struct session session;
static struct discovery discovery = { .fd = -1 };

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

/* fclose() after the data reached the card. /userdata is exFAT: a file
 * renamed over the old one before its data was written comes back empty
 * after a crash or a power loss (the emulation settings, 2026-10-29). */
static int close_synced(FILE *fp)
{
	int rc = fflush(fp) != 0 || fsync(fileno(fp)) != 0 ? -1 : 0;

	if (fclose(fp) != 0)
		rc = -1;
	return rc;
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

/* Sets key to new_value in the core options file at path when it holds
 * old_value; true when the file was readable. */
static bool update_core_option(const char *path, const char *key,
			       const char *old_value, const char *new_value)
{
	char tmp[PATH_MAX + 8];
	char line[1024];
	bool changed = false;
	FILE *in = fopen(path, "r");
	FILE *out;

	if (!in)
		return false;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return false;
	}
	while (fgets(line, sizeof(line), in)) {
		char k[128], v[256];

		if (sscanf(line, " %127[^ =#] = \"%255[^\"]\"", k, v) == 2 &&
		    !strcmp(k, key) && !strcmp(v, old_value)) {
			fprintf(out, "%s = \"%s\"\n", key, new_value);
			changed = true;
		} else {
			fputs(line, out);
		}
	}
	fclose(in);
	if (close_synced(out) != 0 || (changed && rename(tmp, path) != 0)) {
		unlink(tmp);
		return false;
	}
	if (!changed)
		unlink(tmp);
	else
		log_msg("emud: core option %s %s -> %s", key, old_value, new_value);
	return true;
}

/* One-time updates of core option defaults (core-option-updates.conf: id|
 * key|old|new): a key still holding the old default gets the new one, a
 * value the user chose is kept; applied ids are listed in done_path so each
 * runs once per user. */
static void apply_core_option_updates(const char *path, const char *done_path)
{
	char line[512];
	FILE *in = fopen(CORE_OPTION_UPDATES, "r");

	if (!in)
		return;
	while (fgets(line, sizeof(line), in)) {
		char id[96], key[128], old_value[128], new_value[128];
		char done[128];
		bool applied = false;
		FILE *fp;

		if (line[0] == '#' ||
		    sscanf(line, "%95[^|\n]|%127[^|\n]|%127[^|\n]|%127[^|\n]",
			   id, key, old_value, new_value) != 4)
			continue;
		fp = fopen(done_path, "r");
		while (fp && !applied && fgets(done, sizeof(done), fp)) {
			trim(done);
			applied = !strcmp(done, id);
		}
		if (fp)
			fclose(fp);
		if (applied || !update_core_option(path, key, old_value, new_value))
			continue;
		fp = fopen(done_path, "a");
		if (fp) {
			fprintf(fp, "%s\n", id);
			fclose(fp);
		}
	}
	fclose(in);
}

/* The value of key in a core options file. */
static bool read_core_option(const char *path, const char *key, char *out, size_t size)
{
	char line[1024];
	bool found = false;
	FILE *in = fopen(path, "r");

	if (!in)
		return false;
	while (!found && fgets(line, sizeof(line), in)) {
		char k[128], v[256];

		if (sscanf(line, " %127[^ =#] = \"%255[^\"]\"", k, v) == 2 && !strcmp(k, key)) {
			copy_text(out, size, v);
			found = true;
		}
	}
	fclose(in);
	return found;
}

/* Sets key to value in a core options file (appended when missing). */
static void write_core_option(const char *path, const char *key, const char *value)
{
	char tmp[PATH_MAX + 8];
	char line[1024];
	bool done = false;
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
		char k[128];

		if (sscanf(line, " %127[^ =#] =", k) == 1 && !strcmp(k, key)) {
			if (!done)
				fprintf(out, "%s = \"%s\"\n", key, value);
			done = true;
		} else {
			fputs(line, out);
		}
	}
	if (!done)
		fprintf(out, "%s = \"%s\"\n", key, value);
	fclose(in);
	if (close_synced(out) != 0 || rename(tmp, path) != 0)
		unlink(tmp);
}

/* The mode the user's core options give: off, auto, max or custom. */
static const char *frameskip_default(const struct frameskip_option *fs, const char *options)
{
	char value[2][64];

	for (int k = 0; k < 2 && fs->key[k]; k++)
		if (!read_core_option(options, fs->key[k], value[k], sizeof(value[k])))
			copy_text(value[k], sizeof(value[k]), fs->initial[k]);
	for (int m = 0; m < FRAMESKIP_MODES; m++) {
		bool match = true;

		for (int k = 0; k < 2 && fs->key[k]; k++)
			if (fs->value[k][m] && strcmp(value[k], fs->value[k][m]))
				match = false;
		if (match)
			return frameskip_modes[m];
	}
	return "custom";
}

/* Sets a core option for this session only: the user's value comes back
 * when the game ends (after RetroArch wrote its options). */
static void session_core_option(const char *key, const char *value)
{
	int n = session.opt_restore_count;

	if (n >= 8 || !read_core_option(session.core_options, key,
					 session.opt_restore_value[n],
					 sizeof(session.opt_restore_value[n])))
		return;
	copy_text(session.opt_restore_key[n], sizeof(session.opt_restore_key[n]), key);
	session.opt_restore_count++;
	write_core_option(session.core_options, key, value);
}

static void restore_session_core_options(void)
{
	for (int i = session.opt_restore_count - 1; i >= 0; i--)
		write_core_option(session.core_options, session.opt_restore_key[i],
				  session.opt_restore_value[i]);
	session.opt_restore_count = 0;
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

/* Support files of the image (SYSTEM_FILES, ~110 MB / 600 files: PPSSPP
 * assets, ScummVM data, blueMSX databases, EmuTOS) are copied to BIOS_DIR
 * by a low-priority child once per service start: a new image brings new
 * files. Done inside LAUNCH it held the reply for tens of seconds after a
 * flash (fsync per file on exFAT), past Home's 8 s wait: "could not be
 * started", then the game came up anyway (user report 2026-10-17). */
#define SYSTEM_FILE_CORES "ppsspp_nuubos,ppsspp,scummvm,hatari,bluemsx"

static bool list_has(const char *list, const char *name);

static pid_t seed_pid = -1;

static void start_system_seed(void)
{
	pid_t pid = fork();

	if (pid == 0) {
		if (nice(19) == -1) {
			/* copies at normal priority */
		}
		/* At boot not before nuubUI is ready (at the latest 60 s on):
		 * even with nothing to copy, the walk (a lookup per file on the
		 * image and on exFAT) read ~27 MB from the card during the UI's
		 * own start (CubeXX 2026-10-22; nice is no I/O priority with
		 * mq-deadline; docs/dev/platform.md "Boot time"). */
		for (int i = 0; i < 300 && access(UI_READY, F_OK) != 0; i++)
			usleep(200000);
		seed_tree(SYSTEM_FILES, BIOS_DIR);
		_exit(0);
	}
	if (pid < 0)
		seed_tree(SYSTEM_FILES, BIOS_DIR);
	else
		seed_pid = pid;
}

/* A core that reads those files waits for a copy still running. */
static void wait_system_seed(const char *core)
{
	if (seed_pid <= 0 || !list_has(SYSTEM_FILE_CORES, core))
		return;
	while (waitpid(seed_pid, NULL, 0) < 0 && errno == EINTR)
		;
	seed_pid = -1;
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

static void notify_storage_lost(void)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.session", "game.storage_lost");
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

/* An action hardcore mode does not allow (save/load state, slot,
 * fast-forward). */
static void notify_hardcore(void)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.state", "game.hardcore");
	(void)nuubos_notify_send(&n);
}

static void notify_fast_forward(bool on)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.fastforward", "game.fastforward");
	nuubos_notify_str(&n, "state", on ? "on" : "off");
	(void)nuubos_notify_send(&n);
}

static void notify_screens(int layout)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.screens", "game.screens");
	nuubos_notify_str(&n, "layout", nds_layouts[layout].name);
	(void)nuubos_notify_send(&n);
}

/* state=off|right|left|dpad: the control moving the stylus. */
static void notify_stylus(const char *state)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "game.stylus", "game.stylus");
	nuubos_notify_str(&n, "state", state);
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
			 "retroarch_menu=%d\n"
			 "menu=%d\n"
			 "fps=%.1f\n"
			 "hardcore=%d\n"
			 "cheats_on=%d\n"
			 "netplay=%s\n"
			 "netplay_peers=%d\n"
			 "netplay_peer=%s\n"
			 "nds_layout=%d\n"
			 "nds_corner=%d\n"
			 "stylus=%s\n"
			 "disc=%d\n"
			 "discs=%d\n"
			 "quitting=%d\n"
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
			 session.state != SESSION_IDLE && session.settings.retroarch_menu ? 1 : 0,
			 session.menu ? 1 : 0,
			 session.fps,
			 session.state != SESSION_IDLE && session.hardcore ? 1 : 0,
			 session.cheats_on,
			 session.state == SESSION_IDLE ? "off"
			 : session.netplay == NET_HOST ? "host"
			 : session.netplay == NET_CLIENT ? "client"
			 : session.net_launch[0] ? "starting" : "off",
			 session.net_peers, session.net_peer,
			 /* melonDS-nuubOS only (Quick Menu rows), else -1/off. */
			 session.state != SESSION_IDLE && !strcmp(session.core, MELONDS_NUUBOS)
			 ? session.nds_layout : -1,
			 session.nds_corner,
			 session.state != SESSION_IDLE && session.stylus ? session.stylus_control : "off",
			 session.disc, session.discs,
			 /* The game is closing: quit asked (its automatic save may
			 * still run) or RetroArch exiting. The Quick Menu covers it
			 * with the closing screen; not when it restarts as netplay
			 * host. */
			 !session.relaunch_host &&
			 (session.state == SESSION_EXITING ||
			  (session.state == SESSION_RUNNING && session.quit_requested)) ? 1 : 0);

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
	discovery.waiting[c - clients] = false;
}

static void reply(struct client *c, const char *text)
{
	if (c->fd >= 0 && write_all(c->fd, text, strlen(text)) != 0)
		close_client(c);
}

/* Low-latency Wi-Fi (radio power save off) while a netplay session runs:
 * nuubos-wifid watches this directory (battery audit 2026-10-14). */
#define LOW_LATENCY_MARK RUN_ROOT "/low-latency/emud"

static void sync_low_latency(void)
{
	static int marked = -1;
	int want = session.state != SESSION_IDLE &&
		   (session.net_launch[0] || session.netplay != NET_OFF);

	if (want == marked)
		return;
	if (want) {
		int fd;

		(void)mkdir(RUN_ROOT "/low-latency", 0755);
		fd = open(LOW_LATENCY_MARK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0)
			close(fd);
	} else {
		(void)unlink(LOW_LATENCY_MARK);
	}
	marked = want;
}

static void notify_subscribers(void)
{
	char status[768];

	sync_low_latency();
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

	if (session.menu)
		return;
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

/* Data on SD2 (nuubos-storagectl, GitHub #24): BIOS files, saves or states
 * on the SD2 card while it is away (or moving): a game would start without
 * them and write nowhere, so it waits for the card. */
static bool data_away(void)
{
	FILE *fp = fopen("/run/nuubos/storage/data.status", "r");
	char line[256], on[160] = "", state[32] = "";

	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (!strncmp(line, "DATA_ON=", 8))
			copy_text(on, sizeof(on), line + 8);
		else if (!strncmp(line, "DATA_STATE=", 11))
			copy_text(state, sizeof(state), line + 11);
	}
	fclose(fp);
	if (strcmp(state, "offline") && strcmp(state, "moving"))
		return false;
	for (char *tok = strtok(on, ","); tok; tok = strtok(NULL, ","))
		if (!strcmp(tok, "bios") || !strcmp(tok, "saves") || !strcmp(tok, "states"))
			return true;
	return false;
}

/* Systems whose frames keep the H700 GPU or CPU near its limit: they keep
 * the default swap order (see NUUBOS_SWAP_WAIT_AFTER at the launch). */
static bool heavy_system(const char *system)
{
	static const char *const heavy[] = {
		"dreamcast", "naomi", "atomiswave", "n64", "psp", "nds", "saturn", NULL
	};
	int i;

	for (i = 0; heavy[i]; i++)
		if (!strcmp(system, heavy[i]))
			return true;
	return false;
}

/* Frame-rate aware CPU limit (docs/dev/perf-power.md 3.1). Up to 1032 MHz
 * the H700 runs at 900 mV, 1416 MHz needs ~1.1 V; schedutil still spent
 * 35-70 % of the time at 1416 MHz in SNES, GBA, PS1, N64 and NDS games that
 * keep their full frame rate at 1032 (CubeXX 2026-10-28). A game starts
 * capped at the lowest step and goes up a step in the first second with
 * RetroArch's audio buffer running low (the emulation falls behind real
 * time: the same signal the cores' auto frameskip uses) or with fewer
 * frames than the core's nominal rate (cores that drop frames to keep real
 * time, e.g. parallel-n64 at 480 MHz: 30 of 50 fps with the audio intact);
 * it never goes back down in a session (a needless step costs a little
 * power, a late one a visible stutter), and the game's next launch starts at
 * the step it reached (games/<game>.conf cpu_level; Reset in Game Settings
 * forgets it). Launch, state loads, resume from pause and
 * fast-forward run uncapped. power.sh applies the file as one more upper
 * limit next to the profile and the screen-off cap. */
#define GAME_CPU_FILE RUN_ROOT "/game-cpu-max"
#define GAME_CPU_LEVELS 3
/* Frames with the audio buffer below 33 % in a one-second check. */
#define GAME_CPU_LOW_FRAMES 3
/* Frames presented below this share of the core's nominal rate. */
#define GAME_CPU_MIN_FPS_RATIO 0.9
#define GAME_CPU_START_GRACE_MS 10000
#define GAME_CPU_GRACE_MS 3000

static const int game_cpu_caps[GAME_CPU_LEVELS] = { 1032000, 1200000, 0 };

/* Systems that need the full clock from the start (PSP: God of War at
 * 20 instead of 30 fps at 1032 MHz). */
static bool full_clock_system(const char *system)
{
	static const char *const full[] = {
		"psp", "dreamcast", "naomi", "atomiswave", "saturn", NULL
	};
	int i;

	for (i = 0; full[i]; i++)
		if (!strcmp(system, full[i]))
			return true;
	return false;
}

/* 0 = no game limit. */
static void game_cpu_apply(int khz)
{
	static int applied = -1;
	pid_t pid;

	if (khz == applied)
		return;
	applied = khz;
	if (khz > 0) {
		char tmp[] = GAME_CPU_FILE ".tmp";
		FILE *f = fopen(tmp, "w");

		if (!f)
			return;
		fprintf(f, "%d\n", khz);
		if (fclose(f) != 0 || rename(tmp, GAME_CPU_FILE) != 0) {
			unlink(tmp);
			return;
		}
	} else {
		unlink(GAME_CPU_FILE);
	}
	/* Reaped by the main loop like any other child. */
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
		}
		execl("/usr/sbin/nuubos-power-mode", "nuubos-power-mode", "refresh", (char *)NULL);
		_exit(127);
	}
}

/* BIOS files left in the root of the bios folder (as other systems keep
 * them) copied where the core looks (GitHub #26, nuubos-biosctl place). */
static void place_bios(const char *system)
{
	int status;
	pid_t pid = fork();

	if (pid == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
		}
		execl("/usr/bin/nuubos-biosctl", "nuubos-biosctl", "place", system, (char *)NULL);
		_exit(127);
	}
	/* Waited for here, before RetroArch starts (the main loop's reaper
	 * never sees this child first). */
	while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
}

static void game_cpu_grace(long long ms)
{
	long long until = monotonic_ms() + ms;

	if (until > session.cpu_grace_ms)
		session.cpu_grace_ms = until;
}

static void game_cpu_update(void)
{
	int khz = 0;

	if (session.state == SESSION_RUNNING && !session.menu && !session.fast_forward &&
	    monotonic_ms() >= session.cpu_grace_ms)
		khz = game_cpu_caps[session.cpu_level];
	game_cpu_apply(khz);
}

static void load_overrides(const char *user, const char *game, struct game_overrides *o);
static int save_overrides(const char *user, const char *game, const struct game_overrides *o);

/* The step the game reached, for its next launch. */
static void game_cpu_remember(void)
{
	struct game_overrides o;

	if (!session.game[0])
		return;
	load_overrides(session.user, session.game, &o);
	snprintf(o.cpu_level, sizeof(o.cpu_level), "%d", session.cpu_level);
	if (save_overrides(session.user, session.game, &o) != 0)
		log_msg("emud: saving the CPU step of %s failed", session.game);
}

/* A game below the top step is asked for its frame rate once a second. */
static bool game_cpu_watching(void)
{
	return !session.menu && session.cpu_level < GAME_CPU_LEVELS - 1;
}

/* One GET_FPS reply a second: frames presented, frames that ran with the
 * audio buffer low, the core's nominal frame rate. */
static void game_cpu_check(double fps, unsigned low, double nominal)
{
	if (session.state != SESSION_RUNNING || session.menu || session.paused ||
	    session.fast_forward || session.cpu_level >= GAME_CPU_LEVELS - 1 ||
	    monotonic_ms() < session.cpu_grace_ms)
		return;
	if (low < GAME_CPU_LOW_FRAMES && !(nominal > 0 && fps < nominal * GAME_CPU_MIN_FPS_RATIO))
		return;
	session.cpu_level++;
	game_cpu_remember();
	log_msg("emud: %s behind (%.1f of %.2f fps, %u low audio frames), CPU limit %d kHz",
		session.system, fps, nominal, low, game_cpu_caps[session.cpu_level]);
	game_cpu_grace(GAME_CPU_GRACE_MS);
	game_cpu_update();
}

/* RetroArch user_language for the user's nuubOS language. */
static int retroarch_language(const char *user)
{
	static const struct {
		const char *code;
		int retro;
	} langs[] = {
		{ "en", 0 }, { "fr", 2 }, { "es", 3 }, { "de", 4 },
		{ "it", 5 }, { "nl", 6 }, { "pt", 8 }, { "ja", 1 },
		{ "ru", 9 }, { "ko", 10 }, { "zh-tw", 11 }, { "zh-cn", 12 },
		{ "pl", 14 }, { "el", 17 }, { "tr", 18 }, { "fi", 23 },
		{ "sv", 25 }, { "uk", 26 }, { "cs", 27 }, { "hu", 31 },
		{ "nb", 34 }, { "es-419", 3 }, { "pt-br", 7 }, { "id", 24 },
		/* RetroArch has no Danish or Romanian: English. */
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
static bool list_has(const char *list, const char *name);

/* A field ("key = "value"") of a core's libretro core-info file; "" when
 * the file or the field is missing. */
static void core_info_value(const char *core, const char *key, char *out, size_t size)
{
	char path[PATH_MAX];
	char line[1024];
	size_t klen = strlen(key);
	FILE *fp;

	out[0] = '\0';
	snprintf(path, sizeof(path), CORE_INFO_DIR "/%s_libretro.info", core);
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		char *end;

		if (strncmp(p, key, klen))
			continue;
		p += klen;
		while (*p == ' ')
			p++;
		if (*p++ != '=')
			continue;
		while (*p == ' ')
			p++;
		if (*p == '"')
			p++;
		trim(p);
		if ((end = strrchr(p, '"')))
			*end = '\0';
		copy_text(out, size, p);
		break;
	}
	fclose(fp);
}

/* libretro.conf: the libretro databases of a system, "|" separated ("" =
 * none). */
static void system_databases(const char *system, char *out, size_t size)
{
	char line[1024];
	FILE *fp = fopen(LIBRETRO_CONF, "r");
	size_t len = strlen(system);

	out[0] = '\0';
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (line[0] == '#' || strncmp(line, system, len) || line[len] != '|')
			continue;
		copy_text(out, size, line + len + 1);
		break;
	}
	fclose(fp);
}

/* "a|b|c" has name. */
static bool pipe_list_has(const char *list, const char *name)
{
	size_t len = strlen(name);

	for (const char *p = list; *p;) {
		const char *bar = strchr(p, '|');
		size_t n = bar ? (size_t)(bar - p) : strlen(p);

		if (n == len && !strncmp(p, name, len))
			return true;
		if (!bar)
			break;
		p = bar + 1;
	}
	return false;
}

/* The core's core-info names one of the databases dbs ("a|b"). */
static bool core_for_databases(const char *core, const char *dbs)
{
	char info[1024];
	char *save = NULL;

	if (!dbs[0])
		return false;
	core_info_value(core, "database", info, sizeof(info));
	for (char *db = strtok_r(info, "|", &save); db; db = strtok_r(NULL, "|", &save))
		if (pipe_list_has(dbs, db))
			return true;
	return false;
}

static int core_file_filter(const struct dirent *e)
{
	size_t len = strlen(e->d_name);

	return e->d_name[0] != '.' && len > 12 && !strcmp(e->d_name + len - 12, "_libretro.so");
}

/* Cores the user downloaded for the system beyond cores.conf (Get More
 * Emulators, or RetroArch's own Online Updater): a core in
 * /userdata/retroarch/cores whose core-info names one of the system's
 * libretro databases and that nuubOS can run (core-check.h), appended to
 * out ("a,b") in name order. */
static void downloaded_cores(const char *system, char *out, size_t size)
{
	struct dirent **names = NULL;
	char dbs[512];
	int n;

	system_databases(system, dbs, sizeof(dbs));
	if (!dbs[0])
		return;
	n = scandir(DOWNLOADED_CORES, &names, core_file_filter, alphasort);
	for (int i = 0; i < n; i++) {
		char core[NAME_MAX + 1];
		char bundled[PATH_MAX];
		char path[PATH_MAX];

		copy_text(core, sizeof(core), names[i]->d_name);
		core[strlen(core) - 12] = '\0';
		snprintf(bundled, sizeof(bundled), BUNDLED_CORES "/%s_libretro.so", core);
		snprintf(path, sizeof(path), DOWNLOADED_CORES "/%s", names[i]->d_name);
		if (!list_has(out, core) && !file_exists(bundled) &&
		    core_for_databases(core, dbs) && core_runs(core, path) &&
		    strlen(out) + strlen(core) + 2 < size) {
			if (out[0])
				strcat(out, ",");
			strcat(out, core);
		}
		free(names[i]);
	}
	free(names);
}

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
				if (file_exists(path) && (r == 0 || core_runs(name, path))) {
					copy_text(core, core_size, name);
					found = true;
				}
			}
		}
	}
	fclose(fp);
	/* No core of cores.conf: one the user downloaded for the system. */
	if (!found) {
		char extra[512] = "";
		char *comma;

		downloaded_cores(system, extra, sizeof(extra));
		if ((comma = strchr(extra, ',')))
			*comma = '\0';
		if (extra[0]) {
			copy_text(core, core_size, extra);
			snprintf(path, path_size, DOWNLOADED_CORES "/%s_libretro.so", extra);
			resolved_machine[0] = '\0';
			resolved_media[0] = '\0';
			found = true;
		}
	}
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

static void load_overrides_at(const char *path, struct game_overrides *o)
{
	char line[PATH_MAX + 32];
	FILE *fp;

	memset(o, 0, sizeof(*o));
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
		else if (!strcmp(line, "accuracy"))
			copy_text(o->accuracy, sizeof(o->accuracy), eq);
		else if (!strcmp(line, "cheat_file"))
			copy_text(o->cheat_file, sizeof(o->cheat_file), eq);
		else if (!strcmp(line, "cheats"))
			copy_text(o->cheats, sizeof(o->cheats), eq);
		else if (!strcmp(line, "nds_layout"))
			copy_text(o->nds_layout, sizeof(o->nds_layout), eq);
		else if (!strcmp(line, "stylus"))
			copy_text(o->stylus, sizeof(o->stylus), eq);
		else if (!strcmp(line, "nds_corner"))
			copy_text(o->nds_corner, sizeof(o->nds_corner), eq);
		else if (!strcmp(line, "n64_pak"))
			copy_text(o->n64_pak, sizeof(o->n64_pak), eq);
		else if (!strcmp(line, "resolution"))
			copy_text(o->resolution, sizeof(o->resolution), eq);
		else if (!strcmp(line, "frameskip"))
			copy_text(o->frameskip, sizeof(o->frameskip), eq);
		else if (!strcmp(line, "cpu_level"))
			copy_text(o->cpu_level, sizeof(o->cpu_level), eq);
		else if (!strncmp(line, "remap_", 6) && remap_index(line + 6) >= 0)
			copy_text(o->remap[remap_index(line + 6)], sizeof(o->remap[0]), eq);
	}
	fclose(fp);
}

static void load_overrides(const char *user, const char *game, struct game_overrides *o)
{
	char path[PATH_MAX];

	overrides_path(user, game, path, sizeof(path));
	load_overrides_at(path, o);
}

static void system_overrides_path(const char *user, const char *system, char *out, size_t size)
{
	snprintf(out, size, USERS_DIR "/%s/" SYSTEM_SETTINGS_SUBDIR "/%s.conf", user, system);
}

static void load_system_overrides(const char *user, const char *system, struct game_overrides *o)
{
	char path[PATH_MAX];

	system_overrides_path(user, system, path, sizeof(path));
	load_overrides_at(path, o);
}

/* The keys a system can set (System Settings, Controls); the rest are the
 * game's own (layout, stylus, cheats, CPU step). */
static bool system_keys_set(const struct game_overrides *o)
{
	return o->core[0] || o->aspect[0] || o->filter[0] || o->accuracy[0] || o->n64_pak[0] || o->resolution[0] ||
	       o->frameskip[0] || remap_set(o);
}

static void clear_system_keys(struct game_overrides *o)
{
	o->core[0] = o->aspect[0] = o->filter[0] = o->accuracy[0] = o->n64_pak[0] = o->resolution[0] = '\0';
	o->frameskip[0] = '\0';
	memset(o->remap, 0, sizeof(o->remap));
}

/* What a game inherits: its own value, else its system's. */
static void inherit_system(struct game_overrides *o, const struct game_overrides *sys)
{
	if (!o->core[0])
		copy_text(o->core, sizeof(o->core), sys->core);
	if (!o->aspect[0])
		copy_text(o->aspect, sizeof(o->aspect), sys->aspect);
	if (!o->filter[0])
		copy_text(o->filter, sizeof(o->filter), sys->filter);
	if (!o->accuracy[0])
		copy_text(o->accuracy, sizeof(o->accuracy), sys->accuracy);
	if (!o->n64_pak[0])
		copy_text(o->n64_pak, sizeof(o->n64_pak), sys->n64_pak);
	if (!o->resolution[0])
		copy_text(o->resolution, sizeof(o->resolution), sys->resolution);
	if (!o->frameskip[0])
		copy_text(o->frameskip, sizeof(o->frameskip), sys->frameskip);
	for (int i = 0; i < REMAP_COUNT; i++)
		if (!o->remap[i][0])
			copy_text(o->remap[i], sizeof(o->remap[i]), sys->remap[i]);
}

static int save_overrides_at(const char *dir, const char *path, const struct game_overrides *o)
{
	char tmp[PATH_MAX + 8];
	FILE *fp;
	int rc = 0;

	/* Nothing overridden: no file, everything is inherited. */
	if (!o->core[0] && !o->aspect[0] && !o->filter[0] && !o->accuracy[0] &&
	    !o->cheat_file[0] && !o->cheats[0] && !o->nds_layout[0] && !o->stylus[0] && !o->nds_corner[0] &&
	    !o->n64_pak[0] && !o->resolution[0] && !o->frameskip[0] && !o->cpu_level[0] && !remap_set(o))
		return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
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
	if (o->accuracy[0])
		fprintf(fp, "accuracy=%s\n", o->accuracy);
	if (o->cheat_file[0])
		fprintf(fp, "cheat_file=%s\n", o->cheat_file);
	if (o->cheats[0])
		fprintf(fp, "cheats=%s\n", o->cheats);
	if (o->nds_layout[0])
		fprintf(fp, "nds_layout=%s\n", o->nds_layout);
	if (o->stylus[0])
		fprintf(fp, "stylus=%s\n", o->stylus);
	if (o->nds_corner[0])
		fprintf(fp, "nds_corner=%s\n", o->nds_corner);
	if (o->n64_pak[0])
		fprintf(fp, "n64_pak=%s\n", o->n64_pak);
	if (o->resolution[0])
		fprintf(fp, "resolution=%s\n", o->resolution);
	if (o->frameskip[0])
		fprintf(fp, "frameskip=%s\n", o->frameskip);
	if (o->cpu_level[0])
		fprintf(fp, "cpu_level=%s\n", o->cpu_level);
	for (int i = 0; i < REMAP_COUNT; i++)
		if (o->remap[i][0])
			fprintf(fp, "remap_%s=%s\n", remap_controls[i], o->remap[i]);
	if (ferror(fp))
		rc = -1;
	if (close_synced(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

static int save_overrides(const char *user, const char *game, const struct game_overrides *o)
{
	char path[PATH_MAX];
	char dir[PATH_MAX];

	overrides_path(user, game, path, sizeof(path));
	snprintf(dir, sizeof(dir), USERS_DIR "/%s/" GAME_SETTINGS_SUBDIR, user);
	return save_overrides_at(dir, path, o);
}

static int save_system_overrides(const char *user, const char *system, const struct game_overrides *o)
{
	char path[PATH_MAX];
	char dir[PATH_MAX];

	system_overrides_path(user, system, path, sizeof(path));
	snprintf(dir, sizeof(dir), USERS_DIR "/%s/" SYSTEM_SETTINGS_SUBDIR, user);
	return save_overrides_at(dir, path, o);
}

static bool valid_aspect(const char *v)
{
	return !strcmp(v, "core") || !strcmp(v, "4:3") || !strcmp(v, "16:9") ||
	       !strcmp(v, "full") || !strcmp(v, "integer") || !strcmp(v, "square");
}

static bool valid_filter(const char *v)
{
	return !strcmp(v, "sharp") || !strcmp(v, "smooth") || !strcmp(v, "pixel");
}

/* Cores of a system that can be loaded (bundled or downloaded), in
 * cores.conf preference order, then the other cores the user downloaded
 * for it, as "a,b,c". */
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
				ok = file_exists(path) && (r == 0 || core_runs(name, path));
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
	downloaded_cores(system, out, size);
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
	e->bezels = true; /* user decision 2026-10-15 */
	e->crt = true;
	copy_text(e->position, sizeof(e->position), "center");
	copy_text(e->overlay_items, sizeof(e->overlay_items), DEFAULT_OVERLAY_ITEMS);
	for (int i = 0; i < HK_COUNT; i++)
		copy_text(e->hotkey[i], sizeof(e->hotkey[i]), hotkey_defaults[i]);
}

/* Applies key=value to e; false when the key or value is not valid. */
static bool settings_set(struct emu_settings *e, const char *key, const char *v)
{
	bool flag = !strcmp(v, "1");

	if (!strcmp(key, "boot_screens") || !strcmp(key, "slot_rotation") ||
	    !strcmp(key, "bezels") || !strcmp(key, "crt") || !strcmp(key, "overlay") ||
	    !strcmp(key, "retroarch_menu")) {
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
		else if (!strcmp(key, "retroarch_menu"))
			e->retroarch_menu = flag;
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
	if (!strcmp(key, "position")) {
		if (strcmp(v, "top") && strcmp(v, "center") && strcmp(v, "bottom"))
			return false;
		copy_text(e->position, sizeof(e->position), v);
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
			 "position=%s\noverlay=%d\noverlay_items=%s\nretroarch_menu=%d\n",
			 e->boot_screens, e->slots, e->slot_rotation, e->bezels, e->crt,
			 e->position, e->overlay, e->overlay_items, e->retroarch_menu);

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
	if (close_synced(fp) != 0)
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
	/* The game's Controls ("REMAP" alone = every control is itself). */
	snprintf(line, sizeof(line), "REMAP %s\n", session.remap);
	if (write_all(session.input_fd, line, strlen(line)) != 0)
		log_msg("emud: controls to inputd failed");
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
static bool hdmi_active(void)
{
	char value[16] = "";
	FILE *fp = fopen(HDMI_ENABLED, "r");

	if (!fp)
		return false;
	if (!fgets(value, sizeof(value), fp))
		value[0] = '\0';
	fclose(fp);
	trim(value);
	return strcmp(value, "enabled") == 0;
}

static int game_render_height(const char *system)
{
	char value[16] = "";

	if (!hdmi_active())
		return 0;
	display_field(system, 2, value, sizeof(value));
	return value[0] ? atoi(value) : GAME_HDMI_LINES;
}

/* The active output is a square built-in panel (RG CubeXX 720x720): an
 * enabled non-HDMI connector whose first (preferred) mode is WxW. */
static bool square_panel(void)
{
	DIR *dir = opendir(DRM_CLASS);
	struct dirent *e;
	bool square = false;

	if (!dir)
		return false;
	while (!square && (e = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		char value[32] = "";
		unsigned w = 0, h = 0;
		FILE *fp;

		if (strncmp(e->d_name, "card0-", 6) || strstr(e->d_name, "HDMI"))
			continue;
		snprintf(path, sizeof(path), DRM_CLASS "/%s/enabled", e->d_name);
		if (!(fp = fopen(path, "r")))
			continue;
		if (!fgets(value, sizeof(value), fp))
			value[0] = '\0';
		fclose(fp);
		trim(value);
		if (strcmp(value, "enabled"))
			continue;
		snprintf(path, sizeof(path), DRM_CLASS "/%s/modes", e->d_name);
		if (!(fp = fopen(path, "r")))
			continue;
		if (fscanf(fp, "%ux%u", &w, &h) == 2 && w && w == h)
			square = true;
		fclose(fp);
	}
	closedir(dir);
	return square;
}

/* A TV system's picture fills a square handheld screen's height as far as
 * at most a share of its width (a TV's overscan) is cropped (RetroArch patch
 * 0006, NUUBOS_RA_OVERSCAN_X): with the default 8 %
 * (4 % a side) a 4:3 game on the 720x720 RG CubeXX is 783x587, not an
 * integer 2x with large margins (user decisions 2026-10-11: square screens
 * only, never HDMI or 4:3/3:2 panels; integer 3x cropped PS1 too much).
 * display.conf field 4 sets the share per system (PS1 5 %, user request
 * 2026-10-14: 8 % cut its borders a little too much). Only with the default
 * presentation (or the per-game Integer Scaling). 0 crops nothing: the
 * picture is as wide as the screen, any margin at the bottom (Dreamcast,
 * NAOMI, Atomiswave: user request 2026-10-21, their games use the whole
 * 640 pixels). Since 2026-10-22 0 is the default for every system (user:
 * "too much overscan on Saturn too, remove it from all cores"); a
 * display.conf field 4 can still set a share for one system. Returns the
 * percentage of the width that may be cropped, -1 = off. */
#define TV_OVERSCAN_DEFAULT_PCT 0

static int tv_overscan(const char *system, const struct game_overrides *o)
{
	char screen[16], pct[16];
	int value;

	if (o->aspect[0] && strcmp(o->aspect, "integer"))
		return -1;
	if (!square_panel()) /* HDMI turns the panel off */
		return -1;
	system_screen(system, screen, sizeof(screen));
	if (strcmp(screen, "crt"))
		return -1;
	display_field(system, 3, pct, sizeof(pct));
	if (!pct[0])
		return TV_OVERSCAN_DEFAULT_PCT;
	value = atoi(pct);
	return value >= 0 && value < 50 ? value : TV_OVERSCAN_DEFAULT_PCT;
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

/* The Mood colours of the bezel (logo, matte): the lifecycle splash cache
 * /etc/nuubos/splash.palette, written by nuubos-themectl for the active
 * user; built-in nuubOS colours without it. */
struct mood_colours {
	float background[3], text[3], accent[3];
};

static void set_hex_rgb(float out[3], const char *v)
{
	unsigned int r, g, b;

	if (sscanf(v, "#%02x%02x%02x", &r, &g, &b) == 3) {
		out[0] = (float)r / 255.0f;
		out[1] = (float)g / 255.0f;
		out[2] = (float)b / 255.0f;
	}
}

static void mood_colours(struct mood_colours *m)
{
	char line[128];
	FILE *fp;

	set_hex_rgb(m->background, "#0d0e10");
	set_hex_rgb(m->text, "#f5f6f8");
	set_hex_rgb(m->accent, "#3a86ff");
	fp = fopen("/etc/nuubos/splash.palette", "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		if (!strncmp(line, "background=", 11))
			set_hex_rgb(m->background, line + 11);
		else if (!strncmp(line, "text=", 5))
			set_hex_rgb(m->text, line + 5);
		else if (!strncmp(line, "accent=", 7))
			set_hex_rgb(m->accent, line + 7);
	}
	fclose(fp);
}

/* Boot sequence: the core options that skip or show the console's own
 * start-up (BIOS logo), set at every launch from the user's setting. */
static const struct {
	const char *key;
	const char *on;
	const char *off;
	/* Files the start-up needs in the BIOS folder ("" = none). */
	const char *const needs[3];
} boot_options[] = {
	{ "pcsx_rearmed_show_bios_bootlogo", "enabled", "disabled", { "" } },
	{ "mgba_skip_bios", "OFF", "ON", { "" } },
	{ "gambatte_gb_bootloader", "enabled", "disabled", { "" } },
	{ "genesis_plus_gx_bios", "enabled", "disabled", { "" } },
	/* melonDS boots the console's menu only from the real BIOS and
	 * firmware; with its FreeBIOS that start-up never reaches the game
	 * (white screen, 2026-10-25 on the CubeXX with the boot screens on). */
	{ "melonds_boot_directly", "disabled", "enabled", { "bios7.bin", "bios9.bin", "firmware.bin" } },
};

/* The console start-up for option i: wanted, and possible. */
static bool boot_option_on(size_t i, bool boot)
{
	char path[PATH_MAX];

	if (!boot)
		return false;
	for (size_t k = 0; k < 3 && boot_options[i].needs[k] && boot_options[i].needs[k][0]; k++) {
		snprintf(path, sizeof(path), BIOS_DIR "/%s", boot_options[i].needs[k]);
		if (!file_exists(path))
			return false;
	}
	return true;
}

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
				boot_option_on(i, boot) ? boot_options[i].on : boot_options[i].off);
		done[i] = true;
	}
	for (size_t i = 0; i < sizeof(boot_options) / sizeof(boot_options[0]); i++)
		if (!done[i])
			fprintf(out, "%s = \"%s\"\n", boot_options[i].key,
				boot_option_on(i, boot) ? boot_options[i].on : boot_options[i].off);
	fclose(in);
	if (close_synced(out) != 0 || rename(tmp, path) != 0)
		unlink(tmp);
}

/* ------------------------------------------------------------------ */
/* RetroAchievements (EPIC-017)                                        */
/* ------------------------------------------------------------------ */

struct achievements {
	bool enabled;
	bool hardcore;
	bool offline;
	/* RAOfflineProxy is up for this session: RetroArch talks to it. */
	bool proxy;
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
	a->offline = true;
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
		else if (!strcmp(line, "OFFLINE"))
			a->offline = !strcmp(eq, "1");
	}
	fclose(fp);
	if (!a->username[0] || !a->token[0] || !cfg_value_safe(a->username) ||
	    !cfg_value_safe(a->token))
		a->enabled = false;
}

/* ------------------------------------------------------------------ */
/* Offline achievements (RAOfflineProxy, GitHub #18)                    */
/* ------------------------------------------------------------------ */

/*
 * RAOfflineProxy (GPL-3.0, its own process) sits between RetroArch and
 * retroachievements.org for casual sessions of a user with Offline
 * Achievements on (default): online it forwards and caches each game's
 * data, offline it serves the cache and queues the unlocks, which it sends
 * (backdated) once the server answers again. Hardcore never goes through
 * it. Started at the first such launch and left running so the queue can
 * drain after the game; restarted for another user or new credentials,
 * stopped when not wanted. Its data (cache, queue, login) is the user's,
 * in STATE: /state/users/<id>/secrets/raofflineproxy. RetroArch's
 * credentials are in the session config only, so the proxy gets its own
 * root-only copy in /run.
 */
#define RAPROXY_PORT 47380
#define RAPROXY_RUN "/run/nuubos/raproxy"
#define RAPROXY_CFG RAPROXY_RUN "/retroarch.cfg"

static pid_t raproxy_pid = -1;
static char raproxy_user[64];

static void raproxy_kill(pid_t pid)
{
	kill(pid, SIGTERM);
	for (int i = 0; i < 40 && waitpid(pid, NULL, WNOHANG) == 0; i++)
		usleep(50000);
	if (waitpid(pid, NULL, WNOHANG) == 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}
}

static void raproxy_stop(void)
{
	if (raproxy_pid > 0) {
		raproxy_kill(raproxy_pid);
		log_msg("emud: offline achievements proxy stopped");
	}
	raproxy_pid = -1;
	raproxy_user[0] = '\0';
	unlink(RAPROXY_RUN "/pid");
}

/* One left by an earlier nuubos-emud (restarted): its pid file. */
static void raproxy_stop_stale(void)
{
	char path[64], cmd[256] = "";
	FILE *fp = fopen(RAPROXY_RUN "/pid", "r");
	long pid = 0;
	size_t n;

	if (!fp)
		return;
	if (fscanf(fp, "%ld", &pid) != 1)
		pid = 0;
	fclose(fp);
	unlink(RAPROXY_RUN "/pid");
	snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
	if (pid <= 1 || !(fp = fopen(path, "r")))
		return;
	n = fread(cmd, 1, sizeof(cmd) - 1, fp);
	fclose(fp);
	for (size_t i = 0; i < n; i++)
		if (!cmd[i])
			cmd[i] = ' ';
	cmd[n] = '\0';
	if (strstr(cmd, "raofflineproxy"))
		kill((pid_t)pid, SIGTERM);
}

static bool raproxy_listening(void)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(RAPROXY_PORT) };
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	bool ok;

	if (fd < 0)
		return false;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
	close(fd);
	return ok;
}

/* The credentials file the proxy reads (root only); *changed when it was
 * different (a new login). */
static bool raproxy_write_cfg(const struct achievements *a, bool *changed)
{
	char text[512], old[512] = "";
	FILE *fp;
	size_t n;

	snprintf(text, sizeof(text),
		 "cheevos_enable = \"true\"\ncheevos_username = \"%s\"\ncheevos_token = \"%s\"\n"
		 "cheevos_hardcore_mode_enable = \"false\"\n", a->username, a->token);
	if ((fp = fopen(RAPROXY_CFG, "r"))) {
		n = fread(old, 1, sizeof(old) - 1, fp);
		old[n] = '\0';
		fclose(fp);
	}
	*changed = strcmp(old, text) != 0;
	if (!*changed)
		return true;
	if (mkdir(RAPROXY_RUN, 0700) != 0 && errno != EEXIST)
		return false;
	umask(077);
	fp = fopen(RAPROXY_CFG ".tmp", "w");
	umask(022);
	if (!fp)
		return false;
	fputs(text, fp);
	return fclose(fp) == 0 && rename(RAPROXY_CFG ".tmp", RAPROXY_CFG) == 0;
}

/* Up for this user's casual session, or stopped when not wanted. True when
 * RetroArch should talk to it. */
static bool raproxy_ensure(const char *user, const struct achievements *a)
{
	char dir[PATH_MAX], path[PATH_MAX];
	bool changed = false;
	FILE *fp;
	pid_t pid;

	if (raproxy_pid < 0)
		raproxy_stop_stale();
	if (!a->enabled || a->hardcore || !a->offline || access("/usr/bin/python3", X_OK) != 0) {
		raproxy_stop();
		unlink(RAPROXY_CFG);
		return false;
	}
	if (raproxy_pid > 0 && (strcmp(raproxy_user, user) || waitpid(raproxy_pid, NULL, WNOHANG) != 0))
		raproxy_stop();
	if (!raproxy_write_cfg(a, &changed)) {
		raproxy_stop();
		return false;
	}
	if (raproxy_pid > 0 && !changed && raproxy_listening())
		return true;
	raproxy_stop();
	snprintf(dir, sizeof(dir), STATE_USERS "/%s/secrets/raofflineproxy", user);
	if (mkdir_p(dir) != 0)
		return false;
	chmod(dir, 0700);
	/* Its own settings: this port, these credentials, never usage reports
	 * (the uploads are also taken out of the nuubOS build). */
	snprintf(path, sizeof(path), "%s/config.json", dir);
	if ((fp = fopen(path, "w"))) {
		fprintf(fp, "{\"proxy_host\": \"127.0.0.1\", \"proxy_port\": %d, \"retroarch_cfg\": \"%s\", "
			    "\"usage_report_url\": \"\", \"usage_stats_consent\": false}\n", RAPROXY_PORT, RAPROXY_CFG);
		fclose(fp);
	}
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		setsid();
		if (null >= 0) {
			dup2(null, 0);
			dup2(null, 1);
			dup2(null, 2);
		}
		/* Never in the way of the game. */
		setpriority(PRIO_PROCESS, 0, 10);
		setenv("RAOFFLINEPROXY_CONFIG_DIR", dir, 1);
		setenv("RAOFFLINEPROXY_RETROARCH_CFG", RAPROXY_CFG, 1);
		setenv("HOME", dir, 1);
		execl("/usr/bin/python3", "python3", "-m", "raofflineproxy.main", "run-service", (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		return false;
	raproxy_pid = pid;
	copy_text(raproxy_user, sizeof(raproxy_user), user);
	if ((fp = fopen(RAPROXY_RUN "/pid", "w"))) {
		fprintf(fp, "%d\n", (int)pid);
		fclose(fp);
	}
	/* Python starts in about a second on the H700. */
	for (int i = 0; i < 100; i++) {
		if (raproxy_listening()) {
			log_msg("emud: offline achievements proxy up for %s", user);
			return true;
		}
		if (waitpid(pid, NULL, WNOHANG) != 0) {
			raproxy_pid = -1;
			break;
		}
		usleep(50000);
	}
	log_msg("emud: offline achievements proxy did not start, direct connection");
	raproxy_stop();
	return false;
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
	if (close_synced(out) == 0 && rename(tmp, session.ra_cfg) == 0)
		log_msg("emud: restored %d user RetroArch keys", session.restore_count);
	else
		unlink(tmp);
	free_restore();
}

/* Shader preset of the session: one pass. */
static int write_session_shader(const char *shader)
{
	char tmp[] = SESSION_SHADER ".tmp";
	FILE *fp = fopen(tmp, "w");
	int rc;

	if (!fp)
		return -1;
	fprintf(fp, "shaders = \"1\"\nshader0 = \"%s\"\nfilter_linear0 = \"false\"\n", shader);
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
 * horizontally and, when lower than the screen, at the user's Picture
 * Position (top, center or bottom; RetroArch patch 0003 still crops an
 * overscaled picture evenly); the per-game "integer" aspect is the same
 * picture without bezels. "square" is integer scaling with square pixels
 * (8:7 for a 256-pixel-wide NES/SNES picture, GitHub #15): RetroArch
 * patch 0014 lets it take the next vertical factor when that crops at
 * most 8 of the game's pixels a side (SNES 3x on 720x720 = 768x672, 720
 * wide). A bezel
 * leaves the picture as it is: RetroArch (patch 0010) draws it into the
 * rest of the screen from NUUBOS_RA_BEZEL (session.bezel_env); the CRT
 * look applies to TV systems (display.conf) unless
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
	const char *bias_y = !strcmp(session.settings.position, "top") ? "0.000000"
			   : !strcmp(session.settings.position, "bottom") ? "1.000000"
			   : "0.500000";

	for (int i = 0; keys[i]; i++)
		remember_key(keys[i]);
	system_screen(session.system, screen, sizeof(screen));
	crt = session.settings.crt && !strcmp(screen, "crt") && !o->filter[0];
	session.pixel_crop = !strcmp(o->aspect, "square");
	if (session.pixel_crop) {
		index = 21; /* square pixels */
	} else if (o->aspect[0] && strcmp(o->aspect, "integer")) {
		index = !strcmp(o->aspect, "4:3") ? 0
		      : !strcmp(o->aspect, "16:9") ? 1
		      : !strcmp(o->aspect, "full") ? 24
		      : 22; /* core */
		integer = false;
	} else if (!strcmp(o->aspect, "integer")) {
		/* the default presentation, without bezels */
	} else if (session.settings.bezels) {
		bezel = true;
	}
	fprintf(fp,
		"aspect_ratio_index = \"%d\"\n"
		"video_scale_integer = \"%s\"\n"
		"video_scale_integer_scaling = \"2\"\n"
		"video_scale_integer_axis = \"0\"\n"
		"video_viewport_bias_x = \"0.500000\"\n"
		"video_viewport_bias_y = \"%s\"\n"
		"video_viewport_bias_portrait_x = \"0.500000\"\n"
		"video_viewport_bias_portrait_y = \"%s\"\n"
		"video_smooth = \"%s\"\n",
		index, integer ? "true" : "false", bias_y, bias_y,
		!strcmp(o->filter, "smooth") ? "true" : "false");

	shader[0] = '\0';
	session.bezel_env[0] = '\0';
	if (bezel) {
		char icon[PATH_MAX];
		struct mood_colours mood;
		float rgb[3];

		system_colour(session.system, rgb);
		mood_colours(&mood);
		snprintf(icon, sizeof(icon), SYSTEM_ICONS "/%s.png", session.system);
		if (!file_exists(icon))
			copy_text(icon, sizeof(icon), "-");
		snprintf(session.bezel_env, sizeof(session.bezel_env),
			 "%.3f %.3f %.3f %d %s " SHADER_DIR "/bezel-logo.png "
			 "%.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
			 rgb[0], rgb[1], rgb[2], !strcmp(screen, "lcd") ? 1 : 0, icon,
			 mood.background[0], mood.background[1], mood.background[2],
			 mood.text[0], mood.text[1], mood.text[2],
			 mood.accent[0], mood.accent[1], mood.accent[2]);
	}
	/* One CRT strength everywhere, HDMI below 720 lines too: the stronger
	 * 0.55 used there since 2026-10-17 answered scanlines that were missing
	 * because crt.glsl dropped them at exactly 2x (user, 2026-10-24: 0.25 at
	 * 540 lines "ora meglio decisamente"). */
	if (crt) {
		if (write_session_shader(SHADER_DIR "/crt.glsl") == 0)
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
	remember_key("cheevos_custom_host");
	if (a->enabled) {
		fprintf(fp,
			"cheevos_enable = \"true\"\n"
			"cheevos_username = \"%s\"\n"
			"cheevos_token = \"%s\"\n"
			"cheevos_password = \"\"\n"
			"cheevos_hardcore_mode_enable = \"%s\"\n",
			a->username, a->token, a->hardcore ? "true" : "false");
		/* Offline achievements: through the local proxy (casual only). */
		if (a->proxy && !a->hardcore)
			fprintf(fp, "cheevos_custom_host = \"127.0.0.1:%d\"\n", RAPROXY_PORT);
		else
			fprintf(fp, "cheevos_custom_host = \"\"\n");
	} else {
		fprintf(fp, "cheevos_enable = \"false\"\n"
			    "cheevos_token = \"\"\n"
			    "cheevos_password = \"\"\n");
	}
}

/* Netplay: the user's nuubOS name, the local network only (no public
 * lobby, relay or UPnP), the default port, everyone plays. */
static void write_session_netplay(FILE *fp)
{
	static const char *const keys[] = {
		"netplay_nickname", "netplay_public_announce", "netplay_use_mitm_server",
		"netplay_nat_traversal", "netplay_start_as_spectator", "netplay_password",
		"netplay_spectate_password", "netplay_ip_port", NULL,
	};
	char path[PATH_MAX];
	char line[256];
	char nick[32] = "nuubOS";
	FILE *profile;

	snprintf(path, sizeof(path), STATE_USERS "/%s/" PROFILE_FILE, session.user);
	if ((profile = fopen(path, "r"))) {
		while (fgets(line, sizeof(line), profile)) {
			trim(line);
			if (strncmp(line, "USER_NAME=", 10) || !line[10] || !cfg_value_safe(line + 10))
				continue;
			copy_text(nick, sizeof(nick), line + 10);
			/* A name cut to RetroArch's 31 bytes never ends in half a
			 * UTF-8 character. */
			if (strlen(line + 10) >= sizeof(nick)) {
				size_t n = strlen(nick);

				while (n > 0 && ((unsigned char)nick[n - 1] & 0xc0) == 0x80)
					n--;
				if (n > 0 && ((unsigned char)nick[n - 1] & 0xc0) == 0xc0)
					n--;
				nick[n] = '\0';
			}
		}
		fclose(profile);
	}
	for (int i = 0; keys[i]; i++)
		remember_key(keys[i]);
	fprintf(fp,
		"netplay_nickname = \"%s\"\n"
		"netplay_public_announce = \"false\"\n"
		"netplay_use_mitm_server = \"false\"\n"
		"netplay_nat_traversal = \"false\"\n"
		"netplay_start_as_spectator = \"false\"\n"
		"netplay_password = \"\"\n"
		"netplay_spectate_password = \"\"\n"
		"netplay_ip_port = \"%d\"\n",
		nick, NETPLAY_PORT);
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
static bool save_next(bool quiet)
{
	int target = save_target();

	if (!begin_save(target, quiet))
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
static void request_quit(void);

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
	if (kind == PENDING_LOAD && ok)
		game_cpu_grace(GAME_CPU_GRACE_MS);
	if (kind == PENDING_SAVE)
		log_msg("emud: save slot %d %s", slot, ok ? "written" : "failed");
	finish_power_wait();
	if (session.quit_requested)
		request_quit();
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
	if (!strcmp(line, "CHEATS_LOAD FAILED") || !strcmp(line, "NETPLAY_STOP FAILED") ||
	    !strcmp(line, "SET_CORE_OPTION FAILED")) {
		log_msg("emud: retroarch refused %s", line);
		return;
	}
	if (!strncmp(line, "NETPLAY ", 8)) {
		/* Netplay state (RetroArch patch 0008). */
		const char *ev = line + 8;
		const char *nick = strchr(ev, ' ');

		nick = nick ? nick + 1 : "";
		if (!strcmp(ev, "hosting")) {
			session.netplay = NET_HOST;
			session.net_peers = 0;
			session.net_peer[0] = '\0';
		} else if (!strncmp(ev, "joined", 6)) {
			session.net_peers++;
			copy_text(session.net_peer, sizeof(session.net_peer), nick);
		} else if (!strncmp(ev, "left", 4)) {
			if (session.net_peers > 0)
				session.net_peers--;
		} else if (!strncmp(ev, "connected", 9)) {
			session.netplay = NET_CLIENT;
			session.net_peers = 1;
			copy_text(session.net_peer, sizeof(session.net_peer), nick);
		} else if (!strcmp(ev, "disconnected") && session.netplay == NET_CLIENT) {
			session.netplay = NET_OFF;
			session.net_launch[0] = '\0';
		} else if (!strcmp(ev, "off") || !strcmp(ev, "failed")) {
			session.netplay = NET_OFF;
			session.net_launch[0] = '\0';
			session.net_peers = 0;
		}
		log_msg("emud: netplay %s", ev);
		notify_subscribers();
		return;
	}
	{
		unsigned index, count;
		int ejected;

		if (sscanf(line, "GET_DISK %u %u %d", &index, &count, &ejected) == 3) {
			/* While a change is under way the chosen disc stays shown. */
			if (!session.disc_insert_ms)
				session.disc = (int)index;
			session.discs = (int)count;
			notify_subscribers();
			return;
		}
	}
	if (!strncmp(line, "SET_DISK_", 9) && strstr(line, "FAILED")) {
		log_msg("emud: retroarch refused %s", line);
		return;
	}
	if (!strncmp(line, "GET_FPS ", 8)) {
		/* "GET_FPS <fps> <frames with the audio buffer low> <nominal
		 * fps>" (RetroArch patches 0003 and 0015). The snapshot changes
		 * only for the overlay: no Quick Menu wakeup a second otherwise. */
		double fps = 0, nominal = 0;
		unsigned low = 0;

		if (sscanf(line + 8, "%lf %u %lf", &fps, &low, &nominal) >= 1 &&
		    session.settings.overlay) {
			session.fps = fps;
			notify_subscribers();
		}
		game_cpu_check(fps, low, nominal);
		return;
	}
	if (sscanf(line, "SET_PAUSED %d", &slot) == 1) {
		bool paused = slot != 0;

		if (paused != session.paused) {
			session.paused = paused;
			if (!paused)
				game_cpu_grace(GAME_CPU_GRACE_MS);
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

/* ------------------------------------------------------------------ */
/* Cheats                                                              */
/* ------------------------------------------------------------------ */

/*
 * A game's cheats are a .cht file of the user's (/userdata/cheats) or of
 * the libretro cheat collection (nuubos-libretroctl: an archive and its
 * index; the game's files are extracted when its cheats are listed into
 * /userdata/cheats/libretro/<database>). The user picks the list and turns cheats on per
 * game (game overrides cheat_file/cheats); the session gets a copy of the
 * list with those cheats on (session.cht) through RetroArch's CHEATS_LOAD
 * (patch 0008), at launch and at every change. No cheats in hardcore mode
 * or with netplay (RetroArch refuses them there).
 */
struct cheat_list {
	char path[PATH_MAX];
	char name[NAME_MAX + 1];
	int rank;
};

/* A file name's title for matching: letters and digits in lower case,
 * without its (...) [...] {...} tags. */
static void title_key(const char *name, char *out, size_t size)
{
	int depth = 0;
	size_t n = 0;

	for (const char *p = name; *p && n + 1 < size; p++) {
		if (*p == '(' || *p == '[' || *p == '{')
			depth++;
		else if ((*p == ')' || *p == ']' || *p == '}') && depth)
			depth--;
		else if (!depth && isalnum((unsigned char)*p))
			out[n++] = (char)tolower((unsigned char)*p);
	}
	out[n] = '\0';
}

/* First word of a name's first (...) tag: the region ("USA"). */
static void region_tag(const char *name, char *out, size_t size)
{
	const char *open = strchr(name, '(');
	size_t n = 0;

	out[0] = '\0';
	if (!open)
		return;
	for (const char *p = open + 1; *p && *p != ')' && *p != ',' && n + 1 < size; p++)
		out[n++] = *p;
	out[n] = '\0';
}

/* Inserts a list by rank, then name, keeping at most MAX_CHEAT_LISTS. */
static void add_cheat_list(struct cheat_list *lists, int *count, const char *path,
			   const char *name, int rank)
{
	int at = *count;

	for (int i = 0; i < *count; i++)
		if (!strcmp(lists[i].path, path))
			return;
	while (at > 0 && (lists[at - 1].rank > rank ||
			  (lists[at - 1].rank == rank && strcasecmp(lists[at - 1].name, name) > 0)))
		at--;
	if (at >= MAX_CHEAT_LISTS)
		return;
	if (*count < MAX_CHEAT_LISTS)
		(*count)++;
	memmove(&lists[at + 1], &lists[at], (size_t)(*count - 1 - at) * sizeof(*lists));
	copy_text(lists[at].path, sizeof(lists[at].path), path);
	copy_text(lists[at].name, sizeof(lists[at].name), name);
	lists[at].rank = rank;
}

/* Whether a name has a word (letters and digits between other characters),
 * case-insensitive. */
static bool has_word(const char *name, const char *word)
{
	size_t len = strlen(word);

	for (const char *p = name; *p; p++) {
		if (strncasecmp(p, word, len) || (p > name && isalnum((unsigned char)p[-1])) ||
		    isalnum((unsigned char)p[len]))
			continue;
		return true;
	}
	return false;
}

/* Whether a list is of the ROM's region: the region's name or its short
 * forms ("(Europe)", "[EU]"; the PSP lists use the short ones). */
static bool same_region(const char *name, const char *region)
{
	static const char *const aliases[][4] = {
		{ "Europe", "EU", "EUR", NULL }, { "USA", "US", NULL },
		{ "Japan", "JP", "JPN", NULL }, { "Korea", "KR", NULL },
		{ "China", "CN", NULL }, { "Asia", "AS", NULL },
	};

	if (!region[0])
		return false;
	if (has_word(name, region))
		return true;
	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
		if (strcasecmp(aliases[i][0], region))
			continue;
		for (int k = 1; aliases[i][k]; k++)
			if (has_word(name, aliases[i][k]))
				return true;
	}
	return false;
}

/* Whether a list is of another release than the ROM (a demo, a beta, the
 * PlayStation Store version of a disc): its addresses differ. */
static bool other_release(const char *name, const char *stem)
{
	static const char *const tags[] = { "Demo", "Kiosk", "Beta", "Proto", "Prototype", "Sample" };

	for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
		if (has_word(name, tags[i]) && !has_word(stem, tags[i]))
			return true;
	return strcasestr(name, "PlayStation Store") && !has_word(stem, "PSN") &&
	       !strcasestr(stem, "PlayStation Store");
}

/* Rank of a list for the ROM: the one named exactly like the ROM first,
 * then those of its region, then the others (Game Genie, Action
 * Replay...), lists of another release (demos...) last; -1 when its title
 * is another. */
static int cheat_rank(const char *name, const char *stem, const char *key, const char *region)
{
	char other[NAME_MAX + 1];

	title_key(name, other, sizeof(other));
	if (strcmp(other, key))
		return -1;
	if (!strcasecmp(name, stem))
		return 0;
	return (same_region(name, region) ? 1 : 2) + (other_release(name, stem) ? 2 : 0);
}

/* The collection's files of the ROM's title in the system's databases
 * (index.txt lines "<database>/<name>.cht"). */
static void scan_cheat_index(const char *dbs, const char *stem, const char *key,
			     const char *region, int base, struct cheat_list *lists, int *count)
{
	char line[PATH_MAX];
	FILE *fp = fopen(CHEATS_INDEX, "r");

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char name[NAME_MAX + 1];
		char path[PATH_MAX];
		char *slash;
		size_t len;
		int rank;

		trim(line);
		len = strlen(line);
		if (!(slash = strchr(line, '/')) || len < 5 || strcasecmp(line + len - 4, ".cht"))
			continue;
		*slash = '\0';
		if (!pipe_list_has(dbs, line))
			continue;
		copy_text(name, sizeof(name), slash + 1);
		name[strlen(name) - 4] = '\0';
		if ((rank = cheat_rank(name, stem, key, region)) < 0)
			continue;
		snprintf(path, sizeof(path), CHEATS_LIBRETRO "/%s/%s", line, slash + 1);
		add_cheat_list(lists, count, path, name, base + rank);
	}
	fclose(fp);
}

/* Extracts collection files (paths under CHEATS_LIBRETRO) from the
 * archive, all in one run of unzip; its names are patterns there. */
static void extract_cheats(const char *const *paths, int n)
{
	char names[MAX_CHEAT_LISTS][PATH_MAX];
	char *argv[8 + MAX_CHEAT_LISTS];
	int argc = 0;
	int status;
	pid_t pid;

	argv[argc++] = (char *)"unzip";
	argv[argc++] = (char *)"-o";
	argv[argc++] = (char *)"-q";
	argv[argc++] = (char *)CHEATS_ARCHIVE;
	for (int i = 0; i < n && i < MAX_CHEAT_LISTS; i++) {
		const char *entry = paths[i] + strlen(CHEATS_LIBRETRO "/");
		size_t len = 0;

		for (const char *p = entry; *p && len + 2 < sizeof(names[i]); p++) {
			if (strchr("[]*?\\", *p))
				names[i][len++] = '\\';
			names[i][len++] = *p;
		}
		names[i][len] = '\0';
		argv[argc++] = names[i];
	}
	argv[argc++] = (char *)"-d";
	argv[argc++] = (char *)CHEATS_LIBRETRO;
	argv[argc] = NULL;
	if (argc == 6 || !file_exists(CHEATS_ARCHIVE))
		return;
	pid = fork();
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
		}
		execvp("unzip", argv);
		_exit(127);
	}
	/* A few small files: waited for here (the main loop's reaper never
	 * sees this child first). */
	while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
}

/* The .cht files of a folder with the ROM's title. */
static void scan_cheat_dir(const char *dir, const char *stem, const char *key,
			   const char *region, int base, struct cheat_list *lists, int *count)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		char name[NAME_MAX + 1];
		char path[PATH_MAX];
		size_t len = strlen(e->d_name);
		int rank;

		if (e->d_name[0] == '.' || len < 5 || strcasecmp(e->d_name + len - 4, ".cht"))
			continue;
		copy_text(name, sizeof(name), e->d_name);
		name[len - 4] = '\0';
		if ((rank = cheat_rank(name, stem, key, region)) < 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		add_cheat_list(lists, count, path, name, base + rank);
	}
	closedir(d);
}

/* The cheat lists for a game, best first: the user's own files
 * (/userdata/cheats/<system>/<rom>.cht, /userdata/cheats/<rom>.cht, then
 * files of the same title in /userdata/cheats/<system>), then the libretro
 * collection's files of the same title in the system's databases. */
static int cheat_lists(const char *system, const char *rom, struct cheat_list *lists)
{
	char stem[NAME_MAX + 1];
	char key[NAME_MAX + 1];
	char region[32];
	char dbs[512];
	char path[PATH_MAX];
	int count = 0;

	const char *missing[MAX_CHEAT_LISTS];
	int n = 0;

	content_base_name(rom, stem, sizeof(stem));
	title_key(stem, key, sizeof(key));
	region_tag(stem, region, sizeof(region));
	snprintf(path, sizeof(path), CHEATS_DIR "/%s/%s.cht", system, stem);
	if (file_exists(path))
		add_cheat_list(lists, &count, path, stem, 0);
	snprintf(path, sizeof(path), CHEATS_DIR "/%s.cht", stem);
	if (file_exists(path))
		add_cheat_list(lists, &count, path, stem, 0);
	if (!key[0])
		return count;
	snprintf(path, sizeof(path), CHEATS_DIR "/%s", system);
	scan_cheat_dir(path, stem, key, region, 1, lists, &count);
	system_databases(system, dbs, sizeof(dbs));
	if (dbs[0])
		scan_cheat_index(dbs, stem, key, region, 10, lists, &count);
	/* The collection's lists are extracted the first time they are
	 * offered; one that could not be is left out. */
	for (int i = 0; i < count; i++)
		if (!file_exists(lists[i].path))
			missing[n++] = lists[i].path;
	if (n)
		extract_cheats(missing, n);
	for (int i = 0; i < count;) {
		if (file_exists(lists[i].path)) {
			i++;
			continue;
		}
		memmove(&lists[i], &lists[i + 1], (size_t)(count - i - 1) * sizeof(*lists));
		count--;
	}
	return count;
}

/* The game's list: the user's choice while it exists, else the best one
 * ("" when there is none). */
static void current_cheat_file(const struct game_overrides *o, const struct cheat_list *lists,
			       int count, char *out, size_t size)
{
	if (o->cheat_file[0] && !file_exists(o->cheat_file) &&
	    !strncmp(o->cheat_file, CHEATS_LIBRETRO "/", strlen(CHEATS_LIBRETRO "/"))) {
		/* A new collection replaced the extracted files. */
		const char *path = o->cheat_file;

		extract_cheats(&path, 1);
	}
	if (o->cheat_file[0] && file_exists(o->cheat_file))
		copy_text(out, size, o->cheat_file);
	else
		copy_text(out, size, count > 0 ? lists[0].path : "");
}

/* Cheat k of a .cht line "cheat<k>_<field> = ...": the field, NULL when
 * the line is something else. */
static char *cheat_line(char *line, int *k)
{
	char *p = line;
	char *end;
	long n;

	while (*p == ' ')
		p++;
	if (strncmp(p, "cheat", 5) || !isdigit((unsigned char)p[5]))
		return NULL;
	n = strtol(p + 5, &end, 10);
	if (*end != '_' || n < 0 || n > 100000)
		return NULL;
	*k = (int)n;
	return end + 1;
}

/* The session's copy of the list with the user's cheats on, loaded by
 * RetroArch; nothing on clears the session's cheats. */
static void apply_session_cheats(const struct game_overrides *o, const char *file)
{
	char tmp[] = SESSION_CHEATS ".tmp";
	char line[2048];
	char list[sizeof(o->cheats)];
	char *save = NULL;
	FILE *in;
	FILE *out;
	int on = 0;

	if (!file[0] || !o->cheats[0] || mkdir_p(SESSION_DIR) != 0 || !(in = fopen(file, "r"))) {
		ra_command("CHEATS_LOAD");
		session.cheats_on = 0;
		notify_subscribers();
		return;
	}
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return;
	}
	while (fgets(line, sizeof(line), in)) {
		int k;
		char *field = cheat_line(line, &k);

		/* Every cheat off, then the user's on. */
		if (field && !strncmp(field, "enable", 6) && (field[6] == ' ' || field[6] == '='))
			continue;
		fputs(line, out);
	}
	fclose(in);
	copy_text(list, sizeof(list), o->cheats);
	fputc('\n', out);
	for (char *k = strtok_r(list, ",", &save); k; k = strtok_r(NULL, ",", &save)) {
		fprintf(out, "cheat%d_enable = \"true\"\n", atoi(k));
		on++;
	}
	if (fclose(out) != 0 || rename(tmp, SESSION_CHEATS) != 0) {
		unlink(tmp);
		return;
	}
	ra_command("CHEATS_LOAD %s", SESSION_CHEATS);
	session.cheats_on = on;
	notify_subscribers();
}

/* Writes the settings nuubOS enforces at every launch; RetroArch appends
 * them over the user's configuration (--appendconfig). */
/* RetroArch notifications switched off for every session. */
static const char *const ra_notification_keys[] = {
	"notification_show_autoconfig", "notification_show_autoconfig_fails",
	"notification_show_cheats_applied", "notification_show_patch_applied",
	"notification_show_remap_load", "notification_show_config_override_load",
	"notification_show_set_initial_disk", "notification_show_disk_control",
	"notification_show_save_state", "notification_show_fast_forward",
	"notification_show_screenshot", "notification_show_refresh_rate",
	"notification_show_netplay_extra", "menu_show_load_content_animation",
	NULL,
};

/*
 * UI confirm/back swap (2026-10-24, user request): nuubos-input's own
 * BIND SET/GET keeps this a preference for which face button confirms in
 * nuubOS's own menus (Home, Settings, Quick Menu), entirely decoupled from
 * the per-controller game mapping controllersd sends as the padmap - a
 * game's own buttons never change either way. RetroArch gets the matching
 * menu_swap_ok_cancel_buttons so its own menu agrees with nuubOS's; actual
 * gameplay input is read straight from the padmap and is unaffected.
 */
static bool ui_confirm_is_east(void)
{
	/* inputd's effective binding: the active user's file over the
	 * device's (it alone knows the layers). */
	static const char command[] = "BIND GET menu_confirm\n";
	char answer[32];
	int fd = connect_socket(INPUT_SOCKET, 500);
	bool east = false;

	if (fd < 0)
		return false;
	if (write_all(fd, command, strlen(command)) == 0 &&
	    read_line_fd(fd, answer, sizeof(answer)) == 0)
		east = atoi(answer) == 305; /* BTN_EAST */
	close(fd);
	return east;
}

static int write_session_cfg(const char *ra_dir, const char *saves,
			     const char *states, const char *screenshots,
			     const struct game_overrides *o, const struct achievements *a,
			     bool auto_load, const char **shader)
{
	char tmp[] = SESSION_CFG ".tmp";
	bool auto_save = !session.settings.slot_rotation && !session.hardcore;
	FILE *fp;
	int player;
	int rc;
	int i;

	if (mkdir_p(SESSION_DIR) != 0)
		return -1;
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	/* builtin_imageviewer_enable off: with it on, RetroArch opens .png
	 * content in its own image viewer even with a core given by -L, so
	 * every PICO-8 cartridge (.p8.png) showed its label picture instead
	 * of running in FAKE-08/Retro8. nuubOS never uses that viewer. */
	fprintf(fp,
		"# Written by nuubos-emud for the running session.\n"
		"builtin_imageviewer_enable = \"false\"\n"
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
		/* RetroArch's own unswapped default already uses East (A) as
		 * its menu OK: swap only to follow nuubOS to South (B). */
		"menu_swap_ok_cancel_buttons = \"%s\"\n"
		"config_save_on_exit = \"true\"\n"
		"user_language = \"%d\"\n",
		saves, states, screenshots, ra_dir, ra_dir, ra_dir, ra_dir,
		ra_dir, ra_dir, ra_dir, ui_confirm_is_east() ? "false" : "true",
		retroarch_language(session.user));
	/* Port N is reserved for nuubOS game gamepad N (product id N), so
	 * player order always follows nuubOS player assignment. */
	for (player = 1; player <= MAX_PLAYERS; player++)
		fprintf(fp,
			"input_player%d_device_reservation_type = \"2\"\n"
			"input_player%d_reserved_device = \"0000:%04x\"\n",
			player, player, player);
	*shader = write_presentation(fp, o);
	/* Threaded video for CPU-bound systems (2026-10-17, Mario Kart DS on the
	 * CubeXX 39 -> 44 fps): the emulation thread no longer waits in
	 * eglSwapBuffers for the display (3.6 -> 0.8 ms a frame), at the cost
	 * of one frame of latency. Everything else keeps the user's value. */
	remember_key("video_threaded");
	if (!strcmp(session.system, "nds"))
		fprintf(fp, "video_threaded = \"true\"\n");
	/* Light systems run the core late in the refresh, closer to the
	 * vblank, so it reads the input later: RetroArch's automatic frame
	 * delay (0 = target 3/4 of a frame) and it lowers the delay by itself
	 * when the core's time no longer fits. Heavy systems need the whole
	 * frame (see heavy_system). */
	remember_key("video_frame_delay");
	remember_key("video_frame_delay_auto");
	fprintf(fp, "video_frame_delay = \"0\"\nvideo_frame_delay_auto = \"%s\"\n",
		heavy_system(session.system) ? "false" : "true");
	/* The performance overlay line is drawn into the frame with the OSD
	 * font (RetroArch notifications use widgets, not this font). */
	remember_key("video_font_enable");
	remember_key("video_font_path");
	remember_key("video_font_size");
	fprintf(fp, "video_font_enable = \"true\"\nvideo_font_path = \"%s\"\n"
		"video_font_size = \"32.000000\"\n", OSD_FONT);
	/* RetroArch's own notifications are off (nuubOS notifies save/load/
	 * slot/fast-forward/screenshot itself); RetroAchievements popups are
	 * separate widgets and stay (user request 2026-10-11). */
	for (i = 0; ra_notification_keys[i]; i++) {
		remember_key(ra_notification_keys[i]);
		fprintf(fp, "%s = \"false\"\n", ra_notification_keys[i]);
	}
	remember_key("notification_show_screenshot_flash");
	fprintf(fp, "notification_show_screenshot_flash = \"2\"\n");
	/* Progress is kept when the game closes (.state.auto, or with slot
	 * rotation the next slot, saved by emud: request_quit); auto_load
	 * resumes from it when it is the most recent state. */
	remember_key("savestate_auto_save");
	remember_key("savestate_auto_load");
	fprintf(fp, "savestate_auto_save = \"%s\"\nsavestate_auto_load = \"%s\"\n",
		auto_save ? "true" : "false", auto_load ? "true" : "false");
	write_session_overrides(fp, a);
	write_session_netplay(fp);
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

/*
 * GitHub #30: a PSP game in a folder of its own ("<Folder>/EBOOT.PBP",
 * homebrew and PSN games) runs from ms0:/PSP/GAME/<Folder>/ as on a PSP.
 * PPSSPP starts a game outside its memory stick from umd0:/, and some
 * ports need the memory stick path (JGR Portable prepares its data only
 * from there). Its states go to states/psp/<Folder>/: every such game is
 * called EBOOT.
 */
static bool psp_folder_game(const char *system, const char *content, char *folder, size_t size)
{
	const char *name = strrchr(content, '/');
	const char *start;

	if (strcmp(system, "psp") || !name || name == content || strcasecmp(name + 1, "EBOOT.PBP"))
		return false;
	start = name;
	while (start > content && start[-1] != '/')
		start--;
	if (start == name || (size_t)(name - start) >= size)
		return false;
	snprintf(folder, size, "%.*s", (int)(name - start), start);
	return true;
}

/*
 * In the RetroArch child: the game's folder bound on the memory stick, in
 * <saves>/psp/GAME/<Folder> (PPSSPP's memory stick is RetroArch's save
 * directory; named psp, it is the stick's PSP folder itself: STRIP_PSP,
 * PPSSPP patch 0006), in RetroArch's own mount namespace: Syncthing, the backup and the rest
 * of the system never see it, and it goes away with RetroArch. The content
 * path then names the game there. Without it the game still starts, from
 * umd0:/ as before.
 */
static void bind_game_folder(char *content, size_t size)
{
	const char *name = strrchr(content, '/');
	char path[PATH_MAX];

	if (!session.game_mount[0] || !name)
		return;
	if (unshare(CLONE_NEWNS) != 0 ||
	    mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL) != 0 ||
	    mount(session.game_dir, session.game_mount, NULL, MS_BIND | MS_REC, NULL) != 0) {
		log_msg("emud: game folder not on the memory stick: %s", strerror(errno));
		return;
	}
	snprintf(path, sizeof(path), "%s%s", session.game_mount, name);
	copy_text(content, size, path);
}

static void reset_session(void)
{
	stop_watch();
	/* The empty mount point of psp_folder_game (RetroArch is gone). */
	if (session.game_mount[0])
		(void)rmdir(session.game_mount);
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
	game_cpu_apply(0);
}

/*
 * mode: "resume" (default) starts from the most recent state (a slot or the
 * automatic state written when the game last closed), "new" from the
 * beginning, "slot:N" from slot N ("slot:-1" = the automatic state).
 * net: "" alone, "host" hosts a netplay game on the local network,
 * "connect:<address>:<port>:<core>" joins one (from the beginning: the
 * host sends its game; with the host's emulator, NETPLAY_HOSTS).
 */
static const char *cheats_blocked(void);

/*
 * The child side of a session: RetroArch with the session's pipes, log
 * and environment (the user's directories, Wayland, PipeWire, the
 * per-system tuning of the running game). Never returns.
 */
static void exec_retroarch(char **argv, const char *base, const char *ra_dir,
			   const int in_pipe[2], const int out_pipe[2],
			   int overscan, int render_height)
{
	bool diag = access(DIAG_ACTIVE, F_OK) == 0;
	char dir[PATH_MAX];
	int log_fd;
	sigset_t none;

	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	setsid();
	dup2(in_pipe[0], STDIN_FILENO);
	dup2(out_pipe[1], STDOUT_FILENO);
	log_fd = diag ? open(DIAG_RETROARCH_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600)
		      : open(RETROARCH_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (log_fd >= 0)
		dup2(log_fd, STDERR_FILENO);
	clearenv();
	setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
	setenv("HOME", ra_dir, 1);
	/* RetroArch's own default directories ($XDG_CONFIG_HOME/
	 * retroarch) are the user's RetroArch directory. */
	snprintf(dir, sizeof(dir), "%s/appdata", base);
	setenv("XDG_CONFIG_HOME", dir, 1);
	setenv("XDG_RUNTIME_DIR", WAYLAND_RUNTIME, 1);
	setenv("WAYLAND_DISPLAY", "wayland-0", 1);
	setenv("PIPEWIRE_RUNTIME_DIR", PIPEWIRE_RUNTIME, 1);
	setenv("LANG", "C.UTF-8", 1);
	if (overscan >= 0) {
		char pct[16];

		snprintf(pct, sizeof(pct), "%d", overscan);
		setenv("NUUBOS_RA_OVERSCAN_X", pct, 1);
	}
	if (render_height > 0) {
		char lines[16];

		snprintf(lines, sizeof(lines), "%d", render_height);
		setenv("NUUBOS_RA_RENDER_HEIGHT", lines, 1);
	}
	if (session.bezel_env[0])
		setenv("NUUBOS_RA_BEZEL", session.bezel_env, 1);
	if (session.pixel_crop)
		setenv("NUUBOS_RA_PIXEL_CROP", "8", 1);
	/* Saturn: YabaSanshiro uploads its texture atlas by regions; Mesa
	 * patch 0008 makes such textures linear (no staging copy and
	 * software tiling per upload). Virtua Fighter 2 fight 27.3-27.8
	 * -> 28.4 fps (2026-10-23); neutral on Dreamcast, so Saturn only. */
	if (!strcmp(session.system, "saturn"))
		setenv("NUUBOS_PAN_LINEAR_UPLOADS", "1", 1);
	/* Dreamcast family (Flycast-nuubOS draws each frame at the start of
	 * the next retro_run, so it reaches the GPU late): a swap may wait
	 * two frames back instead of one (Mesa patch 0005). Crazy Taxi
	 * while driving 53-56 -> 59.6 fps (2026-10-21). It first waits up to
	 * 8 ms for the frame before (Mesa patch 0006): frames the GPU had
	 * not finished missed their vblank and went out in pairs. Frames
	 * over 20 ms, 3 x 20 s per game (2026-10-22): depth 1 / 2 / 2+8 ms
	 * DoA2 41 / 136 / 68, Le Mans 183 / 22 / 32, Crazy Taxi 12 / 4 /
	 * 11, Shenmue II 13 / 11 / 10, same frame rates. */
	if (!strcmp(session.system, "dreamcast") || !strcmp(session.system, "naomi") ||
	    !strcmp(session.system, "atomiswave")) {
		setenv("NUUBOS_THROTTLE_DEPTH", "2", 1);
		setenv("NUUBOS_THROTTLE_SOFT_MS", "8", 1);
	}
	/* Light systems: every frame is committed at the start of its swap
	 * and the swap then waits for its frame callback (Mesa patch
	 * 0007); before, the wait came first and each frame reached labwc
	 * one refresh later. Input -> screen 57 -> 40.6 ms with the
	 * latency probe core on the CubeXX (2026-10-23, docs/dev/input.md).
	 * Heavy systems keep the wait first: their frames reach the GPU
	 * late, committed at once they missed the vblank (Crazy Taxi
	 * while driving ~60 -> 40 fps, user 2026-10-23). */
	if (!heavy_system(session.system))
		setenv("NUUBOS_SWAP_WAIT_AFTER", "1", 1);
	execv(RETROARCH_BIN, argv);
	_exit(127);
}

static const char *launch(const char *game, const char *mode, const char *net)
{
	char answer[2 * PATH_MAX + 128];
	char command[64];
	char core_path[PATH_MAX];
	char base[160];
	char ra_dir[256];
	char saves[PATH_MAX];
	char screenshots[PATH_MAX];
	char path[PATH_MAX];
	char *tab;
	char *rom;
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
	int overscan;
	char net_address[INET_ADDRSTRLEN] = "";
	char net_port[8] = "";
	char net_core[64] = "";

	if (session.state != SESSION_IDLE)
		return "busy";
	if (!net)
		net = "";
	if (!strncmp(net, "connect:", 8)) {
		struct in_addr ip;
		char *p;
		char *q;
		long port;

		copy_text(net_address, sizeof(net_address), net + 8);
		if (!(p = strchr(net_address, ':')))
			return "net";
		*p = '\0';
		port = strtol(strchr(net + 8, ':') + 1, &q, 10);
		if (inet_pton(AF_INET, net_address, &ip) != 1 || *q != ':' || port < 1 || port > 65535)
			return "net";
		snprintf(net_port, sizeof(net_port), "%ld", port);
		copy_text(net_core, sizeof(net_core), q + 1);
		mode = "new";
	} else if (net[0] && strcmp(net, "host")) {
		return "net";
	}
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
	if (data_away())
		return "data";
	load_settings(session.user, &session.settings);

	snprintf(command, sizeof(command), "RESOLVE\t%s", game);
	if (library_request(command, answer, sizeof(answer)) != 0)
		return "library";
	if (strncmp(answer, "ERR ", 4) == 0)
		return strcmp(answer + 4, "unavailable") == 0 ? "unavailable" : "game";
	if (strncmp(answer, "OK ", 3) != 0 || !(tab = strchr(answer + 3, '\t')))
		return "library";
	*tab = '\0';
	/* A disc set: the playlist of all its discs, then the ROM itself. */
	rom = strchr(tab + 1, '\t');
	if (rom)
		*rom++ = '\0';
	copy_text(session.game, sizeof(session.game), game);
	copy_text(session.system, sizeof(session.system), answer + 3);
	copy_text(session.content, sizeof(session.content), tab + 1);
	copy_text(session.rom, sizeof(session.rom), rom ? rom : tab + 1);
	if (!file_exists(session.content))
		return "unavailable";
	if (!resolve_core(session.system, session.core, sizeof(session.core),
			  core_path, sizeof(core_path)))
		return "no-core";
	load_overrides(session.user, game, &overrides);
	{
		struct game_overrides sys;

		/* What the game leaves unset comes from its System Settings. */
		load_system_overrides(session.user, session.system, &sys);
		inherit_system(&overrides, &sys);
		remap_list(&overrides, session.remap, sizeof(session.remap));
	}
	load_achievements(session.user, &achievements);
	session.hardcore = achievements.enabled && achievements.hardcore;
	if (session.hardcore && !strncmp(mode, "slot:", 5))
		return "hardcore";
	achievements.proxy = raproxy_ensure(session.user, &achievements);
	/* Joining: the host's emulator, whatever the game's choice. */
	if (net_core[0]) {
		char cores[512];

		available_cores(session.system, cores, sizeof(cores));
		if (!list_has(cores, net_core))
			return "no-core";
		copy_text(overrides.core, sizeof(overrides.core), net_core);
	}
	if (net[0] && session.hardcore)
		return "hardcore";
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
	{
		char folder[NAME_MAX + 1];
		const char *slash = strrchr(session.content, '/');

		if (psp_folder_game(session.system, session.content, folder, sizeof(folder))) {
			size_t len = strlen(session.states_dir);

			snprintf(session.states_dir + len, sizeof(session.states_dir) - len, "/%s", folder);
			snprintf(session.game_dir, sizeof(session.game_dir), "%.*s",
				 (int)(slash - session.content), session.content);
			snprintf(session.game_mount, sizeof(session.game_mount), "%s/GAME/%s",
				 saves, folder);
			if (mkdir_p(session.game_mount) != 0)
				return "storage";
		}
	}
	snprintf(screenshots, sizeof(screenshots), "%s/screenshots", base);
	snprintf(path, sizeof(path), "%s/config/remaps", ra_dir);
	if (mkdir_p(path) != 0 || mkdir_p(saves) != 0 ||
	    mkdir_p(session.states_dir) != 0 || mkdir_p(screenshots) != 0 ||
	    mkdir_p(DOWNLOADED_CORES) != 0 || mkdir_p(CACHE_DIR) != 0 ||
	    mkdir_p(BIOS_DIR) != 0)
		return "storage";
	wait_system_seed(session.core);
	place_bios(session.system);
	snprintf(path, sizeof(path), "%s/retroarch.cfg", ra_dir);
	if (seed_file(DEFAULT_RA_CFG, path) != 0)
		return "storage";
	copy_text(session.ra_cfg, sizeof(session.ra_cfg), path);
	copy_text(session.screenshots_dir, sizeof(session.screenshots_dir), screenshots);
	snprintf(path, sizeof(path), "%s/retroarch-core-options.cfg", ra_dir);
	if (seed_file(DEFAULT_CORE_OPTIONS, path) == 0) {
		char done[PATH_MAX];

		merge_missing_keys(DEFAULT_CORE_OPTIONS, path);
		snprintf(done, sizeof(done), "%s/core-option-updates", ra_dir);
		apply_core_option_updates(path, done);
	}
	apply_boot_options(path, session.settings.boot_screens);
	copy_text(session.core_options, sizeof(session.core_options), path);
	if (!strcmp(session.core, MELONDS_NUUBOS)) {
		char value[64];

		/* Layout and stylus as last chosen in this game; else the
		 * user's last layout and the stylus off. */
		for (int i = 0; i < NDS_LAYOUTS; i++)
			if (!strcmp(overrides.nds_layout, nds_layouts[i].name))
				write_core_option(path, "melonds_screen_layout", nds_layouts[i].value);
		if (!strcmp(overrides.stylus, "right") || !strcmp(overrides.stylus, "left") ||
		    !strcmp(overrides.stylus, "dpad")) {
			session.stylus = true;
			copy_text(session.stylus_control, sizeof(session.stylus_control), overrides.stylus);
		}
		write_core_option(path, "melonds_nuubos_stylus",
				  session.stylus ? session.stylus_control : "off");
		session.nds_corner = 0;
		for (int i = 0; i < NDS_CORNERS; i++)
			if (!strcmp(overrides.nds_corner, nds_corners[i]))
				session.nds_corner = i;
		write_core_option(path, "melonds_nuubos_pip_corner", nds_corners[session.nds_corner]);
		if (read_core_option(path, "melonds_screen_layout", value, sizeof(value)))
			for (int i = 0; i < NDS_LAYOUTS; i++)
				if (!strcmp(value, nds_layouts[i].value))
					session.nds_layout = i;
		if (overrides.accuracy[0])
			session_core_option("melonds_accuracy", overrides.accuracy);
	}
	if (!strncmp(session.core, "pcsx_rearmed", 12)) {
		/* PlayStation: a DualShock in every port (the core's default is
		 * the digital pad, which never vibrates). Games that do not know
		 * the DualShock see a digital pad: it starts in digital mode until
		 * the game asks for analog. A RetroArch core override, so other
		 * cores keep the plain joypad; a device the user chose is kept. */
		char override[PATH_MAX], value[16];
		FILE *fp;

		snprintf(override, sizeof(override), "%s/config/PCSX-ReARMed", ra_dir);
		if (mkdir_p(override) == 0) {
			snprintf(override, sizeof(override), "%s/config/PCSX-ReARMed/PCSX-ReARMed.cfg", ra_dir);
			if (access(override, F_OK) != 0 && (fp = fopen(override, "w")))
				fclose(fp);
			for (int port = 1; port <= 4; port++) {
				char key[32];

				snprintf(key, sizeof(key), "input_libretro_device_p%d", port);
				/* RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1) */
				if (!read_core_option(override, key, value, sizeof(value)))
					write_core_option(override, key, "517");
			}
		}
	}
	if (!strcmp(session.system, "n64")) {
		/* The game's accessory in every player's slot, for both N64
		 * cores (ParaLLEl and Mupen64Plus, bundled or downloaded):
		 * Rumble Pak unless the game keeps Controller Pak saves. */
		const char *pak = !strcmp(overrides.n64_pak, "memory") ? "memory" : "rumble";
		static const char *const prefixes[] = { "parallel-n64-pak", "mupen64plus-pak" };

		for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
			for (int player = 1; player <= 4; player++) {
				char key[32];

				snprintf(key, sizeof(key), "%s%d", prefixes[i], player);
				write_core_option(path, key, pak);
			}
	}
	{
		/* 3D render resolution for this session: the game's choice, or
		 * Auto (PCSX ReARMed by the output; the others keep the user's
		 * option). A key RetroArch has not written yet holds the core's
		 * default, its native value, so that is what comes back. */
		const struct resolution_option *res = resolution_option(session.core);
		const char *choice = overrides.resolution;
		char value[64];

		if (flycast_core(session.core) && overrides.accuracy[0]) {
			if (!read_core_option(path, FLYCAST_SORTING, value, sizeof(value)))
				write_core_option(path, FLYCAST_SORTING, FLYCAST_SORTING_FAST);
			session_core_option(FLYCAST_SORTING, !strcmp(overrides.accuracy, "fast") ?
					    FLYCAST_SORTING_FAST : FLYCAST_SORTING_ACCURATE);
		}

		if (res && !choice[0] && res->auto_hdmi)
			choice = hdmi_active() ? "2x" : "1x";
		if (res && choice[0]) {
			if (!read_core_option(path, res->key, value, sizeof(value)))
				write_core_option(path, res->key, res->x1);
			session_core_option(res->key, !strcmp(choice, "2x") ? res->x2 : res->x1);
		}
	}
	{
		/* Frameskip for this session: the game's (or its system's)
		 * choice; none = the user's core options. */
		const struct frameskip_option *fs = frameskip_option(session.core);
		int mode = frameskip_mode(overrides.frameskip);
		char value[64];

		for (int k = 0; fs && mode >= 0 && k < 2 && fs->key[k]; k++) {
			if (!fs->value[k][mode])
				continue;
			if (!read_core_option(path, fs->key[k], value, sizeof(value)))
				write_core_option(path, fs->key[k], fs->initial[k]);
			session_core_option(fs->key[k], fs->value[k][mode]);
		}
	}

	snprintf(path, sizeof(path), "%s/config/global.glslp", ra_dir);
	(void)seed_file(DEFAULT_SHADER_PRESET, path);
	content_base_name(session.content, path, sizeof(path));
	snprintf(session.state_base, sizeof(session.state_base), "%s/%s",
		 session.states_dir, path);

	/* Starting point: RetroArch loads an entry slot (--entryslot) or the
	 * automatic state (savestate_auto_load). Hardcore always starts from
	 * the beginning. */
	if (session.hardcore) {
		(void)newest_state(session.state_base, session.settings.slots, &manual);
	} else if (!strcmp(mode, "resume")) {
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
	overscan = tv_overscan(session.system, &overrides);
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
		char *argv[24] = {
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
		if (!strcmp(net, "host")) {
			argv[argc++] = (char *)"--host";
		} else if (net_address[0]) {
			argv[argc++] = (char *)"--connect";
			argv[argc++] = net_address;
			argv[argc++] = (char *)"--port";
			argv[argc++] = net_port;
		}
		/* Diagnostic mode (docs/dev/system.md): RetroArch logs everything,
		 * every session of the boot kept for the support bundle. */
		if (access(DIAG_ACTIVE, F_OK) == 0)
			argv[argc++] = (char *)"--verbose";
		argv[argc++] = (char *)"-L";
		argv[argc++] = core_path;
		bind_game_folder(session.content, sizeof(session.content));
		argv[argc++] = session.content;
		argv[argc] = NULL;
		snprintf(cfg, sizeof(cfg), "%s/retroarch.cfg", ra_dir);
		exec_retroarch(argv, base, ra_dir, in_pipe, out_pipe, overscan, render_height);
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
	session.cpu_level = full_clock_system(session.system) ? GAME_CPU_LEVELS - 1 : 0;
	if (atoi(overrides.cpu_level) > session.cpu_level)
		session.cpu_level = atoi(overrides.cpu_level) < GAME_CPU_LEVELS ?
				    atoi(overrides.cpu_level) : GAME_CPU_LEVELS - 1;
	game_cpu_grace(GAME_CPU_START_GRACE_MS);
	session.started_ms = monotonic_ms();
	copy_text(session.net_launch, sizeof(session.net_launch), net_address[0] ? "connect" : net);
	/* The user's cheats for the game (never with netplay or hardcore).
	 * RetroArch restores the cheats it saved in its own file of the game
	 * when it last closed (<core>/<game>.cht, those turned on included):
	 * without cheats of the user's they are cleared. */
	if (!cheats_blocked() && overrides.cheats[0]) {
		struct cheat_list lists[MAX_CHEAT_LISTS];
		int count = cheat_lists(session.system, session.rom, lists);
		char file[PATH_MAX];

		current_cheat_file(&overrides, lists, count, file, sizeof(file));
		apply_session_cheats(&overrides, file);
	} else {
		ra_command("CHEATS_LOAD");
	}
	log_msg("emud: launch game=%s system=%s core=%s pid=%d user=%s start=%d shader=%s render=%d hardcore=%d net=%s cheats=%d",
		session.game, session.system, session.core, (int)pid,
		session.user, start, shader[0] ? shader : "none", render_height,
		session.hardcore ? 1 : 0, net[0] ? net : "-", session.cheats_on);
	library_session("SESSION_BEGIN", session.game);
	notify_subscribers();
	return NULL;
}

/* The active user's RetroArch directory, configuration seeded from the
 * nuubOS defaults (first use, or after RESET_RETROARCH). */
static const char *prepare_user_retroarch(char *base, size_t base_size,
					  char *ra_dir, size_t ra_size)
{
	char path[PATH_MAX];

	snprintf(base, base_size, USERS_DIR "/%s", session.user);
	snprintf(ra_dir, ra_size, "%s/appdata/retroarch", base);
	snprintf(path, sizeof(path), "%s/config/remaps", ra_dir);
	if (mkdir_p(path) != 0 || mkdir_p(DOWNLOADED_CORES) != 0 ||
	    mkdir_p(CACHE_DIR) != 0 || mkdir_p(BIOS_DIR) != 0)
		return "storage";
	snprintf(path, sizeof(path), "%s/retroarch.cfg", ra_dir);
	if (seed_file(DEFAULT_RA_CFG, path) != 0)
		return "storage";
	copy_text(session.ra_cfg, sizeof(session.ra_cfg), path);
	snprintf(path, sizeof(path), "%s/retroarch-core-options.cfg", ra_dir);
	if (seed_file(DEFAULT_CORE_OPTIONS, path) == 0) {
		char done[PATH_MAX];

		merge_missing_keys(DEFAULT_CORE_OPTIONS, path);
		snprintf(done, sizeof(done), "%s/core-option-updates", ra_dir);
		apply_core_option_updates(path, done);
	}
	copy_text(session.core_options, sizeof(session.core_options), path);
	snprintf(path, sizeof(path), "%s/config/global.glslp", ra_dir);
	(void)seed_file(DEFAULT_SHADER_PRESET, path);
	return NULL;
}

/*
 * MENU (the RetroArch application, user request 2026-10-27): RetroArch's
 * own menu without a game, with the active user's configuration, for what
 * is not per game (drivers, core downloads, RetroArch's own options). The
 * session has no states, no library entry and no hotkey but Quit; the
 * session configuration (paths, drivers, input ports) is the games' one,
 * with the plain integer picture (no bezel, no CRT) for content started
 * from RetroArch's menu.
 */
static const char *launch_menu(void)
{
	char base[160];
	char ra_dir[256];
	char saves[PATH_MAX];
	char states[PATH_MAX];
	char screenshots[PATH_MAX];
	struct game_overrides overrides;
	struct achievements achievements;
	const char *shader = "";
	const char *error;
	int in_pipe[2];
	int out_pipe[2];
	pid_t pid;

	if (session.state != SESSION_IDLE)
		return "busy";
	if (!read_active_user(session.user, sizeof(session.user)))
		return "no-user";
	load_settings(session.user, &session.settings);
	session.menu = true;
	if ((error = prepare_user_retroarch(base, sizeof(base), ra_dir, sizeof(ra_dir)))) {
		reset_session();
		return error;
	}
	snprintf(saves, sizeof(saves), "%s/saves", base);
	snprintf(states, sizeof(states), "%s/states", base);
	snprintf(screenshots, sizeof(screenshots), "%s/screenshots", base);
	if (mkdir_p(saves) != 0 || mkdir_p(states) != 0 || mkdir_p(screenshots) != 0) {
		reset_session();
		return "storage";
	}
	memset(&overrides, 0, sizeof(overrides));
	copy_text(overrides.aspect, sizeof(overrides.aspect), "integer");
	load_achievements(session.user, &achievements);
	if (write_session_cfg(ra_dir, saves, states, screenshots, &overrides,
			      &achievements, false, &shader) != 0) {
		reset_session();
		return "storage";
	}

	session.input_fd = input_gamepads_on();
	if (session.input_fd < 0)
		log_msg("emud: inputd game gamepads unavailable");
	send_hotkeys();

	if (pipe2(in_pipe, O_CLOEXEC) != 0) {
		reset_session();
		return "spawn";
	}
	if (pipe2(out_pipe, O_CLOEXEC) != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		reset_session();
		return "spawn";
	}
	pid = fork();
	if (pid < 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		reset_session();
		return "spawn";
	}
	if (pid == 0) {
		char *argv[10] = {
			(char *)RETROARCH_BIN, (char *)"--config", session.ra_cfg,
			(char *)"--appendconfig", (char *)SESSION_CFG,
			(char *)"--set-shader", (char *)shader, (char *)"--menu",
		};
		int argc = 8;

		if (access(DIAG_ACTIVE, F_OK) == 0)
			argv[argc++] = (char *)"--verbose";
		argv[argc] = NULL;
		exec_retroarch(argv, base, ra_dir, in_pipe, out_pipe, -1, 0);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);
	session.pid = pid;
	session.stdin_fd = in_pipe[1];
	session.stdout_fd = out_pipe[0];
	(void)fcntl(session.stdin_fd, F_SETFL, O_NONBLOCK);
	(void)fcntl(session.stdout_fd, F_SETFL, O_NONBLOCK);
	session.state = SESSION_RUNNING;
	session.started_ms = monotonic_ms();
	session.overlay_tick_ms = monotonic_ms() + OVERLAY_TICK_MS;
	log_msg("emud: launch retroarch menu pid=%d user=%s", (int)pid, session.user);
	notify_subscribers();
	return NULL;
}

/*
 * RESET_RETROARCH (Settings → System → Recovery, user request 2026-10-27):
 * the active user's own RetroArch configuration back to the nuubOS
 * defaults: retroarch.cfg, the core options and config/ (core and game
 * overrides, remaps, the shader preset) go, and the next session seeds
 * them again. Saves, states, screenshots, playlists and nuubOS's Game
 * Settings stay.
 */
static void reset_retroarch(struct client *c)
{
	static const char *const files[] = {
		"retroarch.cfg", "retroarch-core-options.cfg", "core-option-updates", NULL,
	};
	char user[64];
	char path[PATH_MAX];
	pid_t pid;
	int status = -1;

	if (session.state != SESSION_IDLE) {
		reply(c, "ERR busy\n");
		return;
	}
	if (!read_active_user(user, sizeof(user))) {
		reply(c, "ERR no-user\n");
		return;
	}
	for (int i = 0; files[i]; i++) {
		snprintf(path, sizeof(path), USERS_DIR "/%s/appdata/retroarch/%s", user, files[i]);
		if (unlink(path) != 0 && errno != ENOENT) {
			reply(c, "ERR storage\n");
			return;
		}
	}
	snprintf(path, sizeof(path), USERS_DIR "/%s/appdata/retroarch/config", user);
	pid = fork();
	if (pid == 0) {
		execl("/bin/rm", "rm", "-rf", path, (char *)NULL);
		_exit(127);
	}
	while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		reply(c, "ERR storage\n");
		return;
	}
	sync();
	log_msg("emud: retroarch configuration of %s reset", user);
	reply(c, "OK\n");
}

/* The end of a disc change: the new disc goes in. */
static void close_disc_tray(void)
{
	session.disc_insert_ms = 0;
	ra_command("SET_DISK_EJECTED 0");
	ra_command("GET_DISK");
}

static void request_quit(void)
{
	if (session.state != SESSION_RUNNING)
		return;
	/* Never save or close with the tray open. */
	if (session.disc_insert_ms)
		close_disc_tray();
	/* Silent from here on (RetroArch patch 0011): the game runs unpaused
	 * while the quit state is written and the content closes, and its
	 * audio stuttered meanwhile (user report 2026-10-18). */
	ra_command("SET_MUTED 1");
	/* Not while a screenshot is being written: RetroArch's state save
	 * (whose thumbnail goes through the same screenshot path) then never
	 * completed, the quit waited for its 10 s timeout and the progress was
	 * not saved (2 of 3 quits right after a screenshot, 2026-10-22).
	 * finish_screenshot() comes back here once the PNG is written (or its
	 * 5 s timeout ran out). */
	if (session.shot_fd > 0) {
		session.quit_requested = true;
		notify_subscribers();
		return;
	}
	/* With slot rotation RetroArch's automatic slot is off: the progress
	 * goes into the next slot first (once; a failed save still quits). */
	if (session.settings.slot_rotation && !session.hardcore && !session.menu &&
	    !session.quit_saved) {
		session.quit_requested = true;
		notify_subscribers();
		if (session.pending != PENDING_NONE)
			return;
		session.quit_saved = true;
		if (save_next(true))
			return;
	}
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

/* Saves and states just changed: the automatic backup to the games card
 * runs now if it is on (docs/dev/platform.md "SD2 = games card"). The
 * helper only submits a job; it is reaped like any child. */
static void trigger_backup(void)
{
	pid_t pid = fork();

	if (pid == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
		}
		execl("/usr/sbin/nuubos-sd2-backup", "nuubos-sd2-backup", "trigger",
		      "session-end", (char *)NULL);
		_exit(127);
	}
}

static void child_exited(int status)
{
	long long ran = monotonic_ms() - session.started_ms;
	bool failed = (WIFEXITED(status) && WEXITSTATUS(status) != 0) ||
		      WIFSIGNALED(status);
	int power_client = session.power_client;
	/* Host Game: the same game again, from the progress its quit just
	 * saved, as a netplay host. */
	bool relaunch = session.relaunch_host && !failed && power_client < 0;
	char game[sizeof(session.game)];

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
	/* After RetroArch wrote its options at exit. */
	restore_session_core_options();
	copy_text(game, sizeof(game), session.game);
	reset_session();
	if (relaunch) {
		const char *error = launch(game, "resume", "host");

		if (!error)
			return;
		log_msg("emud: host relaunch of %s refused: %s", game, error);
		reset_session();
		notify_failed("start");
	}
	notify_subscribers();
	if (power_client >= 0 && clients[power_client].fd >= 0)
		reply(&clients[power_client], "OK\n");
	else
		trigger_backup();
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
		else if (pid == seed_pid)
			seed_pid = -1;
		else if (pid == raproxy_pid) {
			log_msg("emud: offline achievements proxy exited");
			raproxy_pid = -1;
			raproxy_user[0] = '\0';
		}
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
	if (discovery.fd >= 0 && (deadline < 0 || discovery.deadline_ms < deadline))
		deadline = discovery.deadline_ms;
	if (session.state == SESSION_RUNNING && (session.settings.overlay || game_cpu_watching()) &&
	    (deadline < 0 || session.overlay_tick_ms < deadline))
		deadline = session.overlay_tick_ms;
	if (session.state == SESSION_RUNNING && session.disc_insert_ms && !session.paused &&
	    (deadline < 0 || session.disc_insert_ms < deadline))
		deadline = session.disc_insert_ms;
	if (deadline < 0)
		return -1;
	if (deadline <= now)
		return 0;
	return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}

static void finish_discovery(void);

static void session_timers(void)
{
	long long now = monotonic_ms();

	if (session.shot_fd > 0 && now >= session.shot_deadline_ms)
		finish_screenshot(false);
	if (discovery.fd >= 0 && now >= discovery.deadline_ms)
		finish_discovery();
	/* Performance overlay: ask RetroArch for its frame rate; the reply
	 * updates the snapshot the Quick Menu draws from. Not while paused
	 * (the Quick Menu hides the overlay then). */
	if (session.state == SESSION_RUNNING && (session.settings.overlay || game_cpu_watching()) &&
	    now >= session.overlay_tick_ms) {
		if (!session.paused)
			ra_command("GET_FPS");
		session.overlay_tick_ms = now + OVERLAY_TICK_MS;
		game_cpu_update();
	}
	if (session.state == SESSION_RUNNING && session.disc_insert_ms && !session.paused &&
	    now >= session.disc_insert_ms)
		close_disc_tray();
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
	if (session.quit_requested && session.state == SESSION_RUNNING)
		request_quit();
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
/* melonDS-nuubOS screen layout, live (RetroArch 0009): the hotkey's next
 * one or the Quick Menu's Screen Layout choice. */
/* The game's layout and stylus as the user left them, for its next launch. */
static void save_nds_choice(void)
{
	struct game_overrides o;

	load_overrides(session.user, session.game, &o);
	copy_text(o.nds_layout, sizeof(o.nds_layout), nds_layouts[session.nds_layout].name);
	copy_text(o.stylus, sizeof(o.stylus), session.stylus ? session.stylus_control : "off");
	copy_text(o.nds_corner, sizeof(o.nds_corner), session.nds_corner ? nds_corners[session.nds_corner] : "");
	if (save_overrides(session.user, session.game, &o) != 0)
		log_msg("emud: saving the NDS layout of %s failed", session.game);
}

static void set_nds_layout(int layout)
{
	if (layout < 0 || layout >= NDS_LAYOUTS)
		return;
	session.nds_layout = layout;
	save_nds_choice();
	ra_command("SET_CORE_OPTION melonds_screen_layout %s",
		   nds_layouts[layout].value);
	notify_screens(layout);
	notify_subscribers();
}

/* Picture in Picture corner, live (Quick Menu, GitHub #5). */
static void set_nds_corner(int corner)
{
	if (corner < 0 || corner >= NDS_CORNERS)
		return;
	session.nds_corner = corner;
	save_nds_choice();
	ra_command("SET_CORE_OPTION melonds_nuubos_pip_corner %s", nds_corners[corner]);
	notify_subscribers();
}

static void handle_hotkey(const char *action)
{
	if (session.state != SESSION_RUNNING)
		return;
	log_msg("emud: hotkey %s", action);
	if (session.menu && strcmp(action, "quit") != 0)
		return;
	if (session.hardcore &&
	    (!strcmp(action, "save_state") || !strcmp(action, "load_state") ||
	     !strcmp(action, "slot_next") || !strcmp(action, "slot_prev") ||
	     !strcmp(action, "fast_forward"))) {
		notify_hardcore();
		return;
	}
	if (!strcmp(action, "save_state")) {
		if (save_next(false))
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
	} else if (!strcmp(action, "screen_layout")) {
		/* melonDS-nuubOS only: the next layout, live (RetroArch 0009). */
		if (strcmp(session.core, MELONDS_NUUBOS) != 0)
			return;
		set_nds_layout((session.nds_layout + 1) % NDS_LAYOUTS);
	} else if (!strncmp(action, "stylus", 6)) {
		/* "stylus <right|left|dpad>": inputd names the control of the
		 * controller used; the hotkey turns the stylus on and off. */
		const char *control = action[6] == ' ' ? action + 7 : "right";

		if (strcmp(session.core, MELONDS_NUUBOS) != 0) {
			log_msg("stylus hotkey ignored: core %s", session.core);
			return;
		}
		if (strcmp(control, "right") && strcmp(control, "left") && strcmp(control, "dpad"))
			control = "right";
		session.stylus = !session.stylus;
		copy_text(session.stylus_control, sizeof(session.stylus_control), control);
		ra_command("SET_CORE_OPTION melonds_nuubos_stylus %s",
			   session.stylus ? control : "off");
		notify_stylus(session.stylus ? control : "off");
		/* Community report 2026-10-25 ("the DS stylus does not work"): the
		 * control inputd chose for this controller is the first suspect. */
		log_msg("stylus %s (requested control %s)", session.stylus ? control : "off", action + 6);
		save_nds_choice();
		notify_subscribers();
	} else if (!strcmp(action, "fast_forward")) {
		session.fast_forward = !session.fast_forward;
		ra_command("FAST_FORWARD");
		if (!session.fast_forward)
			game_cpu_grace(GAME_CPU_GRACE_MS);
		game_cpu_update();
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
	if (session.state == SESSION_RUNNING && !session.menu)
		return true;
	reply(c, session.menu ? "ERR menu\n" : "ERR no-session\n");
	return false;
}

/* Any running session, the RetroArch menu included (QUIT, PAUSE, RESUME). */
static bool session_running(struct client *c)
{
	if (session.state == SESSION_RUNNING)
		return true;
	reply(c, "ERR no-session\n");
	return false;
}

/* Games of a system with their own value for a System Settings key; with
 * reset those values go (the game follows its system again; its layout,
 * stylus, cheats stay). */
static int system_games(const char *user, const char *system, bool reset)
{
	char dir[PATH_MAX];
	char path[PATH_MAX + 300];
	char command[96];
	char answer[PATH_MAX + 128];
	struct dirent *e;
	DIR *d;
	int n = 0;

	snprintf(dir, sizeof(dir), USERS_DIR "/%s/" GAME_SETTINGS_SUBDIR, user);
	if (!(d = opendir(dir)))
		return 0;
	while ((e = readdir(d))) {
		struct game_overrides o;
		char game[64];
		char *dot;
		size_t len;

		copy_text(game, sizeof(game), e->d_name);
		if (!(dot = strstr(game, ".conf")) || dot[5])
			continue;
		*dot = '\0';
		if (!valid_game(game))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		load_overrides_at(path, &o);
		if (!system_keys_set(&o))
			continue;
		snprintf(command, sizeof(command), "RESOLVE\t%s", game);
		len = strlen(system);
		if (library_request(command, answer, sizeof(answer)) != 0 || strncmp(answer, "OK ", 3) ||
		    strncmp(answer + 3, system, len) || answer[3 + len] != '\t')
			continue;
		n++;
		if (reset) {
			clear_system_keys(&o);
			if (save_overrides(user, game, &o) != 0)
				log_msg("emud: resetting the settings of %s failed", game);
		}
	}
	closedir(d);
	return n;
}

/* GAME_SETTINGS <game> <system> | GAME_SET <game> <system> <key> <value> |
 * GAME_RESET <game>; System Settings (GitHub #12): SYSTEM_SETTINGS
 * <system> | SYSTEM_SET <system> <key> <value> | SYSTEM_RESET <system> |
 * SYSTEM_RESET_GAMES <system> (the games' own values of these keys go);
 * for the active user. A game's unset key follows its system, a system's
 * the default. GAME_SETTINGS also lists the system's values (system_*),
 * SYSTEM_SETTINGS the games with their own values (games=). Controls:
 * pad= the system's buttons (pad_layouts), remap= / system_remap= the
 * controls set ("control:target,..."), keys remap_<control> and remap. */
static void handle_game_settings(struct client *c, const char *verb, char *arg)
{
	char *fields[4] = { arg, NULL, NULL, NULL };
	bool scope_system = !strncmp(verb, "SYSTEM_", 7);
	const char *game = NULL, *system;
	char *key, *value;
	char user[64];
	char cores[512];
	char def_core[64];
	char def_path[PATH_MAX];
	struct game_overrides o, sys;
	int n = 1;

	for (char *p = arg; *p && n < 4; p++)
		if (*p == '\t') {
			*p = '\0';
			fields[n++] = p + 1;
		}
	if (scope_system) {
		system = fields[0];
		key = fields[1];
		value = fields[2];
	} else {
		game = fields[0];
		system = fields[1];
		key = fields[2];
		value = fields[3];
		if (!valid_game(game)) {
			reply(c, "ERR game\n");
			return;
		}
	}
	if (!read_active_user(user, sizeof(user))) {
		reply(c, "ERR no-user\n");
		return;
	}
	if (!strcmp(verb, "GAME_RESET")) {
		memset(&o, 0, sizeof(o));
		reply(c, save_overrides(user, game, &o) == 0 ? "OK\n" : "ERR storage\n");
		return;
	}
	if (!system || !system[0] || strchr(system, '/') || system[0] == '.') {
		reply(c, "ERR system\n");
		return;
	}
	memset(&sys, 0, sizeof(sys));
	if (scope_system) {
		load_system_overrides(user, system, &o);
	} else {
		load_overrides(user, game, &o);
		load_system_overrides(user, system, &sys);
	}
	if (!strcmp(verb, "SYSTEM_RESET")) {
		memset(&o, 0, sizeof(o));
		reply(c, save_system_overrides(user, system, &o) == 0 ? "OK\n" : "ERR storage\n");
		return;
	}
	if (!strcmp(verb, "SYSTEM_RESET_GAMES")) {
		char out[32];

		snprintf(out, sizeof(out), "OK %d\n", system_games(user, system, true));
		reply(c, out);
		return;
	}
	available_cores(system, cores, sizeof(cores));
	if (!strcmp(verb, "GAME_SETTINGS") || !strcmp(verb, "SYSTEM_SETTINGS")) {
		char out[3072];
		char remap[512], system_remap[512];
		char accuracy_default[16];
		char res_value[64];
		const char *res_default = "";
		const struct resolution_option *res;
		const struct frameskip_option *fs;
		const char *effective;
		struct emu_settings settings;
		char screen[16];
		int len;

		/* filter_default: what an empty filter gives (write_presentation) */
		load_settings(user, &settings);
		system_screen(system, screen, sizeof(screen));
		def_core[0] = '\0';
		(void)resolve_core(system, def_core, sizeof(def_core), def_path, sizeof(def_path));
		/* A game's default emulator is its system's choice. */
		if (sys.core[0] && list_has(cores, sys.core))
			copy_text(def_core, sizeof(def_core), sys.core);
		effective = o.core[0] && list_has(cores, o.core) ? o.core : def_core;
		/* Emulation accuracy: melonDS-nuubOS and Flycast; the default is
		 * the user's core option (nuubOS ships "fast" for melonDS, per
		 * triangle sorting = "accurate" for Flycast, which ships per strip). */
		snprintf(def_path, sizeof(def_path),
			 USERS_DIR "/%s/appdata/retroarch/retroarch-core-options.cfg", user);
		if (flycast_core(effective)) {
			char option[64];

			copy_text(accuracy_default, sizeof(accuracy_default),
				  read_core_option(def_path, FLYCAST_SORTING, option, sizeof(option)) &&
				  !strcmp(option, FLYCAST_SORTING_ACCURATE) ? "accurate" : "fast");
		} else if (!read_core_option(def_path, "melonds_accuracy", accuracy_default,
					     sizeof(accuracy_default))) {
			copy_text(accuracy_default, sizeof(accuracy_default), "fast");
		}
		/* Render resolution: what Auto gives (auto = by the output, else
		 * the user's core option as 1x, 2x or custom; missing = the core's
		 * native default). */
		res = resolution_option(effective);
		if (res && res->auto_hdmi)
			res_default = "auto";
		else if (res && read_core_option(def_path, res->key, res_value, sizeof(res_value)))
			res_default = !strcmp(res_value, res->x1) ? "1x" :
				      !strcmp(res_value, res->x2) ? "2x" : "custom";
		else if (res)
			res_default = "1x";
		fs = frameskip_option(effective);
		remap_list(&o, remap, sizeof(remap));
		remap_list(&sys, system_remap, sizeof(system_remap));
		len = snprintf(out, sizeof(out),
			       "core=%s\ncore_default=%s\ncore_override=%s\ncores=%s\n"
			       "aspect=%s\nfilter=%s\nfilter_default=%s\n"
			       "accuracy_supported=%d\naccuracy=%s\naccuracy_default=%s\n"
			       "pak_supported=%d\npak=%s\n"
			       "resolution_supported=%d\nresolution=%s\nresolution_default=%s\n"
			       "frameskip_supported=%d\nframeskip=%s\nframeskip_default=%s\n"
			       "system_core=%s\nsystem_aspect=%s\nsystem_filter=%s\nsystem_accuracy=%s\n"
			       "system_pak=%s\nsystem_resolution=%s\nsystem_frameskip=%s\ngames=%d\n"
			       "pad=%s\nremap=%s\nsystem_remap=%s\nend=1\n",
			       effective, def_core, o.core, cores, o.aspect, o.filter,
			       settings.crt && !strcmp(screen, "crt") ? "crt" : "sharp",
			       !strcmp(effective, MELONDS_NUUBOS) || flycast_core(effective),
			       o.accuracy, accuracy_default,
			       !strcmp(system, "n64"), o.n64_pak,
			       res != NULL, o.resolution, res_default,
			       fs != NULL, o.frameskip, fs ? frameskip_default(fs, def_path) : "",
			       sys.core[0] && list_has(cores, sys.core) ? sys.core : "", sys.aspect, sys.filter,
			       sys.accuracy, sys.n64_pak, sys.resolution, sys.frameskip,
			       scope_system ? system_games(user, system, false) : 0,
			       system_pad(system), remap, system_remap);
		if (len > 0 && write_all(c->fd, out, (size_t)len) != 0)
			close_client(c);
		return;
	}
	/* GAME_SET / SYSTEM_SET; an empty value (inherit) arrives without its
	 * field: the request line loses its trailing tab with the other white
	 * space. */
	if (!key) {
		reply(c, "ERR args\n");
		return;
	}
	if (!value)
		value = key + strlen(key);
	if (!strcmp(key, "core") && (!value[0] || list_has(cores, value)))
		copy_text(o.core, sizeof(o.core), value);
	else if (!strcmp(key, "aspect") && (!value[0] || valid_aspect(value)))
		copy_text(o.aspect, sizeof(o.aspect), value);
	else if (!strcmp(key, "filter") && (!value[0] || valid_filter(value)))
		copy_text(o.filter, sizeof(o.filter), value);
	else if (!strcmp(key, "accuracy") &&
		 (!value[0] || !strcmp(value, "fast") || !strcmp(value, "accurate")))
		copy_text(o.accuracy, sizeof(o.accuracy), value);
	else if (!strcmp(key, "pak") &&
		 (!value[0] || !strcmp(value, "rumble") || !strcmp(value, "memory")))
		/* A game under a system set to Controller Pak keeps an explicit
		 * Rumble Pak; a system's rumble is the default (empty). */
		copy_text(o.n64_pak, sizeof(o.n64_pak),
			  !strcmp(value, "rumble") && (scope_system || !sys.n64_pak[0]) ? "" : value);
	else if (!strcmp(key, "resolution") &&
		 (!value[0] || !strcmp(value, "1x") || !strcmp(value, "2x")))
		copy_text(o.resolution, sizeof(o.resolution), value);
	else if (!strcmp(key, "frameskip") && (!value[0] || frameskip_mode(value) >= 0))
		copy_text(o.frameskip, sizeof(o.frameskip), value);
	/* Controls: "remap" clears them all (Reset Controls); remap_<control>
	 * takes another control, "none" or "" (inherited). */
	else if (!strcmp(key, "remap") && !value[0])
		memset(o.remap, 0, sizeof(o.remap));
	else if (!strncmp(key, "remap_", 6) && remap_index(key + 6) >= 0 &&
		 (!value[0] || !strcmp(value, "none") || remap_index(value) >= 0))
		copy_text(o.remap[remap_index(key + 6)], sizeof(o.remap[0]), value);
	else {
		reply(c, "ERR value\n");
		return;
	}
	if (scope_system)
		reply(c, save_system_overrides(user, system, &o) == 0 ? "OK\n" : "ERR storage\n");
	else
		reply(c, save_overrides(user, game, &o) == 0 ? "OK\n" : "ERR storage\n");
}

/* Cheats are refused in hardcore mode and with netplay. */
static const char *cheats_blocked(void)
{
	if (session.hardcore)
		return "hardcore";
	if (session.net_launch[0] || session.netplay != NET_OFF)
		return "netplay";
	return NULL;
}

/* CHEATS: the running game's cheat lists and the cheats of the current
 * one: "available=0|1", "reason=" (hardcore|netplay), "file=<path>",
 * "list=<path>\t<name>" per list, "cheat=<k>\t<0|1>\t<description>" per
 * cheat, "end=1". */
static void handle_cheats(struct client *c)
{
	struct cheat_list lists[MAX_CHEAT_LISTS];
	struct game_overrides o;
	char file[PATH_MAX];
	char line[2048];
	const char *blocked = cheats_blocked();
	int count;
	int total = 0;
	bool *seen = NULL;
	FILE *fp;

	if (!running(c))
		return;
	count = cheat_lists(session.system, session.rom, lists);
	load_overrides(session.user, session.game, &o);
	current_cheat_file(&o, lists, count, file, sizeof(file));
	snprintf(line, sizeof(line), "available=%d\nreason=%s\nfile=%s\n", blocked ? 0 : 1,
		 blocked ? blocked : "", file);
	if (write_all(c->fd, line, strlen(line)) != 0)
		goto gone;
	for (int i = 0; i < count; i++) {
		snprintf(line, sizeof(line), "list=%s\t%s\n", lists[i].path, lists[i].name);
		if (write_all(c->fd, line, strlen(line)) != 0)
			goto gone;
	}
	if (file[0] && (fp = fopen(file, "r"))) {
		char buf[1024];

		while (fgets(buf, sizeof(buf), fp))
			if (sscanf(buf, " cheats = %d", &total) == 1 || sscanf(buf, " cheats = \"%d\"", &total) == 1)
				break;
		/* A list of at most 1000 cheats is shown. */
		if (total > 1000)
			total = 1000;
		if (total > 0)
			seen = calloc((size_t)total, sizeof(*seen));
		rewind(fp);
		while (seen && fgets(buf, sizeof(buf), fp)) {
			int k;
			char *field = cheat_line(buf, &k);
			char *desc;
			char *end;
			char key[16];

			if (!field || strncmp(field, "desc", 4) || k >= total || seen[k] ||
			    !(desc = strchr(field, '"')))
				continue;
			desc++;
			trim(desc);
			if ((end = strrchr(desc, '"')))
				*end = '\0';
			for (char *q = desc; *q; q++)
				if (*q == '\t' || (unsigned char)*q < 0x20)
					*q = ' ';
			if (strlen(desc) > 120)
				desc[120] = '\0';
			seen[k] = true;
			snprintf(key, sizeof(key), "%d", k);
			snprintf(line, sizeof(line), "cheat=%d\t%d\t%s\n", k, list_has(o.cheats, key) ? 1 : 0, desc);
			if (write_all(c->fd, line, strlen(line)) != 0) {
				fclose(fp);
				free(seen);
				goto gone;
			}
		}
		fclose(fp);
		/* Cheats without a description still count. */
		for (int k = 0; seen && k < total; k++) {
			char key[16];

			if (seen[k])
				continue;
			snprintf(key, sizeof(key), "%d", k);
			snprintf(line, sizeof(line), "cheat=%d\t%d\t\n", k, list_has(o.cheats, key) ? 1 : 0);
			if (write_all(c->fd, line, strlen(line)) != 0) {
				free(seen);
				goto gone;
			}
		}
		free(seen);
	}
	if (write_all(c->fd, "end=1\n", 6) == 0)
		return;
gone:
	close_client(c);
}

/* CHEAT_SET <k> <0|1>: turns cheat k of the current list on or off for
 * the running game (kept for the user and the game), applied at once.
 * CHEAT_FILE <path>: picks another of the game's lists (its cheats start
 * off). */
static void handle_cheat_change(struct client *c, const char *verb, char *arg)
{
	struct cheat_list lists[MAX_CHEAT_LISTS];
	struct game_overrides o;
	char file[PATH_MAX];
	const char *blocked;
	int count;

	if (!running(c))
		return;
	if ((blocked = cheats_blocked())) {
		char text[32];

		snprintf(text, sizeof(text), "ERR %s\n", blocked);
		reply(c, text);
		return;
	}
	count = cheat_lists(session.system, session.rom, lists);
	load_overrides(session.user, session.game, &o);
	if (!strcmp(verb, "CHEAT_FILE")) {
		bool known = false;

		for (int i = 0; i < count && !known; i++)
			known = !strcmp(lists[i].path, arg);
		if (!known) {
			reply(c, "ERR list\n");
			return;
		}
		copy_text(o.cheat_file, sizeof(o.cheat_file), arg);
		o.cheats[0] = '\0';
	} else {
		char *value = strchr(arg, '\t');
		char *end;
		long k = strtol(arg, &end, 10);
		char list[sizeof(o.cheats)];
		char *save = NULL;
		char key[16];
		size_t len = 0;

		if (value)
			*value++ = '\0';
		current_cheat_file(&o, lists, count, file, sizeof(file));
		if (*end || k < 0 || k >= 1000 || !value || (strcmp(value, "0") && strcmp(value, "1"))) {
			reply(c, "ERR args\n");
			return;
		}
		if (!file[0]) {
			reply(c, "ERR none\n");
			return;
		}
		/* The list the cheats belong to is kept with them. */
		copy_text(o.cheat_file, sizeof(o.cheat_file), file);
		snprintf(key, sizeof(key), "%ld", k);
		copy_text(list, sizeof(list), o.cheats);
		o.cheats[0] = '\0';
		for (char *w = strtok_r(list, ",", &save); w; w = strtok_r(NULL, ",", &save)) {
			if (!strcmp(w, key))
				continue;
			len += (size_t)snprintf(o.cheats + len, sizeof(o.cheats) - len, "%s%s", len ? "," : "", w);
		}
		if (!strcmp(value, "1")) {
			if (len + strlen(key) + 2 >= sizeof(o.cheats)) {
				reply(c, "ERR full\n");
				return;
			}
			snprintf(o.cheats + len, sizeof(o.cheats) - len, "%s%s", len ? "," : "", key);
		}
	}
	if (save_overrides(session.user, session.game, &o) != 0) {
		reply(c, "ERR storage\n");
		return;
	}
	current_cheat_file(&o, lists, count, file, sizeof(file));
	apply_session_cheats(&o, file);
	reply(c, "OK\n");
}

/* SETTINGS (key, value NULL) lists the active user's settings; SET key
 * value changes one. Hotkeys, the overlay and the RetroArch row follow at
 * once in a running game of that user; the rest applies at the next launch. */
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
	session.settings.retroarch_menu = e.retroarch_menu;
	if (!strncmp(key, "hotkey_", 7))
		send_hotkeys();
	session.overlay_tick_ms = monotonic_ms();
	notify_subscribers();
}

/* STATES [game]: the save states of the running game, or of a game before
 * it starts (Home's start menu): "slots=N", then "state=<slot>\t<mtime>\t
 * <HH:MM local>" for each existing state (slot -1 = the automatic state),
 * "end=1". In hardcore mode "hardcore=1" and no states (none can be
 * loaded). */
static void handle_states(struct client *c, const char *game)
{
	char base[PATH_MAX];
	char out[4096];
	int slots;
	bool hardcore;
	size_t len;

	if (game && game[0]) {
		char user[64];
		char command[64];
		char answer[PATH_MAX + 128];
		char name[NAME_MAX + 1];
		char folder[NAME_MAX + 1];
		char *tab;
		struct emu_settings e;
		struct achievements a;

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
		load_achievements(user, &a);
		hardcore = a.enabled && a.hardcore;
		content_base_name(tab + 1, name, sizeof(name));
		if (psp_folder_game(answer + 3, tab + 1, folder, sizeof(folder)))
			snprintf(base, sizeof(base), USERS_DIR "/%s/states/%s/%s/%s", user, answer + 3,
				 folder, name);
		else
			snprintf(base, sizeof(base), USERS_DIR "/%s/states/%s/%s", user, answer + 3, name);
	} else if (session.state == SESSION_RUNNING) {
		slots = session.settings.slots;
		hardcore = session.hardcore;
		copy_text(base, sizeof(base), session.state_base);
	} else {
		reply(c, "ERR no-session\n");
		return;
	}
	len = (size_t)snprintf(out, sizeof(out), "slots=%d\n%s", slots,
			       hardcore ? "hardcore=1\n" : "");
	for (int slot = AUTO_SLOT; !hardcore && slot < slots && len + 48 < sizeof(out); slot++) {
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

/* ------------------------------------------------------------------ */
/* Netplay hosts on the local network                                  */
/* ------------------------------------------------------------------ */

/* RetroArch's LAN discovery: a "RANQ" broadcast to the netplay port, each
 * host answers with this packet (network/netplay/netplay_frontend.c). */
#define DISCOVERY_QUERY 0x52414E51u
#define DISCOVERY_RESPONSE 0x52414E53u
struct ad_packet {
	uint32_t header;
	int32_t content_crc;
	int32_t port;
	uint32_t has_password;
	char nick[32];
	char frontend[32];
	char core[32];
	char core_version[32];
	char retroarch_version[32];
	char content[256];
	char subsystem_name[256];
};

static void start_discovery(struct client *c)
{
	if (discovery.fd < 0) {
		struct sockaddr_in to;
		uint32_t query = htonl(DISCOVERY_QUERY);
		int on = 1;
		int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_port = htons(NETPLAY_PORT);
		to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0 ||
		    sendto(fd, &query, sizeof(query), 0, (struct sockaddr *)&to, sizeof(to)) !=
			    (ssize_t)sizeof(query)) {
			if (fd >= 0)
				close(fd);
			reply(c, "ERR network\n");
			return;
		}
		memset(discovery.waiting, 0, sizeof(discovery.waiting));
		discovery.fd = fd;
		discovery.count = 0;
		discovery.deadline_ms = monotonic_ms() + DISCOVERY_MS;
	}
	discovery.waiting[c - clients] = true;
}

static void process_discovery(void)
{
	struct ad_packet ad;
	struct sockaddr_in from;
	socklen_t len = sizeof(from);
	ssize_t n;

	while ((n = recvfrom(discovery.fd, &ad, sizeof(ad), 0, (struct sockaddr *)&from, &len)) >= 0) {
		char address[INET_ADDRSTRLEN];
		int port = (int)ntohl((uint32_t)ad.port);
		bool known = false;

		len = sizeof(from);
		if (n != (ssize_t)sizeof(ad) || ntohl(ad.header) != DISCOVERY_RESPONSE ||
		    from.sin_family != AF_INET || port < 1 || port > 65535 ||
		    !inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address)))
			continue;
		for (int i = 0; i < discovery.count && !known; i++)
			known = !strcmp(discovery.hosts[i].address, address) && discovery.hosts[i].port == port;
		if (known || discovery.count >= (int)(sizeof(discovery.hosts) / sizeof(discovery.hosts[0])))
			continue;
		ad.nick[sizeof(ad.nick) - 1] = '\0';
		ad.core[sizeof(ad.core) - 1] = '\0';
		ad.content[sizeof(ad.content) - 1] = '\0';
		copy_text(discovery.hosts[discovery.count].address, INET_ADDRSTRLEN, address);
		discovery.hosts[discovery.count].port = port;
		discovery.hosts[discovery.count].password = ntohl(ad.has_password) & 1;
		copy_text(discovery.hosts[discovery.count].nick, 32, ad.nick);
		copy_text(discovery.hosts[discovery.count].core, 32, ad.core);
		copy_text(discovery.hosts[discovery.count].content, 256, ad.content);
		discovery.count++;
	}
}

/* Text safe for one tab separated field. */
static void field_safe(char *text)
{
	for (char *p = text; *p; p++)
		if (*p == '\t' || (unsigned char)*p < 0x20)
			*p = ' ';
}

/* NETPLAY_HOSTS answer, once the hosts had DISCOVERY_MS to reply:
 * "host=<address>\t<port>\t<nick>\t<emulator name>\t<content file>\t
 * <game id>\t<system>\t<our core>\t<password 0|1>" per host ("" game: not
 * in this library; "" core: the host's emulator is not on this console),
 * then "end=1". */
static void finish_discovery(void)
{
	char out[8192];
	size_t len = 0;

	if (discovery.fd >= 0)
		process_discovery();
	for (int i = 0; i < discovery.count; i++) {
		char command[300];
		char answer[256] = "";
		char game[32] = "";
		char system[64] = "";
		char core[64] = "";
		char *tab;

		field_safe(discovery.hosts[i].nick);
		field_safe(discovery.hosts[i].core);
		field_safe(discovery.hosts[i].content);
		snprintf(command, sizeof(command), "MATCH\t%s", discovery.hosts[i].content);
		if (library_request(command, answer, sizeof(answer)) == 0 &&
		    !strncmp(answer, "OK ", 3) && (tab = strchr(answer + 3, '\t'))) {
			char cores[512];
			char *save = NULL;

			*tab = '\0';
			copy_text(game, sizeof(game), answer + 3);
			copy_text(system, sizeof(system), tab + 1);
			/* Netplay needs the host's emulator (its core name). */
			available_cores(system, cores, sizeof(cores));
			for (char *name = strtok_r(cores, ",", &save); name && !core[0];
			     name = strtok_r(NULL, ",", &save)) {
				char corename[64];

				core_info_value(name, "corename", corename, sizeof(corename));
				if (corename[0] && !strcasecmp(corename, discovery.hosts[i].core))
					copy_text(core, sizeof(core), name);
			}
		}
		len += (size_t)snprintf(out + len, sizeof(out) - len,
					"host=%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n",
					discovery.hosts[i].address, discovery.hosts[i].port,
					discovery.hosts[i].nick, discovery.hosts[i].core,
					discovery.hosts[i].content, game, system, core,
					discovery.hosts[i].password ? 1 : 0);
		if (len >= sizeof(out) - 16)
			break;
	}
	len += (size_t)snprintf(out + len, sizeof(out) - len, "end=1\n");
	if (discovery.fd >= 0)
		close(discovery.fd);
	discovery.fd = -1;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (!discovery.waiting[i])
			continue;
		discovery.waiting[i] = false;
		if (clients[i].fd >= 0 && write_all(clients[i].fd, out, len) != 0)
			close_client(&clients[i]);
	}
}

static void handle_command(struct client *c, char *line)
{
	char *arg = strchr(line, '\t');
	char status[768];

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
	} else if (!strcmp(line, "MENU")) {
		const char *error = launch_menu();

		if (error) {
			char text[64];

			log_msg("emud: menu refused: %s", error);
			snprintf(text, sizeof(text), "ERR %s\n", error);
			reply(c, text);
		} else {
			reply(c, "OK\n");
		}
	} else if (!strcmp(line, "RESET_RETROARCH")) {
		reset_retroarch(c);
	} else if (!strcmp(line, "LAUNCH") && arg) {
		char *mode = strchr(arg, '\t');
		char *net = NULL;
		const char *error;

		if (mode) {
			*mode++ = '\0';
			if ((net = strchr(mode, '\t')))
				*net++ = '\0';
		}
		error = launch(arg, mode, net);

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
		if (!session_running(c))
			return;
		ra_command("SET_PAUSED %d", !strcmp(line, "PAUSE") ? 1 : 0);
		/* The Quick Menu opens: its Disc row follows the reply. */
		if (!strcmp(line, "PAUSE") && !session.menu)
			ra_command("GET_DISK");
		/* A disc change waits for the game to run with the tray open. */
		if (!strcmp(line, "RESUME") && session.disc_insert_ms)
			session.disc_insert_ms = monotonic_ms() + DISC_INSERT_MS;
		reply(c, "OK\n");
	} else if (!strcmp(line, "DISC") && arg) {
		/* DISC <index> (Quick Menu Disc row): open the tray, change the
		 * disc, close it once the game has run DISC_INSERT_MS (unpaused). */
		char *end;
		long index = strtol(arg, &end, 10);

		if (!running(c))
			return;
		if (*end || index < 0 || index >= session.discs) {
			reply(c, "ERR disc\n");
			return;
		}
		reply(c, "OK\n");
		if (index == session.disc && !session.disc_insert_ms)
			return;
		ra_command("SET_DISK_EJECTED 1");
		ra_command("SET_DISK_INDEX %ld", index);
		session.disc = (int)index;
		session.disc_insert_ms = monotonic_ms() + DISC_INSERT_MS;
		log_msg("emud: disc %ld", index + 1);
		notify_subscribers();
	} else if (session.hardcore && session.state == SESSION_RUNNING &&
		   (!strcmp(line, "SAVE_STATE") || !strcmp(line, "LOAD_STATE") ||
		    !strcmp(line, "SLOT") || !strcmp(line, "FAST_FORWARD"))) {
		notify_hardcore();
		reply(c, "ERR hardcore\n");
	} else if (!strcmp(line, "SAVE_STATE")) {
		if (!running(c))
			return;
		if (!save_next(false)) {
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
		if (!session_running(c))
			return;
		request_quit();
		reply(c, "OK\n");
	} else if (!strcmp(line, "CHEATS")) {
		handle_cheats(c);
	} else if ((!strcmp(line, "CHEAT_SET") || !strcmp(line, "CHEAT_FILE")) && arg) {
		handle_cheat_change(c, line, arg);
	} else if (!strcmp(line, "NETPLAY_HOST")) {
		/* Host Game over a running game: it closes (its progress saved
		 * like at every quit) and starts again from there as a host. */
		if (!running(c))
			return;
		if (session.hardcore) {
			notify_hardcore();
			reply(c, "ERR hardcore\n");
			return;
		}
		if (session.net_launch[0] || session.netplay != NET_OFF) {
			reply(c, "ERR busy\n");
			return;
		}
		session.relaunch_host = true;
		request_quit();
		reply(c, "OK\n");
	} else if (!strcmp(line, "NETPLAY_STOP")) {
		/* Stop hosting or leave the host; the game goes on alone. */
		if (!running(c))
			return;
		if (!session.net_launch[0] && session.netplay == NET_OFF) {
			reply(c, "ERR netplay\n");
			return;
		}
		ra_command("SET_PAUSED 0");
		ra_command("NETPLAY_STOP");
		reply(c, "OK\n");
	} else if (!strcmp(line, "NETPLAY_HOSTS")) {
		start_discovery(c);
	} else if (!strcmp(line, "SCREENSHOT")) {
		if (!running(c))
			return;
		reply(c, begin_screenshot() ? "OK\n" : "ERR busy\n");
	} else if (!strcmp(line, "NDS_LAYOUT") && arg) {
		/* NDS_LAYOUT <index of nds_layouts> (Quick Menu Screen Layout). */
		if (!running(c))
			return;
		if (strcmp(session.core, MELONDS_NUUBOS) != 0) {
			reply(c, "ERR core\n");
			return;
		}
		set_nds_layout(atoi(arg));
		reply(c, "OK\n");
	} else if (!strcmp(line, "NDS_CORNER") && arg) {
		/* NDS_CORNER <index of nds_corners> (Quick Menu, Picture in
		 * Picture only). */
		if (!running(c))
			return;
		if (strcmp(session.core, MELONDS_NUUBOS) != 0) {
			reply(c, "ERR core\n");
			return;
		}
		set_nds_corner(atoi(arg));
		reply(c, "OK\n");
	} else if (!strcmp(line, "HOTKEY") && arg) {
		/* A game hotkey by name, as inputd sends it (tests, remote). */
		if (!running(c))
			return;
		handle_hotkey(arg);
		reply(c, "OK\n");
	} else if ((!strcmp(line, "GAME_SETTINGS") || !strcmp(line, "GAME_SET") ||
		    !strcmp(line, "GAME_RESET") || !strcmp(line, "SYSTEM_SETTINGS") ||
		    !strcmp(line, "SYSTEM_SET") || !strcmp(line, "SYSTEM_RESET") ||
		    !strcmp(line, "SYSTEM_RESET_GAMES")) && arg) {
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
	} else if (!strcmp(line, "STORAGE_LOST") && arg) {
		size_t len = strlen(arg);

		reply(c, "OK\n");
		if (session.state != SESSION_RUNNING || !len ||
		    strncmp(session.rom, arg, len) || session.rom[len] != '/')
			return;
		log_msg("emud: the ROM's storage %s went away, saving and closing", arg);
		notify_storage_lost();
		/* Saves and states live on SD1: the core still has the game in
		 * memory, so the automatic state is taken first (not in
		 * hardcore), then the session ends like before a power off.
		 * A core stuck reading the missing card is killed by the quit
		 * timeout. */
		if (!session.hardcore && session.pending == PENDING_NONE) {
			if (!session.settings.slot_rotation)
				(void)begin_save(AUTO_SLOT, true);
			else if (save_next(true))
				session.quit_saved = true;
		}
		session.quit_requested = true;
		if (session.pending == PENDING_NONE)
			request_quit();
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
		if (session.disc_insert_ms)
			close_disc_tray();
		/* Progress is safe before every power action (CLAUDE §20.1):
		 * in the automatic slot, or with slot rotation in the next slot
		 * (the quit before restart/poweroff then does not save again);
		 * a state operation already running completes first. Hardcore
		 * sessions have no states: SRAM is written by the quit. */
		if (session.state == SESSION_RUNNING && !session.hardcore &&
		    !session.menu && session.pending == PENDING_NONE) {
			if (!session.settings.slot_rotation)
				(void)begin_save(AUTO_SLOT, true);
			else if (save_next(true) && strcmp(arg, "sleep") != 0)
				session.quit_saved = true;
		}
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
	sync_low_latency(); /* a mark left by a crashed emud */
	if (mkdir_p(BIOS_DIR) == 0)
		start_system_seed();

	while (!stop_requested) {
		struct pollfd pfd[7 + MAX_CLIENTS];
		int who[7 + MAX_CLIENTS];
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
		if (discovery.fd >= 0) {
			pfd[count] = (struct pollfd){ discovery.fd, POLLIN, 0 };
			who[count++] = -7;
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
			case -7:
				if (discovery.fd >= 0)
					process_discovery();
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

		ra_command("SET_MUTED 1");
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
