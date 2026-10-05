/* SPDX-License-Identifier: MIT */

#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <linux/netlink.h>

#include <nuubos/notify.h>

#define SERVICE_NAME "org.nuubOS.Controllers"
#define OBJECT_PATH "/org/nuubOS/Controllers"
#define INTERFACE_NAME "org.nuubOS.Controllers1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"

#define INPUT_DIR "/dev/input"
#define INPUT_SOCKET "/run/nuubos/inputd.sock"
#define USER_DIR "/run/nuubos/user"
#define ACTIVE_USER USER_DIR "/active"
#define INPUTD_PID "/run/nuubos-inputd.pid"

#define MAX_CONTROLLERS 16
#define MAX_LINE 512
#define MAX_ID 160
#define MAX_NAME 160

/* Accessory battery alerts; the console battery belongs to statusd. */
#define BATTERY_LOW_PERCENT 15
#define BATTERY_REARM_PERCENT 20
/* A HID battery is usually reported a moment after the input node appears;
 * inside this window it completes the "connected" notification in place. */
#define BATTERY_LATE_UPDATE_MS 15000LL
#define MAX_BATTERY_CACHE 8

#define BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define NBITS(n) (((n) + BITS_PER_LONG - 1U) / BITS_PER_LONG)

struct controller_device {
	char id[MAX_ID];
	char name[MAX_NAME];
	char transport[24];
	bool connected;
	bool builtin;
	int preferred_player;
	int effective_player;
	/* sysfs device owning the input node (HID device / USB interface),
	 * without the /sys prefix: its power_supply children are the
	 * controller's battery. */
	char sys_parent[256];
	int battery;
	bool battery_low_sent;
	long long connected_ms;
};

struct battery_cache_entry {
	char devpath[256];
	int capacity;
};

struct player_pref {
	char id[MAX_ID];
	char name[MAX_NAME];
};

enum control_kind {
	CONTROL_BUTTON = 0,
	CONTROL_AXIS = 1,
};

enum source_type {
	SOURCE_NONE = 0,
	SOURCE_KEY,
	SOURCE_ABS,
};

/*
 * One physical source bound to a logical control. For SOURCE_ABS, dir is
 * +1/-1 for the half axis above/below centre (on an axis control: normal or
 * inverted), +2 for full travel rising from the minimum (a trigger resting at
 * its minimum) and -2 for full travel falling from the maximum.
 *
 * Persisted as key:CODE, abs:CODE:+, abs:CODE:-, abs:CODE:>, abs:CODE:< or
 * none. Mapping files hold only the controls that differ from the
 * capability-derived defaults; a bare number is a legacy key binding.
 */
struct source {
	int type;
	int code;
	int dir;
};

struct control_def {
	const char *id;
	int kind;
	int default_key;
};

static const struct control_def controls[] = {
	/* Stable Settings order: face cluster, D-pad, shoulders/triggers,
	 * stick clicks, system/navigation buttons, then stick axes. */
	{ "menu_back", CONTROL_BUTTON, BTN_SOUTH },
	{ "menu_confirm", CONTROL_BUTTON, BTN_EAST },
	{ "face_north", CONTROL_BUTTON, BTN_NORTH },
	{ "face_west", CONTROL_BUTTON, BTN_WEST },
	{ "menu_up", CONTROL_BUTTON, BTN_DPAD_UP },
	{ "menu_down", CONTROL_BUTTON, BTN_DPAD_DOWN },
	{ "menu_left", CONTROL_BUTTON, BTN_DPAD_LEFT },
	{ "menu_right", CONTROL_BUTTON, BTN_DPAD_RIGHT },
	{ "l1", CONTROL_BUTTON, BTN_TL },
	{ "r1", CONTROL_BUTTON, BTN_TR },
	{ "l2", CONTROL_BUTTON, BTN_TL2 },
	{ "r2", CONTROL_BUTTON, BTN_TR2 },
	{ "l3", CONTROL_BUTTON, BTN_THUMBL },
	{ "r3", CONTROL_BUTTON, BTN_THUMBR },
	{ "settings", CONTROL_BUTTON, BTN_START },
	{ "select", CONTROL_BUTTON, BTN_SELECT },
	{ "quick_menu", CONTROL_BUTTON, BTN_MODE },
	{ "left_x", CONTROL_AXIS, -1 },
	{ "left_y", CONTROL_AXIS, -1 },
	{ "right_x", CONTROL_AXIS, -1 },
	{ "right_y", CONTROL_AXIS, -1 },
};

#define CONTROL_COUNT (sizeof(controls) / sizeof(controls[0]))
#define DEFAULT_DEADZONE 20
/* A capture with no usable input for this long is cancelled, so Back can be
 * assigned like any other button and a stuck capture never locks Settings. */
#define REMAP_TIMEOUT_MS 5000
/* Normalized (-100..100) thresholds used to recognise a deliberate axis
 * gesture during capture; an axis must rest near centre (or, for a trigger,
 * at one extreme) when capture starts. */
#define CAPTURE_REST_PERCENT 40
#define CAPTURE_MOVE_PERCENT 60
#define CAPTURE_EXTREME_PERCENT 80

struct device_caps {
	bool present;
	unsigned long keys[NBITS(KEY_MAX + 1)];
	unsigned long abs[NBITS(ABS_MAX + 1)];
};

static volatile sig_atomic_t running = 1;
static struct controller_device devices[MAX_CONTROLLERS];
static size_t device_count;
static struct player_pref prefs[4];
static char active_user[128] = "default";
static int input_fd = -1;
static char input_buf[2048];
static size_t input_used;
static bool tester_enabled;
static char tester_id[MAX_ID];
static struct source tester_map[CONTROL_COUNT];
static bool tester_hotkey_pressed;
static bool tester_start_pressed;
static bool remap_pending;
static char remap_id[MAX_ID];
static size_t remap_control;
static struct source remap_map[CONTROL_COUNT];
static long long remap_deadline_ms;
static bool remap_baseline_ready;
static int remap_baseline[ABS_MAX + 1];

static struct battery_cache_entry battery_cache[MAX_BATTERY_CACHE];

static void inputd_raw_capture(const char *id, bool enabled);
static long long monotonic_ms(void);

static const char introspection_xml[] =
	"<node>"
	"<interface name='org.nuubOS.Controllers1'>"
	"<method name='GetSnapshot'>"
	"<arg name='active_user' type='s' direction='out'/>"
	"<arg name='connected_count' type='u' direction='out'/>"
	"</method>"
	"<method name='GetDevices'>"
	"<arg name='devices' type='a(sssbbii)' direction='out'/>"
	"</method>"
	"<method name='GetAssignments'>"
	"<arg name='assignments' type='a(issb)' direction='out'/>"
	"</method>"
	"<method name='SetPlayerPreference'>"
	"<arg name='player' type='i' direction='in'/>"
	"<arg name='controller_id' type='s' direction='in'/>"
	"<arg name='controller_name' type='s' direction='in'/>"
	"</method>"
	"<method name='GetMapping'>"
	"<arg name='controller_id' type='s' direction='in'/>"
	"<arg name='mapping' type='a(sisb)' direction='out'/>"
	"</method>"
	"<method name='GetDeadzones'>"
	"<arg name='controller_id' type='s' direction='in'/>"
	"<arg name='left_percent' type='i' direction='out'/>"
	"<arg name='right_percent' type='i' direction='out'/>"
	"</method>"
	"<method name='SetDeadzone'>"
	"<arg name='controller_id' type='s' direction='in'/>"
	"<arg name='stick' type='s' direction='in'/>"
	"<arg name='percent' type='i' direction='in'/>"
	"</method>"
	"<method name='BeginRemap'>"
	"<arg name='controller_id' type='s' direction='in'/>"
	"<arg name='action' type='s' direction='in'/>"
	"</method>"
	"<method name='CancelRemap'/>"
	"<method name='ResetMapping'>"
	"<arg name='controller_id' type='s' direction='in'/>"
	"</method>"
	"<method name='ResetMappings'/>"
	"<method name='SetTester'>"
	"<arg name='enabled' type='b' direction='in'/>"
	"<arg name='controller_id' type='s' direction='in'/>"
	"</method>"
	"<signal name='DevicesChanged'>"
	"<arg name='devices' type='a(sssbbii)'/>"
	"</signal>"
	"<signal name='AssignmentsChanged'>"
	"<arg name='assignments' type='a(issb)'/>"
	"</signal>"
	"<signal name='MappingChanged'>"
	"<arg name='controller_id' type='s'/>"
	"</signal>"
	"<signal name='InputEvent'>"
	"<arg name='controller_id' type='s'/>"
	"<arg name='control' type='s'/>"
	"<arg name='event_type' type='i'/>"
	"<arg name='code' type='i'/>"
	"<arg name='value' type='i'/>"
	"</signal>"
	"<signal name='RemapCaptured'>"
	"<arg name='controller_id' type='s'/>"
	"<arg name='control' type='s'/>"
	"<arg name='source' type='s'/>"
	"</signal>"
	"<signal name='RemapCancelled'>"
	"<arg name='controller_id' type='s'/>"
	"<arg name='control' type='s'/>"
	"</signal>"
	"<signal name='TesterExitRequested'>"
	"<arg name='controller_id' type='s'/>"
	"</signal>"
	"<signal name='OperationFailed'>"
	"<arg name='operation' type='s'/>"
	"<arg name='message' type='s'/>"
	"</signal>"
	"</interface>"
	"</node>";

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

