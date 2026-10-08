/* SPDX-License-Identifier: MIT */
/*
 * nuubos-lightingd — Product Lighting service (RGB around the sticks).
 *
 * Owns every product decision about the LEDs and renders every frame;
 * nuubos-rgbd is only the hardware frame sink. Two layers:
 *
 *   Effect   the user's ambient lighting (per user): off, solid, breathe,
 *            orbit, aurora, spectrum, battery (charge gauge) and game
 *            (colour of the running game's system, systems.conf). Colour is
 *            the active theme's accent or a fixed RRGGBB; speed slow,
 *            normal or fast; brightness 10..100.
 *   Signals  short system animations over the effect, grouped in five
 *            per-user categories: power (welcome, sleep, wake, power off),
 *            battery, connections, games, tasks. Their sources are the
 *            typed notification events of nuubos-notifyd (EPIC-006) — one
 *            event vocabulary for the screen and the LEDs — plus the
 *            lifecycle calls of nuubos-systemd. Each signal has one shape
 *            and one colour role (accent, success, warning, error,
 *            neutral) so the same meaning always looks the same.
 *
 * D-Bus org.nuubOS.Lighting /org/nuubOS/Lighting org.nuubOS.Lighting1:
 *   GetState() -> (b supported, s effect, s color, i brightness, s speed,
 *                  u signals, s active_signal)   signals: bit 0 power,
 *                  1 battery, 2 connections, 3 games, 4 tasks
 *   SetEffect(s) SetColor(s "theme"|"rrggbb") SetBrightness(i) SetSpeed(s)
 *   SetSignal(s category, b enabled)   enabling plays the category's signal
 *   PreviewSignal(s category)
 *   Preview(s effect, s color) / EndPreview()   live, never persisted
 *   Lifecycle(s "sleep"|"wake"|"shutdown")   sleep replies once dark
 *   signal StateChanged(same as GetState)
 * Preferences: /state/users/<id>/lighting.conf (FORMAT=2), atomic writes.
 *
 * Idle: poll() without timeout. The frame timer (30 fps) runs only while
 * something moves; static effects are written once.
 */

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SERVICE_NAME "org.nuubOS.Lighting"
#define OBJECT_PATH "/org/nuubOS/Lighting"
#define INTERFACE_NAME "org.nuubOS.Lighting1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"
#define ERROR_INVALID "org.nuubOS.Lighting.Error.InvalidArgument"

#define RUNTIME_DIR "/run/nuubos"
#define RGB_SOCKET RUNTIME_DIR "/rgbd.sock"
#define STATUS_SOCKET RUNTIME_DIR "/statusd.sock"
#define STATUS_STATE RUNTIME_DIR "/statusd.state"
#define NOTIFY_SOCKET RUNTIME_DIR "/notifyd.sock"
#define EMU_SOCKET RUNTIME_DIR "/emud.sock"
#define BOOT_MARKER RUNTIME_DIR "/lighting-welcomed"
#define USER_DIR RUNTIME_DIR "/user"
#define ACTIVE_USER USER_DIR "/active"
#define SYSTEMS_CONF "/usr/share/nuubos/systems.conf"
#define BUILTIN_THEMES "/usr/share/nuubos/themes"
#define USER_THEMES "/userdata/themes"

#define MAX_LEDS 32
#define MAX_ZONES 4
#define FRAME_MS 33
#define CROSSFADE_MS 350
#define DEFAULT_BRIGHTNESS 50

enum {
	SIG_POWER = 1u << 0,
	SIG_BATTERY = 1u << 1,
	SIG_CONNECTIONS = 1u << 2,
	SIG_GAMES = 1u << 3,
	SIG_TASKS = 1u << 4,
	SIG_ALL = 0x1f,
};

static const char *const category_names[] = {
	"power", "battery", "connections", "games", "tasks",
};
#define CATEGORY_COUNT 5

static const char *const effect_names[] = {
	"off", "solid", "breathe", "orbit", "aurora", "spectrum", "battery", "game",
};
#define EFFECT_COUNT 8

struct rgb {
	float r, g, b;
};

/* Colour roles of the signals (the accent comes from the theme). */
static const struct rgb ROLE_SUCCESS = { 0.20f, 0.95f, 0.42f };
static const struct rgb ROLE_WARNING = { 1.00f, 0.60f, 0.06f };
static const struct rgb ROLE_ERROR = { 1.00f, 0.10f, 0.08f };
static const struct rgb ROLE_NEUTRAL = { 0.85f, 0.88f, 0.95f };
static const struct rgb BLACK = { 0, 0, 0 };

enum shape {
	SHAPE_NONE,
	SHAPE_WELCOME,  /* comet runs one lap, blooms to the full ring */
	SHAPE_WAKE,     /* light blooms from position 0 */
	SHAPE_SLEEP,    /* everything fades to dark */
	SHAPE_FAREWELL, /* full ring drains back to position 0 */
	SHAPE_LINK,     /* two arcs meet; optionally the player number */
	SHAPE_UNLINK,   /* the ring retracts to position 0 */
	SHAPE_PULSE,    /* N full-ring pulses */
	SHAPE_FILL,     /* gauge fills to a level */
	SHAPE_GLOW,     /* one slow breath */
	SHAPE_SPIN,     /* one fast lap, direction = dir */
	SHAPE_FLASH,    /* camera flash */
	SHAPE_TICK,     /* one position marked (save slot) */
};

struct signal_play {
	enum shape shape;
	char name[32];
	struct rgb color;
	int prio;
	long long start_ms;
	int duration_ms;
	int count;
	int dir;
	float level;
};

struct prefs {
	char effect[16];
	char color[8]; /* "theme" or rrggbb */
	int brightness;
	char speed[8];
	unsigned int signals;
};

enum phase { PHASE_AWAKE, PHASE_SLEEP, PHASE_SHUTDOWN };

struct stream {
	int fd;
	size_t used;
	char buf[2048];
};

static volatile sig_atomic_t running = 1;
static char active_user[128] = "default";
static struct prefs prefs;

/* Hardware. */
static bool supported;
static int led_count;
static int zone_count;
static int zone_size[MAX_ZONES];
static int rgb_fd = -1;

/* Context. */
static struct rgb theme_accent = { 58 / 255.0f, 134 / 255.0f, 1.0f };
static int battery_percent = -1;
static bool battery_charging;
static bool game_running;
static struct rgb game_color;
static bool game_color_known;

/* Rendering. */
static enum phase phase = PHASE_AWAKE;
static bool preview_active;
static char preview_effect[16];
static char preview_color[8];
static long long effect_epoch_ms;
static struct signal_play sig;
static struct rgb last_out[MAX_LEDS];
static struct rgb fade_from[MAX_LEDS];
static long long fade_start_ms = -1;
static uint8_t sent[MAX_LEDS * 3];
static bool sent_valid;
static bool hardware_off = true;
static DBusMessage *pending_sleep;
static DBusConnection *bus;

static struct stream status_stream = { .fd = -1 };
static struct stream notify_stream = { .fd = -1 };
static struct stream emu_stream = { .fd = -1 };

