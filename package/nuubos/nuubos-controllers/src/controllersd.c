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
#include <unistd.h>

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
};

struct player_pref {
	char id[MAX_ID];
	char name[MAX_NAME];
};

struct mapping_entry {
	const char *action;
	const char *label;
	int default_code;
};

static const struct mapping_entry mapping_defaults[] = {
	/* Stable Settings order: face cluster, D-pad, shoulders/triggers,
	 * stick clicks, then system/navigation buttons. */
	{ "menu_back", "Face South", BTN_SOUTH },
	{ "menu_confirm", "Face East", BTN_EAST },
	{ "face_north", "Face North", BTN_NORTH },
	{ "face_west", "Face West", BTN_WEST },
	{ "menu_up", "D-Pad Up", BTN_DPAD_UP },
	{ "menu_down", "D-Pad Down", BTN_DPAD_DOWN },
	{ "menu_left", "D-Pad Left", BTN_DPAD_LEFT },
	{ "menu_right", "D-Pad Right", BTN_DPAD_RIGHT },
	{ "l1", "L1", BTN_TL },
	{ "r1", "R1", BTN_TR },
	{ "l2", "L2", BTN_TL2 },
	{ "r2", "R2", BTN_TR2 },
	{ "l3", "L3", BTN_THUMBL },
	{ "r3", "R3", BTN_THUMBR },
	{ "settings", "Start", BTN_START },
	{ "select", "Select", BTN_SELECT },
	{ "quick_menu", "Hotkey", BTN_MODE },
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
static bool remap_pending;
static char remap_id[MAX_ID];
static char remap_action[64];
static int tester_codes[sizeof(mapping_defaults) / sizeof(mapping_defaults[0])];
static int tester_left_deadzone = 20;
static int tester_right_deadzone = 20;
static bool tester_hotkey_pressed;
static bool tester_start_pressed;

static void inputd_raw_capture(const char *id, bool enabled);

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
	"<arg name='mapping' type='a(ssi)' direction='out'/>"
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
	"<arg name='action' type='s'/>"
	"<arg name='event_type' type='i'/>"
	"<arg name='code' type='i'/>"
	"<arg name='value' type='i'/>"
	"</signal>"
	"<signal name='RemapCaptured'>"
	"<arg name='controller_id' type='s'/>"
	"<arg name='action' type='s'/>"
	"<arg name='code' type='i'/>"
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

static void rescan_devices(void)
{
	glob_t g;
	size_t i;

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
		device_count++;
		close(fd);
	}

	globfree(&g);
	recompute_effective_players();
}

static bool device_available(const char *id)
{
	return id && id[0] && find_device(id) >= 0;
}

static void load_mapping(const char *id, int *codes)
{
	char path[512];
	FILE *fp;
	char line[256];
	size_t i;

	for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++)
		codes[i] = mapping_defaults[i].default_code;

	mapping_path(id, path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char action[64];
		int code;
		if (sscanf(line, "%63[^=]=%d", action, &code) != 2)
			continue;
		for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++) {
			if (strcmp(action, mapping_defaults[i].action) == 0 &&
			    code >= 0 && code <= KEY_MAX) {
				codes[i] = code;
				break;
			}
		}
	}
	fclose(fp);
}