static bool bit_is_set(const unsigned long *bits, unsigned int bit)
{
	unsigned int word = bit / BITS_PER_LONG;
	unsigned int offset = bit % BITS_PER_LONG;
	return (bits[word] & (1UL << offset)) != 0;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
	size_t len;

	if (!dst || dst_size == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	len = strnlen(src, dst_size - 1);
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void trim(char *text)
{
	size_t len;
	if (!text)
		return;
	len = strlen(text);
	while (len > 0 &&
	       (text[len - 1] == '\n' || text[len - 1] == '\r' ||
		text[len - 1] == ' ' || text[len - 1] == '\t'))
		text[--len] = '\0';
}

static void sanitize_token(const char *src, char *dst, size_t dst_size)
{
	size_t used = 0;

	while (src && *src && used + 1 < dst_size) {
		unsigned char c = (unsigned char)*src++;
		if ((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == ':' || c == '.')
			dst[used++] = (char)c;
		else
			dst[used++] = '_';
	}
	dst[used] = '\0';
}

static void refresh_active_user(void)
{
	FILE *fp = fopen(ACTIVE_USER, "r");
	char value[sizeof(active_user)];

	snprintf(value, sizeof(value), "default");
	if (fp) {
		if (fgets(value, sizeof(value), fp) == NULL)
			snprintf(value, sizeof(value), "default");
		fclose(fp);
		trim(value);
		if (value[0] == '\0')
			snprintf(value, sizeof(value), "default");
	}
	copy_text(active_user, sizeof(active_user), value);
}

static int mkdir_if_needed(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static int ensure_user_dirs(void)
{
	char path[384];

	if (mkdir_if_needed("/state/users") != 0)
		return -1;
	snprintf(path, sizeof(path), "/state/users/%s", active_user);
	if (mkdir_if_needed(path) != 0)
		return -1;
	snprintf(path, sizeof(path), "/state/users/%s/controllers", active_user);
	return mkdir_if_needed(path);
}

static void prefs_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "/state/users/%s/controllers.conf", active_user);
}

static void mapping_path(const char *id, char *out, size_t out_size)
{
	char encoded[MAX_ID * 2 + 1];
	size_t i;
	size_t used = 0;
	static const char hex[] = "0123456789abcdef";

	for (i = 0; id && id[i] != '\0' && used + 2 < sizeof(encoded); i++) {
		unsigned char c = (unsigned char)id[i];
		encoded[used++] = hex[c >> 4];
		encoded[used++] = hex[c & 0x0f];
	}
	encoded[used] = '\0';

	snprintf(out, out_size, "/state/users/%s/controllers/%s.conf",
		 active_user, encoded[0] ? encoded : "default");
}

static void compute_controller_id(int fd, const char *name,
				  char *out, size_t out_size)
{
	struct input_id id;
	char uniq[128];
	char safe[128];

	if (strcmp(name, "adc-joystick") == 0 ||
	    strcmp(name, "gpio-keys-gamepad") == 0) {
		snprintf(out, out_size, "builtin");
		return;
	}

	memset(&id, 0, sizeof(id));
	(void)ioctl(fd, EVIOCGID, &id);
	memset(uniq, 0, sizeof(uniq));
	if (ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq) >= 0 && uniq[0]) {
		sanitize_token(uniq, safe, sizeof(safe));
		snprintf(out, out_size, "bus%04x:%s", id.bustype, safe);
		return;
	}

	sanitize_token(name, safe, sizeof(safe));
	snprintf(out, out_size, "bus%04x-v%04x-p%04x:%s",
		 id.bustype, id.vendor, id.product, safe);
}

static const char *transport_name(int fd, bool builtin)
{
	struct input_id id;
	if (builtin)
		return "Built-in";
	memset(&id, 0, sizeof(id));
	if (ioctl(fd, EVIOCGID, &id) < 0)
		return "Other";
	if (id.bustype == BUS_BLUETOOTH)
		return "Bluetooth";
	if (id.bustype == BUS_USB)
		return "USB";
	return "Other";
}

static bool looks_like_controller(int fd, const char *name)
{
	unsigned long evbits[NBITS(EV_MAX + 1)];
	unsigned long keybits[NBITS(KEY_MAX + 1)];

	if (strcmp(name, "adc-joystick") == 0 ||
	    strcmp(name, "gpio-keys-gamepad") == 0)
		return true;

	memset(evbits, 0, sizeof(evbits));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
		return false;

	if (!bit_is_set(evbits, EV_KEY))
		return false;

	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
		return false;

	return bit_is_set(keybits, BTN_SOUTH) ||
	       bit_is_set(keybits, BTN_EAST) ||
	       bit_is_set(keybits, BTN_GAMEPAD) ||
	       bit_is_set(keybits, BTN_START);
}

static int find_device(const char *id)
{
	size_t i;
	for (i = 0; i < device_count; i++)
		if (strcmp(devices[i].id, id) == 0)
			return (int)i;
	return -1;
}

static void load_preferences(void)
{
	char path[384];
	FILE *fp;
	char line[512];
	int i;

	for (i = 0; i < 4; i++) {
		prefs[i].id[0] = '\0';
		prefs[i].name[0] = '\0';
	}

	prefs_path(path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		int player;
		char value[384];

		if (sscanf(line, "PLAYER%d_ID=%383[^\n]", &player, value) == 2 &&
		    player >= 1 && player <= 4) {
			trim(value);
			copy_text(prefs[player - 1].id, sizeof(prefs[player - 1].id), value);
			continue;
		}
		if (sscanf(line, "PLAYER%d_NAME=%383[^\n]", &player, value) == 2 &&
		    player >= 1 && player <= 4) {
			trim(value);
			copy_text(prefs[player - 1].name, sizeof(prefs[player - 1].name), value);
		}
	}
	fclose(fp);
}

static int save_preferences(void)
{
	char path[384];
	char tmp[420];
	FILE *fp;
	int i;

	if (ensure_user_dirs() != 0)
		return -1;
	prefs_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());

	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	for (i = 0; i < 4; i++) {
		if (fprintf(fp, "PLAYER%d_ID=%s\nPLAYER%d_NAME=%s\n",
			    i + 1, prefs[i].id, i + 1, prefs[i].name) < 0) {
			fclose(fp);
			unlink(tmp);
			return -1;
		}
	}
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0 || fclose(fp) != 0) {
		unlink(tmp);
		return -1;
	}
	if (chmod(tmp, 0644) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int preferred_player_for(const char *id)
{
	int i;
	for (i = 0; i < 4; i++)
		if (prefs[i].id[0] && strcmp(prefs[i].id, id) == 0)
			return i + 1;
	return 0;
}

static void recompute_effective_players(void)
{
	bool used[MAX_CONTROLLERS] = { false };
	int next = 1;
	int p;
	size_t i;

	for (i = 0; i < device_count; i++) {
		devices[i].preferred_player = preferred_player_for(devices[i].id);
		devices[i].effective_player = 0;
	}

	for (p = 0; p < 4; p++) {
		int idx;
		if (!prefs[p].id[0])
			continue;
		idx = find_device(prefs[p].id);
		if (idx < 0 || used[idx])
			continue;
		devices[idx].effective_player = next++;
		used[idx] = true;
	}

	for (i = 0; i < device_count && next <= 4; i++) {
		if (used[i])
			continue;
		devices[i].effective_player = next++;
		used[i] = true;
	}
}

static void resolve_sys_parent(const char *event_path, char *out, size_t out_size)
{
	char link[96];
	char real[PATH_MAX];
	const char *node = strrchr(event_path, '/');

	out[0] = '\0';
	snprintf(link, sizeof(link), "/sys/class/input/%s/device/device",
		 node ? node + 1 : event_path);
	if (realpath(link, real) && strncmp(real, "/sys/", 5) == 0)
		copy_text(out, out_size, real + 4);
}

static bool battery_belongs_to(const char *devpath, const char *sys_parent)
{
	size_t len = strlen(sys_parent);

	return len > 0 && strncmp(devpath, sys_parent, len) == 0 &&
	       strncmp(devpath + len, "/power_supply/", 14) == 0;
}

static int cached_battery(const char *sys_parent)
{
	for (size_t i = 0; i < MAX_BATTERY_CACHE; i++)
		if (battery_cache[i].devpath[0] &&
		    battery_belongs_to(battery_cache[i].devpath, sys_parent))
			return battery_cache[i].capacity;
	return -1;
}

static void controller_notify_id(const char *id, const char *prefix,
				 char *out, size_t out_size)
{
	uint32_t h = 2166136261u;

	for (const char *p = id; *p; p++) {
		h ^= (unsigned char)*p;
		h *= 16777619u;
	}
	snprintf(out, out_size, "%s:%08x", prefix, (unsigned int)h);
}

static void notify_controller(const char *verb, const char *event,
			      const struct controller_device *dev)
{
	struct nuubos_notify n;
	char id[48];

	controller_notify_id(dev->id, "controller", id, sizeof(id));
	nuubos_notify_begin(&n, verb, id, event);
	nuubos_notify_str(&n, "name", dev->name);
	if (strcmp(event, "controller.connected") == 0) {
		if (dev->effective_player > 0)
			nuubos_notify_int(&n, "player", dev->effective_player);
		if (dev->battery >= 0)
			nuubos_notify_int(&n, "battery", dev->battery);
	}
	(void)nuubos_notify_send(&n);
}

static void notify_controller_battery_low(const struct controller_device *dev)
{
	struct nuubos_notify n;
	char id[48];

	controller_notify_id(dev->id, "controller-battery", id, sizeof(id));
	nuubos_notify_begin(&n, "POST", id, "accessory.battery.low");
	nuubos_notify_str(&n, "name", dev->name);
	nuubos_notify_int(&n, "percent", dev->battery);
	nuubos_notify_str(&n, "kind", "controller");
	(void)nuubos_notify_send(&n);
}

static void rescan_devices(bool announce)
{
	static struct controller_device previous[MAX_CONTROLLERS];
	size_t previous_count = device_count;
	glob_t g;
	size_t i;

	memcpy(previous, devices, sizeof(previous));
	device_count = 0;

	if (glob(INPUT_DIR "/event*", 0, NULL, &g) != 0) {
		recompute_effective_players();
		return;
	}

	for (i = 0; i < g.gl_pathc && device_count < MAX_CONTROLLERS; i++) {
		int fd;
		char name[MAX_NAME] = "";
		char id[MAX_ID];
		bool builtin;
		int existing;

		fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
			snprintf(name, sizeof(name), "Unknown Controller");

		if (!looks_like_controller(fd, name)) {
			close(fd);
			continue;
		}

		compute_controller_id(fd, name, id, sizeof(id));
		existing = find_device(id);
		if (existing >= 0) {
			close(fd);
			continue;
		}

		builtin = strcmp(id, "builtin") == 0;
		copy_text(devices[device_count].id, sizeof(devices[device_count].id), id);
		copy_text(devices[device_count].name, sizeof(devices[device_count].name),
			  builtin ? "Built-in Controls" : name);
		copy_text(devices[device_count].transport,
			  sizeof(devices[device_count].transport),
			  transport_name(fd, builtin));
		devices[device_count].connected = true;
		devices[device_count].builtin = builtin;
		resolve_sys_parent(g.gl_pathv[i], devices[device_count].sys_parent,
				   sizeof(devices[device_count].sys_parent));
		devices[device_count].battery =
			cached_battery(devices[device_count].sys_parent);
		devices[device_count].battery_low_sent = false;
		devices[device_count].connected_ms = monotonic_ms();
		for (size_t p = 0; p < previous_count; p++) {
			if (strcmp(previous[p].id, id) == 0) {
				devices[device_count].battery = previous[p].battery;
				devices[device_count].battery_low_sent = previous[p].battery_low_sent;
				devices[device_count].connected_ms = previous[p].connected_ms;
				break;
			}
		}
		device_count++;
		close(fd);
	}

	globfree(&g);
	recompute_effective_players();

	if (!announce)
		return;

	/* Controller Management owns connect/disconnect notifications for every
	 * transport. The built-in controls are never announced. */
	for (i = 0; i < device_count; i++) {
		bool known = false;

		for (size_t p = 0; p < previous_count && !known; p++)
			known = strcmp(previous[p].id, devices[i].id) == 0;
		if (!known && !devices[i].builtin)
			notify_controller("POST", "controller.connected", &devices[i]);
	}
	for (size_t p = 0; p < previous_count; p++) {
		if (previous[p].builtin || find_device(previous[p].id) >= 0)
			continue;
		notify_controller("POST", "controller.disconnected", &previous[p]);
	}
}

static int make_uevent_socket(void)
{
	struct sockaddr_nl addr;
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
			NETLINK_KOBJECT_UEVENT);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = 1;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Device-scoped power_supply uevents carry the accessory battery level
 * (POWER_SUPPLY_CAPACITY), so controller batteries are tracked without
 * reading sysfs capacity (which can block on a HID GET_REPORT).
 */
static void handle_battery_uevent(const char *buf, size_t len)
{
	const char *action = NULL;
	const char *devpath = NULL;
	const char *subsystem = NULL;
	const char *scope = NULL;
	int capacity = -1;
	int slot = -1;

	for (size_t off = 0; off < len; off += strlen(buf + off) + 1) {
		const char *kv = buf + off;

		if (!strncmp(kv, "ACTION=", 7))
			action = kv + 7;
		else if (!strncmp(kv, "DEVPATH=", 8))
			devpath = kv + 8;
		else if (!strncmp(kv, "SUBSYSTEM=", 10))
			subsystem = kv + 10;
		else if (!strncmp(kv, "POWER_SUPPLY_SCOPE=", 19))
			scope = kv + 19;
		else if (!strncmp(kv, "POWER_SUPPLY_CAPACITY=", 22))
			capacity = atoi(kv + 22);
	}
	if (!subsystem || strcmp(subsystem, "power_supply") != 0 || !devpath ||
	    !action || (scope && strcmp(scope, "Device") != 0))
		return;

	for (int i = 0; i < MAX_BATTERY_CACHE; i++) {
		if (!strcmp(battery_cache[i].devpath, devpath)) {
			slot = i;
			break;
		}
		if (slot < 0 && battery_cache[i].devpath[0] == '\0')
			slot = i;
	}
	if (!strcmp(action, "remove")) {
		if (slot >= 0 && !strcmp(battery_cache[slot].devpath, devpath))
			battery_cache[slot].devpath[0] = '\0';
		return;
	}
	if (capacity < 0 || capacity > 100)
		return;
	if (slot >= 0) {
		copy_text(battery_cache[slot].devpath, sizeof(battery_cache[slot].devpath), devpath);
		battery_cache[slot].capacity = capacity;
	}

	for (size_t i = 0; i < device_count; i++) {
		struct controller_device *dev = &devices[i];
		bool first = dev->battery < 0;

		if (dev->builtin || !battery_belongs_to(devpath, dev->sys_parent))
			continue;
		dev->battery = capacity;
		if (first && monotonic_ms() - dev->connected_ms < BATTERY_LATE_UPDATE_MS)
			notify_controller("UPDATE", "controller.connected", dev);
		if (capacity > BATTERY_REARM_PERCENT) {
			dev->battery_low_sent = false;
		} else if (capacity <= BATTERY_LOW_PERCENT && !dev->battery_low_sent) {
			dev->battery_low_sent = true;
			notify_controller_battery_low(dev);
		}
	}
}

static bool device_available(const char *id)
{
	return id && id[0] && find_device(id) >= 0;
}

static int control_index(const char *id)
{
	size_t i;

	for (i = 0; i < CONTROL_COUNT; i++)
		if (strcmp(controls[i].id, id) == 0)
			return (int)i;
	return -1;
}

static bool is_accelerometer(int fd)
{
	unsigned long props[NBITS(INPUT_PROP_MAX + 1)];

	memset(props, 0, sizeof(props));
	if (ioctl(fd, EVIOCGPROP(sizeof(props)), props) < 0)
		return false;
	return bit_is_set(props, INPUT_PROP_ACCELEROMETER);
}

/* Union of the key/axis capabilities of every event node of one controller
 * (the built-in controls are split over gpio-keys and adc-joystick). Motion
 * sensor nodes share a pad's id but are not mappable controls. */
static void load_device_caps(const char *id, struct device_caps *caps)
{
	glob_t g;
	size_t i;

	memset(caps, 0, sizeof(*caps));
	if (glob(INPUT_DIR "/event*", 0, NULL, &g) != 0)
		return;

	for (i = 0; i < g.gl_pathc; i++) {
		unsigned long keys[NBITS(KEY_MAX + 1)];
		unsigned long abs[NBITS(ABS_MAX + 1)];
		char name[MAX_NAME] = "";
		char node_id[MAX_ID];
		size_t w;
		int fd;

		fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
			snprintf(name, sizeof(name), "Unknown Controller");
		compute_controller_id(fd, name, node_id, sizeof(node_id));
		if (strcmp(node_id, id) != 0 || is_accelerometer(fd)) {
			close(fd);
			continue;
		}

		memset(keys, 0, sizeof(keys));
		memset(abs, 0, sizeof(abs));
		(void)ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
		(void)ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
		for (w = 0; w < NBITS(KEY_MAX + 1); w++)
			caps->keys[w] |= keys[w];
		for (w = 0; w < NBITS(ABS_MAX + 1); w++)
			caps->abs[w] |= abs[w];
		caps->present = true;
		close(fd);
	}
	globfree(&g);
}

static void set_source(struct source *src, int type, int code, int dir)
{
	src->type = type;
	src->code = code;
	src->dir = dir;
}

/*
 * Defaults follow Linux gamepad semantics, resolved against what the
 * controller actually reports: a D-pad may be keys or hat 0; L2/R2 may be
 * keys, ABS_Z/ABS_RZ (xpad style, right stick on RX/RY) or ABS_BRAKE/ABS_GAS
 * (HID pads, right stick on Z/RZ). Controls the device lacks stay unassigned.
 */
static void default_mapping(const struct device_caps *caps, struct source *map)
{
	static const struct { const char *id; int key; int axis; int dir; } dpad[] = {
		{ "menu_up", BTN_DPAD_UP, ABS_HAT0Y, -1 },
		{ "menu_down", BTN_DPAD_DOWN, ABS_HAT0Y, 1 },
		{ "menu_left", BTN_DPAD_LEFT, ABS_HAT0X, -1 },
		{ "menu_right", BTN_DPAD_RIGHT, ABS_HAT0X, 1 },
	};
	bool has_rx;
	bool has_ry;
	bool z_triggers;
	size_t i;

	for (i = 0; i < CONTROL_COUNT; i++) {
		if (controls[i].kind == CONTROL_BUTTON)
			set_source(&map[i], SOURCE_KEY, controls[i].default_key, 0);
		else
			set_source(&map[i], SOURCE_NONE, 0, 0);
	}

	if (!caps->present) {
		/* Disconnected controller: plain gamepad layout. */
		set_source(&map[control_index("left_x")], SOURCE_ABS, ABS_X, 1);
		set_source(&map[control_index("left_y")], SOURCE_ABS, ABS_Y, 1);
		set_source(&map[control_index("right_x")], SOURCE_ABS, ABS_RX, 1);
		set_source(&map[control_index("right_y")], SOURCE_ABS, ABS_RY, 1);
		return;
	}

	for (i = 0; i < sizeof(dpad) / sizeof(dpad[0]); i++) {
		if (!bit_is_set(caps->keys, (unsigned int)dpad[i].key) &&
		    bit_is_set(caps->abs, (unsigned int)dpad[i].axis))
			set_source(&map[control_index(dpad[i].id)], SOURCE_ABS,
				   dpad[i].axis, dpad[i].dir);
	}

	has_rx = bit_is_set(caps->abs, ABS_RX);
	has_ry = bit_is_set(caps->abs, ABS_RY);
	z_triggers = !bit_is_set(caps->abs, ABS_BRAKE) &&
		     !bit_is_set(caps->abs, ABS_GAS) && (has_rx || has_ry);

	if (!bit_is_set(caps->keys, BTN_TL2)) {
		if (z_triggers && bit_is_set(caps->abs, ABS_Z))
			set_source(&map[control_index("l2")], SOURCE_ABS, ABS_Z, 2);
		else if (bit_is_set(caps->abs, ABS_BRAKE))
			set_source(&map[control_index("l2")], SOURCE_ABS, ABS_BRAKE, 2);
	}
	if (!bit_is_set(caps->keys, BTN_TR2)) {
		if (z_triggers && bit_is_set(caps->abs, ABS_RZ))
			set_source(&map[control_index("r2")], SOURCE_ABS, ABS_RZ, 2);
		else if (bit_is_set(caps->abs, ABS_GAS))
			set_source(&map[control_index("r2")], SOURCE_ABS, ABS_GAS, 2);
	}

	if (bit_is_set(caps->abs, ABS_X))
		set_source(&map[control_index("left_x")], SOURCE_ABS, ABS_X, 1);
	if (bit_is_set(caps->abs, ABS_Y))
		set_source(&map[control_index("left_y")], SOURCE_ABS, ABS_Y, 1);
	if (has_rx)
		set_source(&map[control_index("right_x")], SOURCE_ABS, ABS_RX, 1);
	else if (!z_triggers && bit_is_set(caps->abs, ABS_Z))
		set_source(&map[control_index("right_x")], SOURCE_ABS, ABS_Z, 1);
	if (has_ry)
		set_source(&map[control_index("right_y")], SOURCE_ABS, ABS_RY, 1);
	else if (!z_triggers && bit_is_set(caps->abs, ABS_RZ))
		set_source(&map[control_index("right_y")], SOURCE_ABS, ABS_RZ, 1);
}

static bool parse_source(const char *text, int kind, struct source *src)
{
	int code;
	char dir;
	char tail;

	if (strcmp(text, "none") == 0) {
		set_source(src, SOURCE_NONE, 0, 0);
		return true;
	}
	if (sscanf(text, "abs:%d:%c%c", &code, &dir, &tail) == 2 &&
	    code >= 0 && code <= ABS_MAX) {
		int value = dir == '+' ? 1 : dir == '-' ? -1
			  : dir == '>' ? 2 : dir == '<' ? -2 : 0;
		if (value == 0 || (kind == CONTROL_AXIS && (value == 2 || value == -2)))
			return false;
		set_source(src, SOURCE_ABS, code, value);
		return true;
	}
	if (kind != CONTROL_BUTTON)
		return false;
	if ((sscanf(text, "key:%d%c", &code, &tail) == 1 ||
	     sscanf(text, "%d%c", &code, &tail) == 1) &&
	    code >= 0 && code <= KEY_MAX) {
		set_source(src, SOURCE_KEY, code, 0);
		return true;
	}
	return false;
}

static void format_source(const struct source *src, char *out, size_t out_size)
{
	if (src->type == SOURCE_KEY)
		snprintf(out, out_size, "key:%d", src->code);
	else if (src->type == SOURCE_ABS)
		snprintf(out, out_size, "abs:%d:%c", src->code,
			 src->dir == 1 ? '+' : src->dir == -1 ? '-'
			 : src->dir == 2 ? '>' : '<');
	else
		snprintf(out, out_size, "none");
}

static bool sources_equal(const struct source *a, const struct source *b)
{
	if (a->type != b->type)
		return false;
	if (a->type == SOURCE_NONE)
		return true;
	return a->code == b->code && (a->type == SOURCE_KEY || a->dir == b->dir);
}

static void load_mapping_with_defaults(const char *id, struct source *map,
				       struct source *defaults)
{
	struct device_caps caps;
	char path[512];
	char line[256];
	FILE *fp;

	load_device_caps(id, &caps);
	default_mapping(&caps, defaults);
	memcpy(map, defaults, sizeof(*map) * CONTROL_COUNT);

	mapping_path(id, path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char name[64];
		char value[64];
		struct source src;
		int index;

		if (sscanf(line, "%63[^=]=%63s", name, value) != 2)
			continue;
		index = control_index(name);
		if (index >= 0 && parse_source(value, controls[index].kind, &src))
			map[index] = src;
	}
	fclose(fp);
}

static void load_mapping(const char *id, struct source *map)
{
	struct source defaults[CONTROL_COUNT];

	load_mapping_with_defaults(id, map, defaults);
}

static void load_deadzones(const char *id, int *left, int *right)
{
	char path[512];
	FILE *fp;
	char line[256];

	*left = DEFAULT_DEADZONE;
	*right = DEFAULT_DEADZONE;
	mapping_path(id, path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		int value;
		if (sscanf(line, "LEFT_DEADZONE=%d", &value) == 1)
			*left = value;
		else if (sscanf(line, "RIGHT_DEADZONE=%d", &value) == 1)
			*right = value;
	}
	fclose(fp);

	if (*left < 0) *left = 0;
	if (*left > 50) *left = 50;
	if (*right < 0) *right = 0;
	if (*right > 50) *right = 50;
}

/* Writes only what differs from the defaults; nothing left means no file. */
static int save_mapping_file(const char *id, const struct source *map,
			     int left_deadzone, int right_deadzone)
{
	struct device_caps caps;
	struct source defaults[CONTROL_COUNT];
	char path[512];
	char tmp[560];
	FILE *fp;
	size_t i;
	bool written = false;

	if (ensure_user_dirs() != 0)
		return -1;

	load_device_caps(id, &caps);
	default_mapping(&caps, defaults);

	mapping_path(id, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;

	for (i = 0; i < CONTROL_COUNT; i++) {
		char value[64];

		if (sources_equal(&map[i], &defaults[i]))
			continue;
		format_source(&map[i], value, sizeof(value));
		if (fprintf(fp, "%s=%s\n", controls[i].id, value) < 0)
			goto fail;
		written = true;
	}
	if (left_deadzone != DEFAULT_DEADZONE) {
		if (fprintf(fp, "LEFT_DEADZONE=%d\n", left_deadzone) < 0)
			goto fail;
		written = true;
	}
	if (right_deadzone != DEFAULT_DEADZONE) {
		if (fprintf(fp, "RIGHT_DEADZONE=%d\n", right_deadzone) < 0)
			goto fail;
		written = true;
	}

	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0 || fclose(fp) != 0) {
		unlink(tmp);
		return -1;
	}
	if (!written) {
		unlink(tmp);
		return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
	}
	if (chmod(tmp, 0644) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;

fail:
	fclose(fp);
	unlink(tmp);
	return -1;
}

static bool sources_overlap(const struct source *a, int a_kind,
			    const struct source *b, int b_kind)
{
	if (a->type == SOURCE_NONE || a->type != b->type || a->code != b->code)
		return false;
	if (a->type == SOURCE_KEY)
		return true;
	/* A stick axis owns the whole axis; a full-travel trigger likewise. */
	if (a_kind == CONTROL_AXIS || b_kind == CONTROL_AXIS ||
	    a->dir == 2 || a->dir == -2 || b->dir == 2 || b->dir == -2)
		return true;
	return a->dir == b->dir;
}

/*
 * One physical source drives at most one control. A control of the same
 * kind that already used the new source takes the target's previous source
 * (swap), so every navigation button stays reachable; a control of the other
 * kind (a D-pad hat taken over by a stick) becomes unassigned.
 */
static void assign_source(struct source *map, size_t target, const struct source *src)
{
	struct source previous = map[target];
	bool swapped = false;
	size_t i;

	for (i = 0; i < CONTROL_COUNT; i++) {
		if (i == target ||
		    !sources_overlap(src, controls[target].kind, &map[i], controls[i].kind))
			continue;
		if (!swapped && controls[i].kind == controls[target].kind) {
			map[i] = previous;
			swapped = true;
		} else {
			set_source(&map[i], SOURCE_NONE, 0, 0);
		}
	}
	map[target] = *src;
}

static int save_deadzone_value(const char *id, const char *stick, int percent)
{
	struct source map[CONTROL_COUNT];
	int left_deadzone;
	int right_deadzone;

	if (percent < 0 || percent > 50)
		return -1;
	load_mapping(id, map);
	load_deadzones(id, &left_deadzone, &right_deadzone);

	if (strcmp(stick, "left") == 0)
		left_deadzone = percent;
	else if (strcmp(stick, "right") == 0)
		right_deadzone = percent;
	else
		return -1;

	return save_mapping_file(id, map, left_deadzone, right_deadzone);
}

static void reload_inputd_mapping(void)
{
	FILE *fp = fopen(INPUTD_PID, "r");
	long pid = -1;
	if (!fp)
		return;
	if (fscanf(fp, "%ld", &pid) != 1)
		pid = -1;
	fclose(fp);
	if (pid > 1)
		(void)kill((pid_t)pid, SIGHUP);
}

/* Reset System Settings: every user returns to the approved default mapping
 * and deadzones. Mapping files only hold differences from the defaults, so
 * removing them is the reset; player assignments are left untouched. */
static int reset_all_mappings(void)
{
	glob_t found;
	size_t i;
	int rc = 0;

	if (glob("/state/users/*/controllers/*.conf", 0, NULL, &found) != 0)
		return 0;
	for (i = 0; i < found.gl_pathc; i++) {
		if (unlink(found.gl_pathv[i]) != 0 && errno != ENOENT)
			rc = -1;
	}
	globfree(&found);
	return rc;
}

/* Restore Default Mapping for one controller of the active user. */
static int reset_mapping(const char *id)
{
	char path[512];

	mapping_path(id, path, sizeof(path));
	if (unlink(path) != 0 && errno != ENOENT)
		return -1;
	return 0;
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static DBusMessage *new_error(DBusMessage *request, const char *name, const char *text)
{
	return dbus_message_new_error(request, name, text);
}

static void send_reply(DBusConnection *conn, DBusMessage *reply)
{
	if (!reply)
		return;
	dbus_connection_send(conn, reply, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(reply);
}

static bool append_devices_array(DBusMessageIter *parent)
{
	DBusMessageIter array;
	size_t i;

	if (!dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "(sssbbii)", &array))
		return false;

	for (i = 0; i < device_count; i++) {
		DBusMessageIter st;
		const char *id = devices[i].id;
		const char *name = devices[i].name;
		const char *transport = devices[i].transport;
		dbus_bool_t connected = devices[i].connected;
		dbus_bool_t builtin = devices[i].builtin;
		dbus_int32_t preferred = devices[i].preferred_player;
		dbus_int32_t effective = devices[i].effective_player;

		if (!dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &st))
			return false;
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &id);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &name);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &transport);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &connected);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &builtin);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &preferred);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &effective);
		dbus_message_iter_close_container(&array, &st);
	}
	return dbus_message_iter_close_container(parent, &array);
}