static const char introspection_xml[] =
	"<node>"
	"<interface name='org.nuubOS.Lighting1'>"
	"<method name='GetState'>"
	"<arg name='supported' type='b' direction='out'/>"
	"<arg name='effect' type='s' direction='out'/>"
	"<arg name='color' type='s' direction='out'/>"
	"<arg name='brightness' type='i' direction='out'/>"
	"<arg name='speed' type='s' direction='out'/>"
	"<arg name='signals' type='u' direction='out'/>"
	"<arg name='active_signal' type='s' direction='out'/>"
	"</method>"
	"<method name='SetEffect'><arg name='effect' type='s' direction='in'/></method>"
	"<method name='SetColor'><arg name='color' type='s' direction='in'/></method>"
	"<method name='SetBrightness'><arg name='brightness' type='i' direction='in'/></method>"
	"<method name='SetSpeed'><arg name='speed' type='s' direction='in'/></method>"
	"<method name='SetSignal'>"
	"<arg name='category' type='s' direction='in'/>"
	"<arg name='enabled' type='b' direction='in'/>"
	"</method>"
	"<method name='PreviewSignal'><arg name='category' type='s' direction='in'/></method>"
	"<method name='Preview'>"
	"<arg name='effect' type='s' direction='in'/>"
	"<arg name='color' type='s' direction='in'/>"
	"</method>"
	"<method name='EndPreview'/>"
	"<method name='Lifecycle'><arg name='phase' type='s' direction='in'/></method>"
	"<signal name='StateChanged'>"
	"<arg name='supported' type='b'/>"
	"<arg name='effect' type='s'/>"
	"<arg name='color' type='s'/>"
	"<arg name='brightness' type='i'/>"
	"<arg name='speed' type='s'/>"
	"<arg name='signals' type='u'/>"
	"<arg name='active_signal' type='s'/>"
	"</signal>"
	"</interface>"
	"</node>";