static void load_deadzones(const char *id, int *left, int *right)
{
	char path[512];
	FILE *fp;
	char line[256];

	*left = 20;
	*right = 20;
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

static int save_mapping_file(const char *id, const int *codes,
			     int left_deadzone, int right_deadzone)
{
	char path[512];
	char tmp[560];
	FILE *fp;
	size_t i;

	if (ensure_user_dirs() != 0)
		return -1;

	mapping_path(id, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;

	for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++) {
		if (fprintf(fp, "%s=%d\n", mapping_defaults[i].action, codes[i]) < 0) {
			fclose(fp);
			unlink(tmp);
			return -1;
		}
	}
	if (fprintf(fp, "LEFT_DEADZONE=%d\nRIGHT_DEADZONE=%d\n",
		    left_deadzone, right_deadzone) < 0) {
		fclose(fp);
		unlink(tmp);
		return -1;
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

static int save_mapping_value(const char *id, const char *action, int code)
{
	int codes[sizeof(mapping_defaults) / sizeof(mapping_defaults[0])];
	int left_deadzone;
	int right_deadzone;
	size_t i;
	bool found = false;

	if (ensure_user_dirs() != 0)
		return -1;

	for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++)
		if (strcmp(mapping_defaults[i].action, action) == 0)
			found = true;
	if (!found)
		return -1;

	load_mapping(id, codes);
	load_deadzones(id, &left_deadzone, &right_deadzone);
	for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++)
		if (strcmp(mapping_defaults[i].action, action) == 0)
			codes[i] = code;

	return save_mapping_file(id, codes, left_deadzone, right_deadzone);
}

static int save_deadzone_value(const char *id, const char *stick, int percent)
{
	int codes[sizeof(mapping_defaults) / sizeof(mapping_defaults[0])];
	int left_deadzone;
	int right_deadzone;

	if (percent < 0 || percent > 50)
		return -1;
	load_mapping(id, codes);
	load_deadzones(id, &left_deadzone, &right_deadzone);

	if (strcmp(stick, "left") == 0)
		left_deadzone = percent;
	else if (strcmp(stick, "right") == 0)
		right_deadzone = percent;
	else
		return -1;

	return save_mapping_file(id, codes, left_deadzone, right_deadzone);
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

static const char *mapped_action_for_code(int code)
{
	size_t i;
	for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++)
		if (tester_codes[i] == code)
			return mapping_defaults[i].label;
	return "";
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

static void emit_remap_captured(DBusConnection *conn,
				const char *id, const char *action, int code)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "RemapCaptured");
	dbus_int32_t v = code;
	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_STRING, &id,
				 DBUS_TYPE_STRING, &action,
				 DBUS_TYPE_INT32, &v,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static void emit_input_event(DBusConnection *conn, const char *id,
			     const char *action, int type, int code, int value)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "InputEvent");
	dbus_int32_t t = type;
	dbus_int32_t c = code;
	dbus_int32_t v = value;
	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_STRING, &id,
				 DBUS_TYPE_STRING, &action,
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
		int codes[sizeof(mapping_defaults) / sizeof(mapping_defaults[0])];
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
		load_mapping(id, codes);

		reply = dbus_message_new_method_return(message);
		if (!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		dbus_message_iter_init_append(reply, &iter);
		if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ssi)", &array)) {
			dbus_message_unref(reply);
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}
		for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++) {
			DBusMessageIter st;
			const char *action = mapping_defaults[i].action;
			const char *label = mapping_defaults[i].label;
			dbus_int32_t code = codes[i];
			dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &st);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &action);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &label);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &code);
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
		const char *action;
		DBusError error;
		size_t i;
		bool valid = false;

		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &id,
					   DBUS_TYPE_STRING, &action,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Controller ID and action are required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++)
			if (strcmp(mapping_defaults[i].action, action) == 0)
				valid = true;
		if (!valid) {
			send_reply(conn, new_error(message,
				"org.nuubOS.Controllers.Error.InvalidArgument",
				"Unknown mapping action"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		copy_text(remap_id, sizeof(remap_id), id);
		copy_text(remap_action, sizeof(remap_action), action);
		remap_pending = true;
		inputd_raw_capture(remap_id, true);
		reply_empty(conn, message);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "CancelRemap")) {
		if (remap_pending)
			inputd_raw_capture(remap_id, false);
		remap_pending = false;
		remap_id[0] = '\0';
		remap_action[0] = '\0';
		reply_empty(conn, message);
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
			load_mapping(tester_id, tester_codes);
			load_deadzones(tester_id, &tester_left_deadzone, &tester_right_deadzone);
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

static void handle_raw_line(DBusConnection *conn, const char *line)
{
	char id[MAX_ID];
	unsigned int type;
	unsigned int code;
	int value;

	if (sscanf(line, "RAW %159s %u %u %d", id, &type, &code, &value) != 4)
		return;

	if (remap_pending &&
	    strcmp(id, remap_id) == 0 &&
	    type == EV_KEY && value == 1) {
		if (save_mapping_value(id, remap_action, (int)code) == 0) {
			emit_mapping_changed(conn, id);
			emit_remap_captured(conn, id, remap_action, (int)code);
			reload_inputd_mapping();
			if (tester_enabled && strcmp(tester_id, id) == 0)
				load_mapping(tester_id, tester_codes);
		}
		inputd_raw_capture(remap_id, false);
		remap_pending = false;
		remap_id[0] = '\0';
		remap_action[0] = '\0';
	}

	if (tester_enabled &&
	    (tester_id[0] == '\0' || strcmp(tester_id, id) == 0)) {
		const char *action = "";
		if (type == EV_KEY)
			action = mapped_action_for_code((int)code);

		if (type == EV_KEY) {
			size_t i;
			for (i = 0; i < sizeof(mapping_defaults) / sizeof(mapping_defaults[0]); i++) {
				if (strcmp(mapping_defaults[i].action, "quick_menu") == 0 &&
				    tester_codes[i] == (int)code)
					tester_hotkey_pressed = value != 0;
				if (strcmp(mapping_defaults[i].action, "settings") == 0 &&
				    tester_codes[i] == (int)code)
					tester_start_pressed = value != 0;
			}
		}

		emit_input_event(conn, id, action, (int)type, (int)code, value);

		if (tester_hotkey_pressed && tester_start_pressed) {
			emit_tester_exit_requested(conn, id);
			inputd_raw_capture(tester_id, false);
			tester_enabled = false;
			tester_id[0] = '\0';
			tester_hotkey_pressed = false;
			tester_start_pressed = false;
		}
	}
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
			if (strncmp(input_buf, "RAW ", 4) == 0)
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

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	refresh_active_user();
	load_preferences();
	rescan_devices();

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

	emit_devices_changed(conn);
	emit_assignments_changed(conn);

	while (running) {
		struct pollfd fds[3];
		int kinds[3];
		nfds_t count = 0;
		int rc;
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

		rc = poll(fds, count, -1);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
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
					rescan_devices();
					reload_inputd_mapping();
					emit_devices_changed(conn);
					emit_assignments_changed(conn);
				} else if (input_changed) {
					rescan_devices();
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