static bool append_assignments_array(DBusMessageIter *parent)
{
	DBusMessageIter array;
	int i;

	if (!dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "(issb)", &array))
		return false;

	for (i = 0; i < 4; i++) {
		DBusMessageIter st;
		dbus_int32_t player = i + 1;
		const char *id = prefs[i].id;
		const char *name = prefs[i].name;
		dbus_bool_t available = device_available(prefs[i].id);

		if (!dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &st))
			return false;
		dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &player);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &id);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &name);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &available);
		dbus_message_iter_close_container(&array, &st);
	}
	return dbus_message_iter_close_container(parent, &array);
}

static void emit_devices_changed(DBusConnection *conn)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "DevicesChanged");
	DBusMessageIter iter;
	if (!signal)
		return;
	dbus_message_iter_init_append(signal, &iter);
	if (append_devices_array(&iter)) {
		dbus_connection_send(conn, signal, NULL);
		dbus_connection_flush(conn);
	}
	dbus_message_unref(signal);
}

static void emit_assignments_changed(DBusConnection *conn)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "AssignmentsChanged");
	DBusMessageIter iter;
	if (!signal)
		return;
	dbus_message_iter_init_append(signal, &iter);
	if (append_assignments_array(&iter)) {
		dbus_connection_send(conn, signal, NULL);
		dbus_connection_flush(conn);
	}
	dbus_message_unref(signal);
}