static void signal_handler(int sig_no)
{
	(void)sig_no;
	running = 0;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void trim(char *text)
{
	size_t len = strlen(text);

	while (len > 0 &&
	       (text[len - 1] == '\n' || text[len - 1] == '\r' ||
		text[len - 1] == ' ' || text[len - 1] == '\t'))
		text[--len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Colour helpers                                                      */
/* ------------------------------------------------------------------ */

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static float frac(float v)
{
	return v - floorf(v);
}

static struct rgb scale(struct rgb c, float k)
{
	return (struct rgb){ c.r * k, c.g * k, c.b * k };
}

static struct rgb mix(struct rgb a, struct rgb b, float t)
{
	return (struct rgb){
		a.r + (b.r - a.r) * t,
		a.g + (b.g - a.g) * t,
		a.b + (b.b - a.b) * t,
	};
}

static struct rgb hsv(float h, float s, float v)
{
	float r = clamp01(fabsf(frac(h + 1.0f) * 6.0f - 3.0f) - 1.0f);
	float g = clamp01(2.0f - fabsf(frac(h + 2.0f / 3.0f) * 6.0f - 3.0f));
	float b = clamp01(2.0f - fabsf(frac(h + 1.0f / 3.0f) * 6.0f - 3.0f));

	return (struct rgb){
		v * (1.0f - s + s * r),
		v * (1.0f - s + s * g),
		v * (1.0f - s + s * b),
	};
}

/* Same hue at full intensity: a dark UI colour still lights the LEDs. */
static struct rgb vivid(struct rgb c)
{
	float m = fmaxf(c.r, fmaxf(c.g, c.b));

	return m < 0.01f ? ROLE_NEUTRAL : scale(c, 1.0f / m);
}

static struct rgb hue_shift(struct rgb c, float turns)
{
	float mx = fmaxf(c.r, fmaxf(c.g, c.b));
	float mn = fminf(c.r, fminf(c.g, c.b));
	float d = mx - mn;
	float h;

	if (d < 0.001f)
		return c;
	if (mx == c.r)
		h = (c.g - c.b) / d / 6.0f;
	else if (mx == c.g)
		h = ((c.b - c.r) / d + 2.0f) / 6.0f;
	else
		h = ((c.r - c.g) / d + 4.0f) / 6.0f;
	return hsv(frac(h + turns), d / mx, mx);
}

static bool parse_hex(const char *text, struct rgb *out)
{
	unsigned int r, g, b;

	if (*text == '#')
		text++;
	if (strlen(text) != 6 || strspn(text, "0123456789abcdefABCDEF") != 6 ||
	    sscanf(text, "%2x%2x%2x", &r, &g, &b) != 3)
		return false;
	*out = (struct rgb){ r / 255.0f, g / 255.0f, b / 255.0f };
	return true;
}

/* Circular distance between two ring positions in turns (0..0.5). */
static float ring_distance(float a, float b)
{
	float d = fabsf(frac(a - b));

	return d > 0.5f ? 1.0f - d : d;
}

static float smooth(float t)
{
	t = clamp01(t);
	return t * t * (3.0f - 2.0f * t);
}

/* ------------------------------------------------------------------ */
/* Preferences                                                         */
/* ------------------------------------------------------------------ */

static int index_of(const char *const *names, int count, const char *value)
{
	for (int i = 0; i < count; i++)
		if (strcmp(names[i], value) == 0)
			return i;
	return -1;
}

static bool valid_effect(const char *value)
{
	return index_of(effect_names, EFFECT_COUNT, value) >= 0;
}

static bool valid_color(const char *value)
{
	struct rgb unused;

	return strcmp(value, "theme") == 0 ||
	       (value[0] != '#' && parse_hex(value, &unused));
}

static bool valid_speed(const char *value)
{
	return strcmp(value, "slow") == 0 || strcmp(value, "normal") == 0 ||
	       strcmp(value, "fast") == 0;
}

static int clamp_brightness(int value)
{
	value = (value + 5) / 10 * 10;
	return value < 10 ? 10 : value > 100 ? 100 : value;
}

static void default_prefs(struct prefs *p)
{
	/* Quiet by default: the LEDs speak only for system signals. */
	snprintf(p->effect, sizeof(p->effect), "off");
	snprintf(p->color, sizeof(p->color), "theme");
	p->brightness = DEFAULT_BRIGHTNESS;
	snprintf(p->speed, sizeof(p->speed), "normal");
	p->signals = SIG_ALL;
}

static unsigned int parse_signals(const char *list)
{
	char copy[128];
	char *save = NULL;
	unsigned int mask = 0;

	snprintf(copy, sizeof(copy), "%s", list);
	for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		int i = index_of(category_names, CATEGORY_COUNT, tok);

		if (i >= 0)
			mask |= 1u << i;
	}
	return mask;
}

/* nuubOS 0.5 lighting.conf (MODE/RED/GREEN/BLUE/SYSTEM_EFFECTS). */
static void migrate_v1(const char *mode, int r, int g, int b, int effects)
{
	static const char *const map[][2] = {
		{ "off", "off" }, { "static", "solid" }, { "breathe", "breathe" },
		{ "pulse", "breathe" }, { "chase", "orbit" }, { "wave", "aurora" },
		{ "sparkle", "aurora" }, { "rainbow", "spectrum" }, { "screen", "solid" },
	};

	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (strcmp(mode, map[i][0]) == 0)
			snprintf(prefs.effect, sizeof(prefs.effect), "%s", map[i][1]);
	if (r >= 0 && g >= 0 && b >= 0 && r <= 255 && g <= 255 && b <= 255 &&
	    !(r == 58 && g == 134 && b == 255))
		snprintf(prefs.color, sizeof(prefs.color), "%02x%02x%02x",
			 (unsigned int)r & 0xff, (unsigned int)g & 0xff, (unsigned int)b & 0xff);
	if (effects == 0)
		prefs.signals = 0;
}

static void settings_path(char *out, size_t size)
{
	snprintf(out, size, "/state/users/%s/lighting.conf", active_user);
}

static void load_prefs(void)
{
	char path[384];
	char line[256];
	char mode[24] = "";
	int r = -1, g = -1, b = -1, effects = -1;
	bool v2 = false;
	FILE *fp;

	default_prefs(&prefs);
	settings_path(path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char *eq = strchr(line, '=');
		const char *key = line;
		char *value;
		int n;

		if (!eq)
			continue;
		*eq = '\0';
		value = eq + 1;
		trim(value);

		if (strcmp(key, "FORMAT") == 0)
			v2 = atoi(value) >= 2;
		else if (strcmp(key, "EFFECT") == 0 && valid_effect(value))
			snprintf(prefs.effect, sizeof(prefs.effect), "%s", value);
		else if (strcmp(key, "COLOR") == 0 && valid_color(value))
			snprintf(prefs.color, sizeof(prefs.color), "%s", value);
		else if (strcmp(key, "SPEED") == 0 && valid_speed(value))
			snprintf(prefs.speed, sizeof(prefs.speed), "%s", value);
		else if (strcmp(key, "SIGNALS") == 0)
			prefs.signals = parse_signals(value);
		else if (strcmp(key, "BRIGHTNESS") == 0 && sscanf(value, "%d", &n) == 1)
			prefs.brightness = clamp_brightness(n);
		else if (strcmp(key, "MODE") == 0)
			snprintf(mode, sizeof(mode), "%s", value);
		else if (strcmp(key, "RED") == 0)
			r = atoi(value);
		else if (strcmp(key, "GREEN") == 0)
			g = atoi(value);
		else if (strcmp(key, "BLUE") == 0)
			b = atoi(value);
		else if (strcmp(key, "SYSTEM_EFFECTS") == 0)
			effects = atoi(value);
	}
	fclose(fp);

	if (!v2 && mode[0])
		migrate_v1(mode, r, g, b, effects);
}

static int save_prefs(void)
{
	char dir[256];
	char path[384];
	char tmp[420];
	char list[64] = "";
	FILE *fp;

	for (int i = 0; i < CATEGORY_COUNT; i++) {
		if (!(prefs.signals & (1u << i)))
			continue;
		if (list[0])
			strcat(list, ",");
		strcat(list, category_names[i]);
	}

	if ((mkdir("/state/users", 0755) != 0 && errno != EEXIST))
		return -1;
	snprintf(dir, sizeof(dir), "/state/users/%s", active_user);
	if (mkdir(dir, 0755) != 0 && errno != EEXIST)
		return -1;

	settings_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	if (fprintf(fp,
		    "FORMAT=2\nEFFECT=%s\nCOLOR=%s\nBRIGHTNESS=%d\nSPEED=%s\nSIGNALS=%s\n",
		    prefs.effect, prefs.color, prefs.brightness, prefs.speed, list) < 0 ||
	    fflush(fp) != 0 || fsync(fileno(fp)) != 0 || fclose(fp) != 0) {
		unlink(tmp);
		return -1;
	}
	if (chmod(tmp, 0644) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void refresh_active_user(void)
{
	FILE *fp = fopen(ACTIVE_USER, "r");
	char value[sizeof(active_user)] = "default";

	if (fp) {
		if (!fgets(value, sizeof(value), fp))
			snprintf(value, sizeof(value), "default");
		fclose(fp);
		trim(value);
		if (!value[0] || strchr(value, '/') || value[0] == '.')
			snprintf(value, sizeof(value), "default");
	}
	snprintf(active_user, sizeof(active_user), "%s", value);
}

/* The active user's theme accent (themes.md): THEME= in settings.conf. */
static void load_theme_accent(void)
{
	char path[512];
	char line[256];
	char theme[64] = "";
	FILE *fp;

	theme_accent = (struct rgb){ 58 / 255.0f, 134 / 255.0f, 1.0f };
	snprintf(path, sizeof(path), "/state/users/%s/settings.conf", active_user);
	fp = fopen(path, "r");
	if (fp) {
		while (fgets(line, sizeof(line), fp))
			if (strncmp(line, "THEME=", 6) == 0) {
				trim(line);
				snprintf(theme, sizeof(theme), "%.63s", line + 6);
			}
		fclose(fp);
	}
	if (!theme[0] || strspn(theme, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(theme))
		return;

	snprintf(path, sizeof(path), USER_THEMES "/%s/theme.conf", theme);
	fp = fopen(path, "r");
	if (!fp) {
		snprintf(path, sizeof(path), BUILTIN_THEMES "/%s/theme.conf", theme);
		fp = fopen(path, "r");
	}
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		struct rgb c;

		if (strncmp(line, "accent=", 7) != 0)
			continue;
		trim(line);
		if (parse_hex(line + 7, &c))
			theme_accent = c;
	}
	fclose(fp);
}

/* ------------------------------------------------------------------ */
/* Hardware (nuubos-rgbd)                                              */
/* ------------------------------------------------------------------ */

static int connect_unix(const char *path, bool nonblock)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0), 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void rgb_disconnect(void)
{
	if (rgb_fd >= 0)
		close(rgb_fd);
	rgb_fd = -1;
	sent_valid = false;
}

/* One command, one reply line. */
static int rgb_command(const char *command, char *reply, size_t size)
{
	size_t len = strlen(command);
	size_t used = 0;

	if (rgb_fd < 0) {
		struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };

		rgb_fd = connect_unix(RGB_SOCKET, false);
		if (rgb_fd < 0)
			return -1;
		setsockopt(rgb_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(rgb_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	if (send(rgb_fd, command, len, MSG_NOSIGNAL) != (ssize_t)len) {
		rgb_disconnect();
		return -1;
	}
	while (used + 1 < size) {
		ssize_t n = recv(rgb_fd, reply + used, 1, 0);

		if (n <= 0) {
			rgb_disconnect();
			return -1;
		}
		if (reply[used++] == '\n')
			break;
	}
	reply[used] = '\0';
	return strncmp(reply, "OK", 2) == 0 || strncmp(reply, "supported=", 10) == 0 ? 0 : -1;
}

static void detect_rgb(void)
{
	char reply[1024];
	char *p;
	int total = 0;

	supported = false;
	led_count = 0;
	zone_count = 0;
	if (rgb_command("STATUS\n", reply, sizeof(reply)) != 0 ||
	    !strstr(reply, "supported=1"))
		return;
	p = strstr(reply, "leds=");
	led_count = p ? atoi(p + 5) : 0;
	if (led_count < 1 || led_count > MAX_LEDS)
		return;
	for (int z = 0; z < MAX_ZONES; z++) {
		char key[16];
		char *c, *end, *last = NULL;
		int count;

		/* zoneN=name:type:count — the count follows the token's last colon. */
		snprintf(key, sizeof(key), " zone%d=", z);
		p = strstr(reply, key);
		if (!p)
			break;
		end = strchr(p + 1, ' ');
		for (c = p + strlen(key); *c && *c != '\n' && (!end || c < end); c++)
			if (*c == ':')
				last = c;
		if (!last)
			break;
		count = atoi(last + 1);
		if (count <= 0 || total + count > led_count)
			break;
		zone_size[zone_count++] = count;
		total += count;
	}
	if (zone_count == 0 || total != led_count) {
		zone_count = 1;
		zone_size[0] = led_count;
	}
	supported = true;
}

static void hardware_off_now(void)
{
	char reply[256];

	if (!supported)
		return;
	if (rgb_command("OFF\n", reply, sizeof(reply)) == 0)
		hardware_off = true;
	sent_valid = false;
}

/* ------------------------------------------------------------------ */
/* Effects                                                             */
/* ------------------------------------------------------------------ */

static const char *current_effect(void)
{
	return preview_active ? preview_effect : prefs.effect;
}

static struct rgb current_color(void)
{
	const char *color = preview_active ? preview_color : prefs.color;
	struct rgb c;

	if (strcmp(color, "theme") != 0 && parse_hex(color, &c))
		return vivid(c);
	return vivid(theme_accent);
}

static float speed_factor(void)
{
	return strcmp(prefs.speed, "slow") == 0 ? 1.7f
		: strcmp(prefs.speed, "fast") == 0 ? 0.55f : 1.0f;
}

static struct rgb battery_role(void)
{
	return battery_percent >= 50 ? ROLE_SUCCESS
		: battery_percent >= 20 ? ROLE_WARNING : ROLE_ERROR;
}

static bool effect_animated(void)
{
	const char *e = current_effect();

	return strcmp(e, "breathe") == 0 || strcmp(e, "orbit") == 0 ||
	       strcmp(e, "aurora") == 0 || strcmp(e, "spectrum") == 0 ||
	       (strcmp(e, "battery") == 0 && battery_charging);
}

/* u = position around the ring in turns, t = seconds since the effect began. */
static struct rgb effect_pixel(float u, int i, int n, float t)
{
	const char *e = current_effect();
	struct rgb c = current_color();
	float sf = speed_factor();

	if (strcmp(e, "solid") == 0)
		return c;

	if (strcmp(e, "breathe") == 0) {
		float x = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * t / (5.0f * sf));

		return scale(c, 0.06f + 0.94f * x);
	}

	if (strcmp(e, "orbit") == 0) {
		float head = frac(t / (2.6f * sf));
		float d = frac(head - u);
		float tail = 0.55f;
		float level = d < tail ? powf(1.0f - d / tail, 2.0f) : 0.0f;

		return scale(c, 0.05f + 0.95f * level);
	}

	if (strcmp(e, "aurora") == 0) {
		float p = 10.0f * sf;
		float w = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * (u - t / p));
		float k = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * (2.0f * u + t / (p * 0.7f)));

		return scale(mix(c, hue_shift(c, 0.14f), w), 0.45f + 0.55f * k);
	}

	if (strcmp(e, "spectrum") == 0)
		return hsv(frac(u + t / (14.0f * sf)), 0.92f, 1.0f);

	if (strcmp(e, "battery") == 0) {
		int lit;
		struct rgb role;

		if (battery_percent < 0)
			return scale(c, 0.25f);
		role = battery_role();
		lit = (battery_percent * n + 50) / 100;
		if (lit < 1)
			lit = 1;
		if (battery_charging && i == (lit < n ? lit : n - 1)) {
			float x = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * t / 1.8f);

			return scale(role, 0.05f + 0.95f * x);
		}
		return i < lit ? role : scale(role, 0.05f);
	}

	if (strcmp(e, "game") == 0)
		return game_running && game_color_known ? game_color : c;

	return BLACK;
}

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

/*
 * Signal pixel over the base pixel. Shapes that own the whole ring return
 * alpha 1 with their own darkness; the envelope fades back to the effect.
 */
static struct rgb signal_pixel(struct rgb base, float u, int i, int n, float s)
{
	const struct signal_play *g = &sig;
	float d = g->duration_ms / 1000.0f;
	struct rgb c = g->color;
	struct rgb out = BLACK;
	float alpha = 1.0f;
	float tail = 0.2f; /* final fade back to the effect, seconds */

	switch (g->shape) {
	case SHAPE_WELCOME: {
		float lap = 1.1f;

		if (s < lap) {
			float head = s / lap;
			float dist = frac(head - u);
			float len = 0.12f + 0.88f * smooth(s / lap);

			out = dist < len ? scale(c, powf(1.0f - dist / len, 1.5f)) : BLACK;
		} else {
			out = c;
		}
		tail = 0.6f;
		break;
	}
	case SHAPE_WAKE: {
		float r = 0.5f * smooth(s / 0.45f);

		out = ring_distance(u, 0.0f) <= r + 0.5f / n ? c : BLACK;
		tail = 0.5f;
		break;
	}
	case SHAPE_SLEEP:
		/* Fade whatever is shown to dark; never back. */
		return scale(base, 1.0f - smooth(s / d));
	case SHAPE_FAREWELL: {
		float r = 0.5f * (1.0f - smooth(s / (d * 0.85f)));

		out = ring_distance(u, 0.0f) <= r ? c : BLACK;
		return out;
	}
	case SHAPE_LINK: {
		float r = 0.5f * smooth(s / 0.5f);

		if (g->count > 0 && s >= 0.8f)
			out = i < g->count ? c : BLACK;
		else
			out = ring_distance(u, 0.0f) <= r + 0.5f / n ? c : BLACK;
		tail = 0.4f;
		break;
	}
	case SHAPE_UNLINK: {
		float r = 0.5f * (1.0f - smooth(s / 0.7f));

		out = ring_distance(u, 0.0f) <= r + 0.5f / n ? c : BLACK;
		tail = 0.25f;
		break;
	}
	case SHAPE_PULSE: {
		float p = frac(s / 0.5f);
		float k = s < g->count * 0.5f ? sinf((float)M_PI * fminf(1.0f, p / 0.8f)) : 0.0f;

		out = scale(c, k);
		tail = 0.15f;
		break;
	}
	case SHAPE_FILL: {
		float head = g->level * smooth(s / 0.6f);
		float pos = (i + 0.5f) / n;

		out = pos <= head ? c : scale(c, 0.06f);
		tail = 0.5f;
		break;
	}
	case SHAPE_GLOW:
		out = c;
		alpha = sinf((float)M_PI * clamp01(s / d));
		return mix(base, out, alpha);
	case SHAPE_SPIN: {
		float head = frac(g->dir > 0 ? s / 0.6f : -s / 0.6f);
		float dist = frac(g->dir > 0 ? head - u : u - head);
		float level = dist < 0.35f ? powf(1.0f - dist / 0.35f, 1.5f) : 0.0f;

		out = scale(mix(ROLE_NEUTRAL, c, clamp01(s / 0.6f)), level);
		tail = 0.25f;
		break;
	}
	case SHAPE_FLASH:
		out = ROLE_NEUTRAL;
		alpha = powf(1.0f - clamp01(s / d), 2.0f);
		return mix(base, out, alpha);
	case SHAPE_TICK:
		out = i == g->count % n ? c : scale(base, 0.25f);
		tail = 0.3f;
		break;
	case SHAPE_NONE:
		return base;
	}

	if (s > d - tail)
		alpha = clamp01((d - s) / tail);
	return mix(base, out, alpha);
}

static bool signal_active(void)
{
	return sig.shape != SHAPE_NONE;
}

static void emit_state(void);

static void start_signal(enum shape shape, const char *name, struct rgb color,
			 int prio, int duration_ms, int count, int dir, float level)
{
	if (!supported || phase != PHASE_AWAKE)
		return;
	if (signal_active() && prio < sig.prio)
		return;
	sig = (struct signal_play){
		.shape = shape,
		.color = color,
		.prio = prio,
		.start_ms = now_ms(),
		.duration_ms = duration_ms,
		.count = count,
		.dir = dir,
		.level = level,
	};
	snprintf(sig.name, sizeof(sig.name), "%s", name);
	emit_state();
}

static struct rgb accent(void)
{
	return vivid(theme_accent);
}

static void play_category(unsigned int category)
{
	switch (category) {
	case SIG_POWER:
		start_signal(SHAPE_WELCOME, "welcome", accent(), 9, 2300, 0, 0, 0);
		break;
	case SIG_BATTERY:
		if (battery_percent >= 0)
			start_signal(SHAPE_FILL, "battery.charging", ROLE_SUCCESS, 1, 1900, 0, 0,
				     battery_percent / 100.0f);
		else
			start_signal(SHAPE_GLOW, "battery.full", ROLE_SUCCESS, 1, 1600, 0, 0, 0);
		break;
	case SIG_CONNECTIONS:
		start_signal(SHAPE_LINK, "controller.connected", accent(), 1, 2200, 1, 0, 0);
		break;
	case SIG_GAMES:
		start_signal(SHAPE_SPIN, "game.state.saved", accent(), 1, 850, 0, 1, 0);
		break;
	case SIG_TASKS:
		start_signal(SHAPE_FILL, "job.succeeded", ROLE_SUCCESS, 1, 1700, 0, 0, 1.0f);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Frames                                                              */
/* ------------------------------------------------------------------ */

static void schedule_crossfade(void)
{
	memcpy(fade_from, last_out, sizeof(fade_from));
	fade_start_ms = now_ms();
	effect_epoch_ms = fade_start_ms;
}

static bool crossfading(long long now)
{
	return fade_start_ms >= 0 && now - fade_start_ms < CROSSFADE_MS;
}

static bool needs_frames(void)
{
	return supported && phase != PHASE_SHUTDOWN &&
	       (signal_active() || crossfading(now_ms()) ||
		(phase == PHASE_AWAKE && effect_animated()));
}

static uint8_t to_byte(float v, float gain)
{
	/* LEDs respond linearly: gamma 2.2 makes steps and fades even. */
	float x = powf(clamp01(v), 2.2f) * gain;

	return (uint8_t)lroundf(clamp01(x) * 255.0f);
}

static void finish_sleep(void);

static void render(void)
{
	long long now = now_ms();
	float t = (now - effect_epoch_ms) / 1000.0f;
	float s = signal_active() ? (now - sig.start_ms) / 1000.0f : 0.0f;
	float gain = powf(prefs.brightness / 100.0f, 2.2f);
	bool effect_off = strcmp(current_effect(), "off") == 0;
	bool lit = false;
	char command[MAX_LEDS * 12 + 16];
	char reply[256];
	uint8_t bytes[MAX_LEDS * 3];
	size_t used;
	int j = 0;

	if (!supported || phase == PHASE_SHUTDOWN)
		return;

	if (signal_active() && s * 1000.0f >= sig.duration_ms) {
		enum shape ended = sig.shape;

		sig.shape = SHAPE_NONE;
		sig.name[0] = '\0';
		s = 0.0f;
		emit_state();
		if (ended == SHAPE_SLEEP) {
			finish_sleep();
			return;
		}
		if (ended == SHAPE_FAREWELL) {
			phase = PHASE_SHUTDOWN;
			hardware_off_now();
			return;
		}
	}

	for (int z = 0; z < zone_count; z++) {
		int n = zone_size[z];

		for (int i = 0; i < n; i++, j++) {
			float u = (float)i / n;
			struct rgb px = phase == PHASE_AWAKE || signal_active()
				? effect_pixel(u, i, n, t) : BLACK;

			if (phase != PHASE_AWAKE && !signal_active())
				px = BLACK;
			if (crossfading(now))
				px = mix(fade_from[j], px, smooth((now - fade_start_ms) / (float)CROSSFADE_MS));
			if (signal_active())
				px = signal_pixel(px, u, i, n, s);
			last_out[j] = px;
			bytes[j * 3 + 0] = to_byte(px.r, gain);
			bytes[j * 3 + 1] = to_byte(px.g, gain);
			bytes[j * 3 + 2] = to_byte(px.b, gain);
			if (bytes[j * 3] || bytes[j * 3 + 1] || bytes[j * 3 + 2])
				lit = true;
		}
	}
	if (fade_start_ms >= 0 && !crossfading(now))
		fade_start_ms = -1;

	/* Nothing to show: power the MCU and LED rail down. */
	if (!lit && !signal_active() && !crossfading(now) &&
	    (effect_off || phase != PHASE_AWAKE)) {
		if (!hardware_off)
			hardware_off_now();
		return;
	}

	if (sent_valid && memcmp(sent, bytes, (size_t)led_count * 3) == 0)
		return;

	used = (size_t)snprintf(command, sizeof(command), "FRAME");
	for (int k = 0; k < led_count * 3; k++)
		used += (size_t)snprintf(command + used, sizeof(command) - used, " %u", bytes[k]);
	snprintf(command + used, sizeof(command) - used, "\n");
	if (rgb_command(command, reply, sizeof(reply)) == 0) {
		memcpy(sent, bytes, (size_t)led_count * 3);
		sent_valid = true;
		hardware_off = false;
	}
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void send_reply(DBusMessage *reply)
{
	if (!reply)
		return;
	dbus_connection_send(bus, reply, NULL);
	dbus_connection_flush(bus);
	dbus_message_unref(reply);
}

static void reply_pending_sleep(void)
{
	if (!pending_sleep)
		return;
	send_reply(dbus_message_new_method_return(pending_sleep));
	dbus_message_unref(pending_sleep);
	pending_sleep = NULL;
}

static void finish_sleep(void)
{
	hardware_off_now();
	reply_pending_sleep();
}

static void lifecycle(const char *what, DBusMessage *message)
{
	if (strcmp(what, "sleep") == 0) {
		reply_pending_sleep();
		sig.shape = SHAPE_NONE;
		phase = PHASE_SLEEP;
		if (!supported || hardware_off) {
			send_reply(dbus_message_new_method_return(message));
			hardware_off_now();
			return;
		}
		pending_sleep = dbus_message_ref(message);
		if (prefs.signals & SIG_POWER) {
			/* SLEEP fades whatever is lit; start_signal needs AWAKE. */
			sig = (struct signal_play){ .shape = SHAPE_SLEEP, .prio = 10,
						    .start_ms = now_ms(), .duration_ms = 450 };
			snprintf(sig.name, sizeof(sig.name), "sleep");
		} else {
			finish_sleep();
		}
		return;
	}

	send_reply(dbus_message_new_method_return(message));

	if (strcmp(what, "wake") == 0) {
		/* Also sent when nuubos-systemd starts and after a failed power
		 * off: only a dark (or darkening) ring comes back. */
		if (phase == PHASE_AWAKE && sig.shape != SHAPE_FAREWELL &&
		    sig.shape != SHAPE_SLEEP)
			return;
		reply_pending_sleep();
		phase = PHASE_AWAKE;
		sig.shape = SHAPE_NONE;
		memset(last_out, 0, sizeof(last_out));
		schedule_crossfade();
		if (prefs.signals & SIG_POWER)
			start_signal(SHAPE_WAKE, "wake", accent(), 9, 1200, 0, 0, 0);
	} else if (strcmp(what, "shutdown") == 0) {
		reply_pending_sleep();
		if (phase == PHASE_AWAKE && (prefs.signals & SIG_POWER) && !hardware_off) {
			sig.shape = SHAPE_NONE;
			start_signal(SHAPE_FAREWELL, "farewell", accent(), 10, 900, 0, 0, 0);
		} else {
			phase = PHASE_SHUTDOWN;
			hardware_off_now();
		}
	}
}

/* ------------------------------------------------------------------ */
/* Event sources                                                       */
/* ------------------------------------------------------------------ */

static void read_battery(void)
{
	FILE *fp = fopen(STATUS_STATE, "r");
	char line[128];
	int percent = -1;
	bool charging = false;

	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (strncmp(line, "BATTERY_PERCENT=", 16) == 0)
			percent = atoi(line + 16);
		else if (strncmp(line, "BATTERY_STATE=", 14) == 0)
			charging = strcmp(line + 14, "charging") == 0;
	}
	fclose(fp);
	if (percent > 100)
		percent = 100;
	if (percent != battery_percent || charging != battery_charging) {
		bool gauge = strcmp(current_effect(), "battery") == 0;

		battery_percent = percent;
		battery_charging = charging;
		if (gauge)
			schedule_crossfade();
	}
}

/* Value of key=<percent-encoded> in a notification line. */
static bool field(const char *line, const char *key, char *out, size_t size)
{
	size_t klen = strlen(key);
	const char *p = line;

	while ((p = strstr(p, key)) != NULL) {
		if ((p == line || p[-1] == ' ') && p[klen] == '=') {
			size_t o = 0;

			for (p += klen + 1; *p && *p != ' ' && o + 1 < size; p++) {
				unsigned int v;

				if (*p == '%' && sscanf(p + 1, "%2x", &v) == 1) {
					out[o++] = (char)v;
					p += 2;
				} else {
					out[o++] = *p;
				}
			}
			out[o] = '\0';
			return true;
		}
		p += klen;
	}
	out[0] = '\0';
	return false;
}

static int field_int(const char *line, const char *key, int fallback)
{
	char value[16];

	return field(line, key, value, sizeof(value)) && value[0] ? atoi(value) : fallback;
}

static bool on(unsigned int category)
{
	return (prefs.signals & category) != 0;
}

/*
 * EPIC-006 event → signal. Only POST starts a signal (UPDATE refreshes a
 * notification already announced) and Live Notifications only when they
 * complete.
 */
static void notification(const char *line)
{
	char event[48];
	char state[24];
	bool done;

	if (strncmp(line, "POST ", 5) != 0 || !field(line, "event", event, sizeof(event)))
		return;
	done = !field(line, "progress", state, sizeof(state));
	field(line, "state", state, sizeof(state));

	/* Battery. */
	if (strcmp(event, "battery.critical") == 0 && on(SIG_BATTERY))
		start_signal(SHAPE_PULSE, event, ROLE_ERROR, 3, 1700, 3, 0, 0);
	else if ((strcmp(event, "battery.low") == 0 ||
		  strcmp(event, "accessory.battery.low") == 0) && on(SIG_BATTERY))
		start_signal(SHAPE_PULSE, event, ROLE_WARNING, 2, 1200, 2, 0, 0);
	else if (strcmp(event, "battery.saver") == 0 && on(SIG_BATTERY))
		start_signal(SHAPE_PULSE, event, ROLE_WARNING, 2, 700, 1, 0, 0);
	else if (strcmp(event, "battery.charging") == 0 && on(SIG_BATTERY))
		start_signal(SHAPE_FILL, event, ROLE_SUCCESS, 1, 1900, 0, 0,
			     field_int(line, "percent", battery_percent < 0 ? 50 : battery_percent) / 100.0f);
	else if (strcmp(event, "battery.full") == 0 && on(SIG_BATTERY))
		start_signal(SHAPE_GLOW, event, ROLE_SUCCESS, 1, 1600, 0, 0, 0);

	/* Connections. */
	else if (strcmp(event, "controller.connected") == 0 && on(SIG_CONNECTIONS))
		start_signal(SHAPE_LINK, event, accent(), 1, 2200,
			     field_int(line, "player", 0), 0, 0);
	else if ((strcmp(event, "headphones.connected") == 0 ||
		  strcmp(event, "wifi.connected") == 0 ||
		  strcmp(event, "media.connected") == 0 ||
		  strcmp(event, "stream.paired") == 0) && on(SIG_CONNECTIONS))
		start_signal(SHAPE_LINK, event, accent(), 1, 1300, 0, 0, 0);
	else if ((strcmp(event, "controller.disconnected") == 0 ||
		  strcmp(event, "headphones.disconnected") == 0 ||
		  strcmp(event, "media.removed") == 0) && on(SIG_CONNECTIONS))
		start_signal(SHAPE_UNLINK, event, scale(ROLE_NEUTRAL, 0.7f), 1, 950, 0, 0, 0);
	else if (strcmp(event, "wifi.lost") == 0 && on(SIG_CONNECTIONS))
		start_signal(SHAPE_UNLINK, event, ROLE_WARNING, 2, 950, 0, 0, 0);
	else if (strcmp(event, "stream.pair.failed") == 0 && on(SIG_CONNECTIONS))
		start_signal(SHAPE_PULSE, event, ROLE_ERROR, 3, 1200, 2, 0, 0);

	/* Games. */
	else if (strcmp(event, "game.state.saved") == 0 && on(SIG_GAMES))
		start_signal(SHAPE_SPIN, event, accent(), 1, 850, 0, 1, 0);
	else if (strcmp(event, "game.state.loaded") == 0 && on(SIG_GAMES))
		start_signal(SHAPE_SPIN, event, accent(), 1, 850, 0, -1, 0);
	else if (strcmp(event, "game.screenshot.saved") == 0 && on(SIG_GAMES))
		start_signal(SHAPE_FLASH, event, ROLE_NEUTRAL, 1, 550, 0, 0, 0);
	else if (strcmp(event, "game.slot") == 0 && on(SIG_GAMES))
		start_signal(SHAPE_TICK, event, accent(), 1, 900, field_int(line, "slot", 0), 0, 0);
	else if (strcmp(event, "game.state.empty") == 0 && on(SIG_GAMES))
		start_signal(SHAPE_PULSE, event, scale(ROLE_NEUTRAL, 0.6f), 1, 700, 1, 0, 0);
	else if ((strcmp(event, "game.state.failed") == 0 ||
		  strcmp(event, "game.screenshot.failed") == 0 ||
		  strcmp(event, "game.failed") == 0 ||
		  strcmp(event, "stream.failed") == 0) && on(SIG_GAMES))
		start_signal(SHAPE_PULSE, event, ROLE_ERROR, 3, 1200, 2, 0, 0);

	/* Tasks. */
	else if ((strcmp(event, "job") == 0 || strcmp(event, "storage.job") == 0) &&
		 done && on(SIG_TASKS)) {
		if (strcmp(state, "succeeded") == 0)
			start_signal(SHAPE_FILL, "job.succeeded", ROLE_SUCCESS, 1, 1700, 0, 0, 1.0f);
		else if (strcmp(state, "failed") == 0)
			start_signal(SHAPE_PULSE, "job.failed", ROLE_ERROR, 3, 1200, 2, 0, 0);
	} else if (strcmp(event, "stream.steamlink.installed") == 0 && on(SIG_TASKS))
		start_signal(SHAPE_FILL, event, ROLE_SUCCESS, 1, 1700, 0, 0, 1.0f);
	else if ((strcmp(event, "stream.steamlink.failed") == 0 ||
		  strcmp(event, "media.failed") == 0 ||
		  strcmp(event, "web.failed") == 0) && on(SIG_TASKS))
		start_signal(SHAPE_PULSE, event, ROLE_ERROR, 3, 1200, 2, 0, 0);
}

static void lookup_game_color(const char *system)
{
	FILE *fp = fopen(SYSTEMS_CONF, "r");
	char line[512];
	size_t len = strlen(system);

	game_color_known = false;
	if (!fp || !len) {
		if (fp)
			fclose(fp);
		return;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *f = line;

		if (strncmp(line, system, len) != 0 || line[len] != '|')
			continue;
		for (int k = 0; k < 5 && f; k++) {
			f = strchr(f, '|');
			if (f)
				f++;
		}
		if (f) {
			char *end = strchr(f, '|');
			struct rgb c;

			if (end)
				*end = '\0';
			trim(f);
			if (parse_hex(f, &c)) {
				game_color = vivid(c);
				game_color_known = true;
			}
		}
		break;
	}
	fclose(fp);
}

/* emud SUBSCRIBE snapshots: key=value lines ending with end=1. */
static void emu_line(const char *line)
{
	static bool snap_running;
	static char snap_system[64];

	if (strncmp(line, "state=", 6) == 0) {
		snap_running = strcmp(line + 6, "running") == 0;
	} else if (strncmp(line, "system=", 7) == 0) {
		snprintf(snap_system, sizeof(snap_system), "%s", line + 7);
	} else if (strcmp(line, "end=1") == 0) {
		static char shown[64];
		bool changed = snap_running != game_running ||
			(snap_running && strcmp(snap_system, shown) != 0);

		game_running = snap_running;
		snprintf(shown, sizeof(shown), "%s", snap_running ? snap_system : "");
		if (changed) {
			lookup_game_color(shown);
			if (strcmp(current_effect(), "game") == 0)
				schedule_crossfade();
		}
	}
}

static void status_line(const char *line)
{
	if (strcmp(line, "changed") == 0)
		read_battery();
}

static void stream_close(struct stream *st)
{
	if (st->fd >= 0)
		close(st->fd);
	st->fd = -1;
	st->used = 0;
}

static void stream_open(struct stream *st, const char *path, const char *hello)
{
	if (st->fd >= 0)
		return;
	st->fd = connect_unix(path, true);
	st->used = 0;
	if (st->fd >= 0 && hello &&
	    send(st->fd, hello, strlen(hello), MSG_NOSIGNAL) != (ssize_t)strlen(hello))
		stream_close(st);
}

static void stream_read(struct stream *st, void (*on_line)(const char *))
{
	for (;;) {
		ssize_t n = recv(st->fd, st->buf + st->used, sizeof(st->buf) - 1 - st->used, 0);
		char *nl;

		if (n == 0) {
			stream_close(st);
			return;
		}
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				stream_close(st);
			return;
		}
		st->used += (size_t)n;
		st->buf[st->used] = '\0';
		while ((nl = memchr(st->buf, '\n', st->used)) != NULL) {
			size_t len = (size_t)(nl - st->buf);
			char line[sizeof(st->buf)];

			memcpy(line, st->buf, len);
			line[len] = '\0';
			memmove(st->buf, nl + 1, st->used - len - 1);
			st->used -= len + 1;
			if (len > 0 && line[len - 1] == '\r')
				line[len - 1] = '\0';
			on_line(line);
		}
		if (st->used >= sizeof(st->buf) - 1)
			st->used = 0; /* over-long line: drop it */
	}
}

static void connect_sources(void)
{
	bool had_rgb = supported;

	if (!supported)
		detect_rgb();
	if (supported && !had_rgb) {
		memset(last_out, 0, sizeof(last_out));
		schedule_crossfade();
		emit_state();
	}
	if (status_stream.fd < 0) {
		stream_open(&status_stream, STATUS_SOCKET, NULL);
		read_battery();
	}
	stream_open(&notify_stream, NOTIFY_SOCKET, "SUBSCRIBE\n");
	stream_open(&emu_stream, EMU_SOCKET, "SUBSCRIBE\n");
}

/* ------------------------------------------------------------------ */
/* D-Bus                                                               */
/* ------------------------------------------------------------------ */

static void append_state(DBusMessage *m)
{
	dbus_bool_t s = supported;
	const char *effect = prefs.effect;
	const char *color = prefs.color;
	dbus_int32_t brightness = prefs.brightness;
	const char *speed = prefs.speed;
	dbus_uint32_t signals = prefs.signals;
	const char *active = sig.shape != SHAPE_NONE ? sig.name : "";

	dbus_message_append_args(m,
				 DBUS_TYPE_BOOLEAN, &s,
				 DBUS_TYPE_STRING, &effect,
				 DBUS_TYPE_STRING, &color,
				 DBUS_TYPE_INT32, &brightness,
				 DBUS_TYPE_STRING, &speed,
				 DBUS_TYPE_UINT32, &signals,
				 DBUS_TYPE_STRING, &active,
				 DBUS_TYPE_INVALID);
}

static void emit_state(void)
{
	DBusMessage *m;

	if (!bus)
		return;
	m = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "StateChanged");
	if (!m)
		return;
	append_state(m);
	dbus_connection_send(bus, m, NULL);
	dbus_connection_flush(bus);
	dbus_message_unref(m);
}

static bool get_string(DBusMessage *m, const char **a)
{
	DBusError e;
	bool ok;

	dbus_error_init(&e);
	ok = dbus_message_get_args(m, &e, DBUS_TYPE_STRING, a, DBUS_TYPE_INVALID);
	dbus_error_free(&e);
	return ok;
}

static void invalid(DBusMessage *m, const char *text)
{
	send_reply(dbus_message_new_error(m, ERROR_INVALID, text));
}

/* A user-visible preference changed: save, show it, announce it. */
static void committed(DBusMessage *m, bool restyle)
{
	if (restyle)
		schedule_crossfade();
	(void)save_prefs();
	send_reply(dbus_message_new_method_return(m));
	emit_state();
}

static DBusHandlerResult handle_message(DBusConnection *conn, DBusMessage *m, void *data)
{
	const char *a = NULL;
	const char *b = NULL;

	(void)conn;
	(void)data;

	if (dbus_message_is_method_call(m, INTROSPECT_IFACE, "Introspect")) {
		DBusMessage *reply = dbus_message_new_method_return(m);
		const char *xml = introspection_xml;

		if (reply)
			dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
		send_reply(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "GetState")) {
		DBusMessage *reply = dbus_message_new_method_return(m);

		if (reply)
			append_state(reply);
		send_reply(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "SetEffect")) {
		if (!get_string(m, &a) || !valid_effect(a)) {
			invalid(m, "Unknown effect");
		} else {
			snprintf(prefs.effect, sizeof(prefs.effect), "%s", a);
			preview_active = false;
			committed(m, true);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "SetColor")) {
		if (!get_string(m, &a) || !valid_color(a)) {
			invalid(m, "Color must be theme or rrggbb");
		} else {
			snprintf(prefs.color, sizeof(prefs.color), "%s", a);
			preview_active = false;
			committed(m, true);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "SetSpeed")) {
		if (!get_string(m, &a) || !valid_speed(a)) {
			invalid(m, "Speed must be slow, normal or fast");
		} else {
			snprintf(prefs.speed, sizeof(prefs.speed), "%s", a);
			committed(m, false);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "SetBrightness")) {
		dbus_int32_t value;
		DBusError e;

		dbus_error_init(&e);
		if (!dbus_message_get_args(m, &e, DBUS_TYPE_INT32, &value, DBUS_TYPE_INVALID) ||
		    value < 10 || value > 100) {
			invalid(m, "Brightness must be 10..100");
		} else {
			prefs.brightness = clamp_brightness(value);
			committed(m, false);
			/* Dark effect: show the new level on the whole ring. */
			if (strcmp(prefs.effect, "off") == 0)
				start_signal(SHAPE_GLOW, "brightness", accent(), 0, 900, 0, 0, 0);
		}
		dbus_error_free(&e);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "SetSignal")) {
		dbus_bool_t enabled;
		DBusError e;
		int i = -1;

		dbus_error_init(&e);
		if (dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &a,
					  DBUS_TYPE_BOOLEAN, &enabled, DBUS_TYPE_INVALID))
			i = index_of(category_names, CATEGORY_COUNT, a);
		dbus_error_free(&e);
		if (i < 0) {
			invalid(m, "Unknown signal category");
		} else {
			if (enabled)
				prefs.signals |= 1u << i;
			else
				prefs.signals &= ~(1u << i);
			committed(m, false);
			if (enabled)
				play_category(1u << i);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "PreviewSignal")) {
		int i = get_string(m, &a) ? index_of(category_names, CATEGORY_COUNT, a) : -1;

		if (i < 0) {
			invalid(m, "Unknown signal category");
		} else {
			send_reply(dbus_message_new_method_return(m));
			sig.shape = SHAPE_NONE;
			play_category(1u << i);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "Preview")) {
		DBusError e;
		bool ok;

		dbus_error_init(&e);
		ok = dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &a, DBUS_TYPE_STRING, &b,
					   DBUS_TYPE_INVALID);
		dbus_error_free(&e);
		if (!ok || !valid_effect(a) || !valid_color(b)) {
			invalid(m, "Invalid preview");
		} else {
			snprintf(preview_effect, sizeof(preview_effect), "%s", a);
			snprintf(preview_color, sizeof(preview_color), "%s", b);
			preview_active = true;
			sig.shape = SHAPE_NONE;
			schedule_crossfade();
			send_reply(dbus_message_new_method_return(m));
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "EndPreview")) {
		if (preview_active) {
			preview_active = false;
			schedule_crossfade();
		}
		send_reply(dbus_message_new_method_return(m));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(m, INTERFACE_NAME, "Lifecycle")) {
		if (!get_string(m, &a) ||
		    (strcmp(a, "sleep") != 0 && strcmp(a, "wake") != 0 && strcmp(a, "shutdown") != 0))
			invalid(m, "Phase must be sleep, wake or shutdown");
		else
			lifecycle(a, m);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable object_vtable = {
	.unregister_function = NULL,
	.message_function = handle_message,
};

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static void sync_frame_timer(int fd, bool *armed)
{
	struct itimerspec spec;
	bool want = needs_frames();

	if (fd < 0 || want == *armed)
		return;
	memset(&spec, 0, sizeof(spec));
	if (want) {
		spec.it_value.tv_nsec = FRAME_MS * 1000000L;
		spec.it_interval.tv_nsec = FRAME_MS * 1000000L;
	}
	if (timerfd_settime(fd, 0, &spec, NULL) == 0)
		*armed = want;
}

static int watch_user_state(int inotify_fd, int old_watch)
{
	char dir[256];

	if (old_watch >= 0)
		inotify_rm_watch(inotify_fd, old_watch);
	snprintf(dir, sizeof(dir), "/state/users/%s", active_user);
	return inotify_add_watch(inotify_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO);
}

static void switch_user(void)
{
	refresh_active_user();
	load_prefs();
	load_theme_accent();
	preview_active = false;
	if (sig.shape != SHAPE_SLEEP && sig.shape != SHAPE_FAREWELL)
		sig.shape = SHAPE_NONE;
	schedule_crossfade();
	emit_state();
}

int main(void)
{
	struct sigaction sa;
	DBusError error;
	int dbus_fd = -1;
	int inotify_fd;
	int runtime_watch = -1;
	int user_watch = -1;
	int state_watch = -1;
	int timer_fd;
	bool timer_armed = false;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	refresh_active_user();
	load_prefs();
	load_theme_accent();
	effect_epoch_ms = now_ms();

	dbus_error_init(&error);
	bus = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (!bus)
		return 1;
	if (dbus_bus_request_name(bus, SERVICE_NAME, DBUS_NAME_FLAG_REPLACE_EXISTING, &error) !=
	    DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
	    !dbus_connection_register_object_path(bus, OBJECT_PATH, &object_vtable, NULL)) {
		dbus_connection_unref(bus);
		return 1;
	}
	dbus_error_free(&error);
	(void)dbus_connection_get_unix_fd(bus, &dbus_fd);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0) {
		runtime_watch = inotify_add_watch(inotify_fd, RUNTIME_DIR, IN_CREATE | IN_MOVED_TO);
		user_watch = inotify_add_watch(inotify_fd, USER_DIR,
			IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE);
		state_watch = watch_user_state(inotify_fd, -1);
	}
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	connect_sources();
	/* The welcome plays once per boot, not when the service restarts. */
	if (supported && access(BOOT_MARKER, F_OK) != 0) {
		int fd = open(BOOT_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

		if (fd >= 0)
			close(fd);
		if (prefs.signals & SIG_POWER)
			play_category(SIG_POWER);
	}
	render();

	while (running) {
		struct pollfd fds[6];
		int kinds[6];
		nfds_t count = 0;

		sync_frame_timer(timer_fd, &timer_armed);
		if (dbus_fd >= 0) {
			fds[count] = (struct pollfd){ .fd = dbus_fd, .events = POLLIN };
			kinds[count++] = 1;
		}
		if (inotify_fd >= 0) {
			fds[count] = (struct pollfd){ .fd = inotify_fd, .events = POLLIN };
			kinds[count++] = 2;
		}
		if (timer_fd >= 0) {
			fds[count] = (struct pollfd){ .fd = timer_fd, .events = POLLIN };
			kinds[count++] = 3;
		}
		if (status_stream.fd >= 0) {
			fds[count] = (struct pollfd){ .fd = status_stream.fd, .events = POLLIN };
			kinds[count++] = 4;
		}
		if (notify_stream.fd >= 0) {
			fds[count] = (struct pollfd){ .fd = notify_stream.fd, .events = POLLIN };
			kinds[count++] = 5;
		}
		if (emu_stream.fd >= 0) {
			fds[count] = (struct pollfd){ .fd = emu_stream.fd, .events = POLLIN };
			kinds[count++] = 6;
		}

		if (poll(fds, count, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (nfds_t i = 0; i < count; i++) {
			if (!fds[i].revents)
				continue;
			switch (kinds[i]) {
			case 1:
				(void)dbus_connection_read_write(bus, 0);
				while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS)
					;
				break;
			case 2: {
				char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
				bool user_changed = false;
				bool theme_changed = false;
				bool sockets = false;
				ssize_t n;

				while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
					for (size_t off = 0; off < (size_t)n;) {
						struct inotify_event *ev = (struct inotify_event *)(buf + off);

						if (ev->wd == user_watch)
							user_changed = true;
						else if (ev->wd == runtime_watch && ev->len &&
							 strstr(ev->name, ".sock"))
							sockets = true;
						else if (ev->wd == state_watch && ev->len &&
							 strcmp(ev->name, "settings.conf") == 0)
							theme_changed = true;
						off += sizeof(*ev) + ev->len;
					}
				}
				if (user_changed) {
					switch_user();
					state_watch = watch_user_state(inotify_fd, state_watch);
				} else if (theme_changed) {
					load_theme_accent();
					schedule_crossfade();
				}
				if (sockets)
					connect_sources();
				break;
			}
			case 3: {
				uint64_t expirations;

				if (read(timer_fd, &expirations, sizeof(expirations)) < 0)
					expirations = 0;
				break;
			}
			case 4:
				stream_read(&status_stream, status_line);
				break;
			case 5:
				stream_read(&notify_stream, notification);
				break;
			case 6:
				stream_read(&emu_stream, emu_line);
				break;
			}
		}
		render();
	}

	reply_pending_sleep();
	hardware_off_now();
	stream_close(&status_stream);
	stream_close(&notify_stream);
	stream_close(&emu_stream);
	rgb_disconnect();
	if (timer_fd >= 0)
		close(timer_fd);
	if (inotify_fd >= 0)
		close(inotify_fd);
	dbus_connection_unregister_object_path(bus, OBJECT_PATH);
	dbus_connection_unref(bus);
	return 0;
}