static void emit_mapping_changed(DBusConnection *conn, const char *id)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "MappingChanged");
	if (!signal)
		return;
	dbus_message_append_args(signal, DBUS_TYPE_STRING, &id, DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static void emit_remap_captured(DBusConnection *conn, const char *id,
				const char *control, const char *source)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "RemapCaptured");
	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_STRING, &id,
				 DBUS_TYPE_STRING, &control,
				 DBUS_TYPE_STRING, &source,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static void emit_remap_cancelled(DBusConnection *conn, const char *id,
				 const char *control)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "RemapCancelled");
	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_STRING, &id,
				 DBUS_TYPE_STRING, &control,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static void emit_input_event(DBusConnection *conn, const char *id,
			     const char *control, int type, int code, int value)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "InputEvent");
	dbus_int32_t t = type;
	dbus_int32_t c = code;
	dbus_int32_t v = value;
	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_STRING, &id,
				 DBUS_TYPE_STRING, &control,
				 DBUS_TYPE_INT32, &t,
				 DBUS_TYPE_INT32, &c,
				 DBUS_TYPE_INT32, &v,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static void emit_tester_exit_requested(DBusConnection *conn, const char *id)
{
	DBusMessage *signal =
		dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "TesterExitRequested");
	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_STRING, &id,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static void reply_empty(DBusConnection *conn, DBusMessage *request)
{
	send_reply(conn, dbus_message_new_method_return(request));
}

static void end_remap(void)
{
	if (remap_pending)
		inputd_raw_capture(remap_id, false);
	remap_pending = false;
	remap_id[0] = '\0';
	remap_baseline_ready = false;
}

static void refresh_tester_map(void)
{
	if (tester_enabled)
		load_mapping(tester_id, tester_map);
}

static DBusHandlerResult handle_message(DBusConnection *conn,
					DBusMessage *message, void *data)
{
	(void)data;

	if (dbus_message_is_method_call(message, INTROSPECT_IFACE, "Introspect")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		const char *xml = introspection_xml;
		if (reply)
			dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetSnapshot")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		dbus_uint32_t count = (dbus_uint32_t)device_count;
		const char *user = active_user;
		if (reply)
			dbus_message_append_args(reply,
						 DBUS_TYPE_STRING, &user,
						 DBUS_TYPE_UINT32, &count,
						 DBUS_TYPE_INVALID);
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetDevices")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		DBusMessageIter iter;
		if (reply) {
			dbus_message_iter_init_append(reply, &iter);
			if (!append_devices_array(&iter)) {
				dbus_message_unref(reply);
				reply = NULL;
			}
		}
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetAssignments")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		DBusMessageIter iter;
		if (reply) {
			dbus_message_iter_init_append(reply, &iter);
			if (!append_assignments_array(&iter)) {
				dbus_message_unref(reply);
				reply = NULL;
			}
		}
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetPlayerPreference")) {
		dbus_int32_t player;
		const char *id;
		const char *name;
		DBusError error;
		int i;

		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_INT32, &player,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_INVALID) ||
		    player < 1 || player > 4) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Player must be between 1 and 4"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);

		if (id[0]) {
			for (i = 0; i < 4; i++) {
				if (strcmp(prefs[i].id, id) == 0) {
					prefs[i].id[0] = '\0';
					prefs[i].name[0] = '\0';
				}
			}
		}
		copy_text(prefs[player - 1].id, sizeof(prefs[player - 1].id), id);
		copy_text(prefs[player - 1].name, sizeof(prefs[player - 1].name), name);

		if (save_preferences() != 0) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.PersistFailed",
				"Unable to save player preference"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		recompute_effective_players();
		reply_empty(conn, message);
		emit_assignments_changed(conn);
		emit_devices_changed(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetMapping")) {
		const char *id;
		DBusError error;
		DBusMessage *reply;
		DBusMessageIter iter;
		DBusMessageIter array;
		struct source map[CONTROL_COUNT];
		struct source defaults[CONTROL_COUNT];
		size_t i;

		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Controller ID is required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		load_mapping_with_defaults(id, map, defaults);

		reply = dbus_message_new_method_return(message);
		if (!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_message_iter_init_append(reply, &iter);
		if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(sisb)", &array)) {
			dbus_message_unref(reply);
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		for (i = 0; i < CONTROL_COUNT; i++) {
			DBusMessageIter st;
			char text[64];
			const char *control = controls[i].id;
			const char *source = text;
			dbus_int32_t kind = controls[i].kind;
			dbus_bool_t customized = !sources_equal(&map[i], &defaults[i]);

			format_source(&map[i], text, sizeof(text));
			dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &st);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &control);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &kind);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &source);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_BOOLEAN, &customized);
			dbus_message_iter_close_container(&array, &st);
		}
		dbus_message_iter_close_container(&iter, &array);
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetDeadzones")) {
		const char *id;
		DBusError error;
		DBusMessage *reply;
		dbus_int32_t left;
		dbus_int32_t right;
		int left_value;
		int right_value;

		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Controller ID is required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		load_deadzones(id, &left_value, &right_value);
		left = left_value;
		right = right_value;
		reply = dbus_message_new_method_return(message);
		if (reply)
			dbus_message_append_args(reply,
						 DBUS_TYPE_INT32, &left,
						 DBUS_TYPE_INT32, &right,
						 DBUS_TYPE_INVALID);
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetDeadzone")) {
		const char *id;
		const char *stick;
		dbus_int32_t percent;
		DBusError error;

		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_STRING, &stick,
					   DBUS_TYPE_INT32, &percent,
					   DBUS_TYPE_INVALID) ||
		    percent < 0 || percent > 50) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Controller ID, stick and deadzone 0..50 are required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);

		if (save_deadzone_value(id, stick, percent) != 0) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.PersistFailed",
				"Unable to save controller deadzone"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		reload_inputd_mapping();
		reply_empty(conn, message);
		emit_mapping_changed(conn, id);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "BeginRemap")) {
		const char *id;
		const char *control;
		DBusError error;
		int index;

		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_STRING, &control,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Controller ID and control are required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		index = control_index(control);
		if (index < 0 || !device_available(id)) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				index < 0 ? "Unknown mapping control"
					  : "Controller is not connected"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		end_remap();
		copy_text(remap_id, sizeof(remap_id), id);
		remap_control = (size_t)index;
		load_mapping(remap_id, remap_map);
		memset(remap_baseline, 0, sizeof(remap_baseline));
		remap_baseline_ready = false;
		remap_deadline_ms = monotonic_ms() + REMAP_TIMEOUT_MS;
		remap_pending = true;
		/* inputd answers with the current normalized value of every axis
		 * (RAWBASE) and RAWREADY before any further RAW event. */
		inputd_raw_capture(remap_id, true);
		reply_empty(conn, message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "CancelRemap")) {
		end_remap();
		reply_empty(conn, message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "ResetMapping")) {
		const char *id;
		DBusError error;

		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_INVALID) || id[0] == '\0') {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Controller ID is required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		end_remap();
		if (reset_mapping(id) != 0) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.PersistFailed",
				"Unable to reset controller mapping"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		refresh_tester_map();
		reload_inputd_mapping();
		reply_empty(conn, message);
		emit_mapping_changed(conn, id);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "ResetMappings")) {
		size_t i;

		end_remap();
		if (reset_all_mappings() != 0) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.PersistFailed",
				"Unable to reset controller mappings"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		refresh_tester_map();
		reload_inputd_mapping();
		reply_empty(conn, message);
		for (i = 0; i < device_count; i++)
			emit_mapping_changed(conn, devices[i].id);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetTester")) {
		dbus_bool_t enabled;
		const char *id;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_BOOLEAN, &enabled,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Tester state and controller ID are required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		if (tester_enabled && !enabled)
			inputd_raw_capture(tester_id, false);
		tester_enabled = enabled != FALSE;
		copy_text(tester_id, sizeof(tester_id), tester_enabled ? id : "");
		tester_hotkey_pressed = false;
		tester_start_pressed = false;
		if (tester_enabled) {
			refresh_tester_map();
			inputd_raw_capture(tester_id, true);
		}
		reply_empty(conn, message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable object_vtable = {
	.unregister_function = NULL,
	.message_function = handle_message,
};


static void inputd_raw_capture(const char *id, bool enabled)
{
	char command[256];

	if (input_fd < 0)
		return;

	snprintf(command, sizeof(command), "RAW CAPTURE %s %d\n",
		 id && id[0] ? id : "-", enabled ? 1 : 0);
	if (write(input_fd, command, strlen(command)) < 0) {
		/* The input daemon may have restarted; the normal reconnect path
		 * will restore the subscription/capture state on the next cycle. */
	}
}

static int connect_inputd(void)
{
	struct sockaddr_un addr;
	int fd;
	const char subscribe[] = "SUBSCRIBE CONTROLLERS\n";

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", INPUT_SOCKET);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (write(fd, subscribe, sizeof(subscribe) - 1) < 0) {
		close(fd);
		return -1;
	}
	input_used = 0;
	return fd;
}

static bool axis_owned_by_stick(const struct source *map, int code)
{
	size_t i;

	for (i = 0; i < CONTROL_COUNT; i++)
		if (controls[i].kind == CONTROL_AXIS &&
		    map[i].type == SOURCE_ABS && map[i].code == code)
			return true;
	return false;
}

/*
 * Recognises the gesture that binds the control being remapped. Axis values
 * are normalized by inputd to -100..100 around the device centre; the
 * baseline taken when capture started tells a stick (resting at centre) from
 * a trigger (resting at one extreme).
 */
static bool capture_source(unsigned int type, unsigned int code, int value,
			   struct source *out)
{
	int kind = controls[remap_control].kind;
	int base;

	if (type == EV_KEY) {
		if (value != 1 || kind != CONTROL_BUTTON || code > KEY_MAX)
			return false;
		set_source(out, SOURCE_KEY, (int)code, 0);
		return true;
	}
	if (type != EV_ABS || code > ABS_MAX || !remap_baseline_ready)
		return false;

	base = remap_baseline[code];
	if (kind == CONTROL_AXIS) {
		/* The prompt asks for right/down: the positive Linux direction. */
		if (abs(base) >= CAPTURE_REST_PERCENT || abs(value) < CAPTURE_MOVE_PERCENT)
			return false;
		set_source(out, SOURCE_ABS, (int)code, value > 0 ? 1 : -1);
		return true;
	}

	if (base <= -CAPTURE_EXTREME_PERCENT && value >= 0) {
		set_source(out, SOURCE_ABS, (int)code, 2);
		return true;
	}
	if (base >= CAPTURE_EXTREME_PERCENT && value <= 0) {
		set_source(out, SOURCE_ABS, (int)code, -2);
		return true;
	}
	/* A centred stick axis is never taken by a button by accident. */
	if (abs(base) >= CAPTURE_REST_PERCENT || abs(value) < CAPTURE_MOVE_PERCENT ||
	    axis_owned_by_stick(remap_map, (int)code))
		return false;
	set_source(out, SOURCE_ABS, (int)code, value > 0 ? 1 : -1);
	return true;
}

static void commit_remap(DBusConnection *conn, const struct source *src)
{
	char id[MAX_ID];
	char text[64];
	const char *control = controls[remap_control].id;
	int left_deadzone;
	int right_deadzone;

	copy_text(id, sizeof(id), remap_id);
	assign_source(remap_map, remap_control, src);
	end_remap();

	load_deadzones(id, &left_deadzone, &right_deadzone);
	if (save_mapping_file(id, remap_map, left_deadzone, right_deadzone) != 0) {
		emit_remap_cancelled(conn, id, control);
		return;
	}
	format_source(src, text, sizeof(text));
	refresh_tester_map();
	reload_inputd_mapping();
	emit_mapping_changed(conn, id);
	emit_remap_captured(conn, id, control, text);
}

/* Logical value of one control: buttons 0..100 (keys 0/100, axes by travel),
 * stick axes -100..100 with inversion applied. */
static int logical_value(const struct source *src, int kind, int value)
{
	if (src->type == SOURCE_KEY)
		return value ? 100 : 0;
	switch (src->dir) {
	case 1:
		return kind == CONTROL_AXIS ? value : (value > 0 ? value : 0);
	case -1:
		return kind == CONTROL_AXIS ? -value : (value < 0 ? -value : 0);
	case 2:
		return (value + 100) / 2;
	default:
		return (100 - value) / 2;
	}
}

static void tester_event(DBusConnection *conn, const char *id,
			 unsigned int type, unsigned int code, int value)
{
	bool matched = false;
	size_t i;

	if (type == EV_KEY && value == 2)
		return;

	for (i = 0; i < CONTROL_COUNT; i++) {
		const struct source *src = &tester_map[i];
		int logical;

		if (src->code != (int)code ||
		    !((src->type == SOURCE_KEY && type == EV_KEY) ||
		      (src->type == SOURCE_ABS && type == EV_ABS)))
			continue;
		logical = logical_value(src, controls[i].kind, value);
		matched = true;
		if (strcmp(controls[i].id, "quick_menu") == 0)
			tester_hotkey_pressed = logical >= 50;
		else if (strcmp(controls[i].id, "settings") == 0)
			tester_start_pressed = logical >= 50;
		emit_input_event(conn, id, controls[i].id, (int)type, (int)code, logical);
	}
	if (!matched)
		emit_input_event(conn, id, "", (int)type, (int)code, value);

	if (tester_hotkey_pressed && tester_start_pressed) {
		emit_tester_exit_requested(conn, id);
		inputd_raw_capture(tester_id, false);
		tester_enabled = false;
		tester_id[0] = '\0';
		tester_hotkey_pressed = false;
		tester_start_pressed = false;
	}
}

static void handle_raw_line(DBusConnection *conn, const char *line)
{
	char id[MAX_ID];
	unsigned int type;
	unsigned int code;
	int value;

	if (sscanf(line, "RAWBASE %159s %u %d", id, &code, &value) == 3) {
		if (remap_pending && strcmp(id, remap_id) == 0 && code <= ABS_MAX)
			remap_baseline[code] = value;
		return;
	}
	if (sscanf(line, "RAWREADY %159s", id) == 1) {
		if (remap_pending && strcmp(id, remap_id) == 0)
			remap_baseline_ready = true;
		return;
	}
	if (sscanf(line, "RAW %159s %u %u %d", id, &type, &code, &value) != 4)
		return;

	if (remap_pending && strcmp(id, remap_id) == 0) {
		struct source src;

		if (type == EV_KEY && value == 1 &&
		    controls[remap_control].kind == CONTROL_AXIS) {
			/* Keys cannot drive a stick axis: any button cancels. */
			emit_remap_cancelled(conn, remap_id, controls[remap_control].id);
			end_remap();
		} else if (capture_source(type, code, value, &src)) {
			commit_remap(conn, &src);
		}
		return;
	}

	if (tester_enabled &&
	    (tester_id[0] == '\0' || strcmp(tester_id, id) == 0))
		tester_event(conn, id, type, code, value);
}

static void process_inputd(DBusConnection *conn)
{
	char buf[512];
	ssize_t n;
	size_t i;

	n = read(input_fd, buf, sizeof(buf));
	if (n <= 0) {
		close(input_fd);
		input_fd = -1;
		input_used = 0;
		return;
	}

	for (i = 0; i < (size_t)n; i++) {
		char c = buf[i];
		if (c == '\n') {
			input_buf[input_used] = '\0';
			if (strncmp(input_buf, "RAW", 3) == 0)
				handle_raw_line(conn, input_buf);
			input_used = 0;
		} else if (input_used + 1 < sizeof(input_buf)) {
			input_buf[input_used++] = c;
		} else {
			input_used = 0;
		}
	}
}

int main(void)
{
	DBusConnection *conn;
	DBusError error;
	int request;
	int dbus_fd = -1;
	int inotify_fd = -1;
	int input_watch = -1;
	int runtime_watch = -1;
	int user_watch = -1;
	int uevent_fd = -1;

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	refresh_active_user();
	load_preferences();
	rescan_devices(false);

	dbus_error_init(&error);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (!conn) {
		fprintf(stderr, "nuubos-controllersd: system bus: %s\n",
			error.message ? error.message : "unknown error");
		dbus_error_free(&error);
		return 1;
	}

	request = dbus_bus_request_name(conn, SERVICE_NAME,
					DBUS_NAME_FLAG_REPLACE_EXISTING,
					&error);
	if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "nuubos-controllersd: cannot own %s\n", SERVICE_NAME);
		dbus_error_free(&error);
		dbus_connection_unref(conn);
		return 1;
	}

	if (!dbus_connection_register_object_path(conn, OBJECT_PATH, &object_vtable, NULL)) {
		dbus_connection_unref(conn);
		return 1;
	}
	(void)dbus_connection_get_unix_fd(conn, &dbus_fd);

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0) {
		input_watch = inotify_add_watch(inotify_fd, INPUT_DIR,
			IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
		runtime_watch = inotify_add_watch(inotify_fd, "/run/nuubos",
			IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
		user_watch = inotify_add_watch(inotify_fd, USER_DIR,
			IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
			IN_CLOSE_WRITE | IN_ATTRIB);
	}

	input_fd = connect_inputd();
	uevent_fd = make_uevent_socket();

	emit_devices_changed(conn);
	emit_assignments_changed(conn);

	while (running) {
		struct pollfd fds[4];
		int kinds[4];
		nfds_t count = 0;
		int rc;
		int timeout;
		nfds_t i;

		if (dbus_fd >= 0) {
			fds[count].fd = dbus_fd;
			fds[count].events = POLLIN;
			kinds[count++] = 1;
		}
		if (input_fd >= 0) {
			fds[count].fd = input_fd;
			fds[count].events = POLLIN | POLLHUP | POLLERR;
			kinds[count++] = 2;
		}
		if (inotify_fd >= 0) {
			fds[count].fd = inotify_fd;
			fds[count].events = POLLIN;
			kinds[count++] = 3;
		}
		if (uevent_fd >= 0) {
			fds[count].fd = uevent_fd;
			fds[count].events = POLLIN;
			kinds[count++] = 4;
		}

		timeout = -1;
		if (remap_pending) {
			long long remaining = remap_deadline_ms - monotonic_ms();
			timeout = remaining <= 0 ? 0 : (int)remaining;
		}

		rc = poll(fds, count, timeout);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (remap_pending && monotonic_ms() >= remap_deadline_ms) {
			emit_remap_cancelled(conn, remap_id, controls[remap_control].id);
			end_remap();
		}

		for (i = 0; i < count; i++) {
			if (!fds[i].revents)
				continue;
			if (kinds[i] == 1) {
				(void)dbus_connection_read_write_dispatch(conn, 0);
			} else if (kinds[i] == 2) {
				if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
					close(input_fd);
					input_fd = -1;
					input_used = 0;
				} else if (fds[i].revents & POLLIN) {
					process_inputd(conn);
				}
			} else if (kinds[i] == 4) {
				char buf[8192];
				ssize_t n;

				while ((n = recv(uevent_fd, buf, sizeof(buf) - 1, 0)) > 0) {
					buf[n] = '\0';
					handle_battery_uevent(buf, (size_t)n);
				}
			} else if (kinds[i] == 3) {
				char events[4096];
				ssize_t n;
				bool input_changed = false;
				bool runtime_changed = false;
				bool user_changed = false;

				while ((n = read(inotify_fd, events, sizeof(events))) > 0) {
					size_t off = 0;
					while (off < (size_t)n) {
						struct inotify_event *ev =
							(struct inotify_event *)(events + off);
						if (ev->wd == input_watch)
							input_changed = true;
						if (ev->wd == runtime_watch)
							runtime_changed = true;
						if (ev->wd == user_watch)
							user_changed = true;
						off += sizeof(*ev) + ev->len;
					}
				}

				if (user_changed) {
					refresh_active_user();
					load_preferences();
					rescan_devices(false);
					reload_inputd_mapping();
					emit_devices_changed(conn);
					emit_assignments_changed(conn);
				} else if (input_changed) {
					rescan_devices(true);
					emit_devices_changed(conn);
					emit_assignments_changed(conn);
				}

				if (runtime_changed && input_fd < 0)
					input_fd = connect_inputd();
			}
		}

		while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
			;
	}

	if (input_fd >= 0)
		close(input_fd);
	if (uevent_fd >= 0)
		close(uevent_fd);
	if (inotify_fd >= 0) {
		if (input_watch >= 0)
			inotify_rm_watch(inotify_fd, input_watch);
		if (runtime_watch >= 0)
			inotify_rm_watch(inotify_fd, runtime_watch);
		if (user_watch >= 0)
			inotify_rm_watch(inotify_fd, user_watch);
		close(inotify_fd);
	}

	dbus_connection_unregister_object_path(conn, OBJECT_PATH);
	dbus_connection_unref(conn);
	return 0;
}
