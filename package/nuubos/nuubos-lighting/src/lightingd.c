/* SPDX-License-Identifier: MIT */

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SERVICE_NAME "org.nuubOS.Lighting"
#define OBJECT_PATH "/org/nuubOS/Lighting"
#define INTERFACE_NAME "org.nuubOS.Lighting1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"

#define RGB_SOCKET "/run/nuubos/rgbd.sock"
#define STATUS_SOCKET "/run/nuubos/statusd.sock"
#define STATUS_STATE "/run/nuubos/statusd.state"
#define USER_DIR "/run/nuubos/user"
#define ACTIVE_USER USER_DIR "/active"

enum effect_kind {
	EFFECT_NONE = 0,
	EFFECT_BOOT,
	EFFECT_LOW_BATTERY,
	EFFECT_SYSTEM,
};

struct lighting_state {
	bool supported;
	char mode[24];
	int brightness;
	int r;
	int g;
	int b;
	bool system_effects;
	char active_effect[32];
	int led_count;
	enum effect_kind effect;
	int effect_step;
	int effect_steps_total;
	uint8_t effect_r;
	uint8_t effect_g;
	uint8_t effect_b;
	int baseline_step;
	int screen_r;
	int screen_g;
	int screen_b;
	bool screen_sample_seen;
};

static volatile sig_atomic_t running = 1;
static char active_user[128] = "default";
static struct lighting_state state = {
	.supported = false,
	.mode = "off",
	.brightness = 50,
	.r = 58,
	.g = 134,
	.b = 255,
	.system_effects = true,
	.active_effect = "",
	.led_count = 0,
	.baseline_step = 0,
	.screen_r = 58,
	.screen_g = 134,
	.screen_b = 255,
	.screen_sample_seen = false,
};
static int status_fd = -1;
static char status_buf[512];
static size_t status_used;
static int last_battery = -1;
static bool low_battery_latched;

static const char introspection_xml[] =
	"<node>"
	"<interface name='org.nuubOS.Lighting1'>"
	"<method name='GetState'>"
	"<arg name='supported' type='b' direction='out'/>"
	"<arg name='mode' type='s' direction='out'/>"
	"<arg name='brightness' type='i' direction='out'/>"
	"<arg name='red' type='i' direction='out'/>"
	"<arg name='green' type='i' direction='out'/>"
	"<arg name='blue' type='i' direction='out'/>"
	"<arg name='system_effects' type='b' direction='out'/>"
	"<arg name='active_effect' type='s' direction='out'/>"
	"</method>"
	"<method name='SetMode'><arg name='mode' type='s' direction='in'/></method>"
	"<method name='SetColor'>"
	"<arg name='red' type='i' direction='in'/>"
	"<arg name='green' type='i' direction='in'/>"
	"<arg name='blue' type='i' direction='in'/>"
	"</method>"
	"<method name='SetBrightness'><arg name='brightness' type='i' direction='in'/></method>"
	"<method name='SetSystemEffects'><arg name='enabled' type='b' direction='in'/></method>"
	"<method name='SetScreenColor'>"
	"<arg name='red' type='i' direction='in'/>"
	"<arg name='green' type='i' direction='in'/>"
	"<arg name='blue' type='i' direction='in'/>"
	"</method>"
	"<method name='StopEffect'/>"
	"<method name='PlayEffect'>"
	"<arg name='effect' type='s' direction='in'/>"
	"<arg name='context' type='s' direction='in'/>"
	"</method>"
	"<signal name='StateChanged'>"
	"<arg name='supported' type='b'/>"
	"<arg name='mode' type='s'/>"
	"<arg name='brightness' type='i'/>"
	"<arg name='red' type='i'/>"
	"<arg name='green' type='i'/>"
	"<arg name='blue' type='i'/>"
	"<arg name='system_effects' type='b'/>"
	"<arg name='active_effect' type='s'/>"
	"</signal>"
	"</interface>"
	"</node>";

static void signal_handler(int sig)
{
	(void)sig;
	running = 0;
}

static void trim(char *text)
{
	size_t len = strlen(text);
	while (len > 0 &&
	       (text[len - 1] == '\n' || text[len - 1] == '\r' ||
		text[len - 1] == ' ' || text[len - 1] == '\t'))
		text[--len] = '\0';
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
		if (!value[0])
			snprintf(value, sizeof(value), "default");
	}
	snprintf(active_user, sizeof(active_user), "%s", value);
}

static int mkdir_if_needed(const char *path)
{
	if (mkdir(path, 0755) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static void settings_path(char *out, size_t size)
{
	snprintf(out, size, "/state/users/%s/lighting.conf", active_user);
}

static void load_settings(void)
{
	char path[384];
	FILE *fp;
	char line[256];

	snprintf(state.mode, sizeof(state.mode), "off");
	state.brightness = 50;
	state.r = 58;
	state.g = 134;
	state.b = 255;
	state.system_effects = true;

	settings_path(path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char value[sizeof(state.mode)];
		int n;
		if (sscanf(line, "MODE=%23s", value) == 1)
			snprintf(state.mode, sizeof(state.mode), "%s", value);
		else if (sscanf(line, "BRIGHTNESS=%d", &n) == 1)
			state.brightness = n;
		else if (sscanf(line, "RED=%d", &n) == 1)
			state.r = n;
		else if (sscanf(line, "GREEN=%d", &n) == 1)
			state.g = n;
		else if (sscanf(line, "BLUE=%d", &n) == 1)
			state.b = n;
		else if (sscanf(line, "SYSTEM_EFFECTS=%d", &n) == 1)
			state.system_effects = n != 0;
	}
	fclose(fp);

	if (state.brightness < 0) state.brightness = 0;
	if (state.brightness > 100) state.brightness = 100;
	if (state.r < 0)
		state.r = 0;
	if (state.r > 255)
		state.r = 255;
	if (state.g < 0)
		state.g = 0;
	if (state.g > 255)
		state.g = 255;
	if (state.b < 0)
		state.b = 0;
	if (state.b > 255)
		state.b = 255;
}

static int save_settings(void)
{
	char dir[256];
	char path[384];
	char tmp[420];
	FILE *fp;

	if (mkdir_if_needed("/state/users") != 0)
		return -1;
	snprintf(dir, sizeof(dir), "/state/users/%s", active_user);
	if (mkdir_if_needed(dir) != 0)
		return -1;

	settings_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());

	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	if (fprintf(fp,
		    "MODE=%s\nBRIGHTNESS=%d\nRED=%d\nGREEN=%d\nBLUE=%d\nSYSTEM_EFFECTS=%d\n",
		    state.mode, state.brightness, state.r, state.g, state.b,
		    state.system_effects ? 1 : 0) < 0 ||
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

static int rgb_command(const char *command, char *reply, size_t reply_size)
{
	struct sockaddr_un addr;
	int fd;
	ssize_t n;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", RGB_SOCKET);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (write(fd, command, strlen(command)) != (ssize_t)strlen(command)) {
		close(fd);
		return -1;
	}
	n = read(fd, reply, reply_size - 1);
	close(fd);
	if (n < 0)
		return -1;
	reply[n] = '\0';
	return strncmp(reply, "ERR ", 4) == 0 ? -1 : 0;
}

static void detect_rgb(void)
{
	char reply[1024];
	char *p;

	state.supported = false;
	state.led_count = 0;
	if (rgb_command("STATUS", reply, sizeof(reply)) != 0)
		return;
	if (strstr(reply, "supported=1") == NULL)
		return;
	state.supported = true;
	p = strstr(reply, "leds=");
	if (p)
		state.led_count = atoi(p + 5);
	if (state.led_count < 1)
		state.led_count = 1;
	if (state.led_count > 32)
		state.led_count = 32;
}

static int apply_baseline(void)
{
	char command[128];
	char reply[1024];

	if (!state.supported)
		return 0;

	if (strcmp(state.mode, "off") == 0)
		return rgb_command("OFF", reply, sizeof(reply));

	snprintf(command, sizeof(command), "BRIGHTNESS %d", state.brightness);
	if (rgb_command(command, reply, sizeof(reply)) != 0)
		return -1;

	snprintf(command, sizeof(command), "COLOR %d %d %d", state.r, state.g, state.b);
	if (rgb_command(command, reply, sizeof(reply)) != 0)
		return -1;

	if (strcmp(state.mode, "static") == 0)
		return rgb_command("MODE static", reply, sizeof(reply));
	if (strcmp(state.mode, "breathe") == 0)
		return rgb_command("MODE breathe", reply, sizeof(reply));
	if (strcmp(state.mode, "rainbow") == 0)
		return rgb_command("MODE rainbow", reply, sizeof(reply));

	/* Frame-based modes are rendered by the product service timer. */
	return 0;
}

static int scale_channel(int value, int percent)
{
	int scaled = (value * percent) / 100;
	if (scaled < 0) scaled = 0;
	if (scaled > 255) scaled = 255;
	return scaled;
}

/* Modes rendered frame by frame here; static/breathe/rainbow run in rgbd. */
static bool frame_mode_active(void)
{
	return strcmp(state.mode, "pulse") == 0 ||
	       strcmp(state.mode, "chase") == 0 ||
	       strcmp(state.mode, "wave") == 0 ||
	       strcmp(state.mode, "sparkle") == 0 ||
	       strcmp(state.mode, "screen") == 0;
}

static int send_baseline_frame(void)
{
	char command[4096];
	char reply[1024];
	size_t used = 0;
	int i;
	int step = state.baseline_step++;
	int brightness = state.brightness;

	if (!state.supported || state.led_count <= 0)
		return 0;

	if (!frame_mode_active())
		return 0;

	used += (size_t)snprintf(command + used, sizeof(command) - used, "FRAME");

	for (i = 0; i < state.led_count; i++) {
		int r = state.r;
		int g = state.g;
		int b = state.b;
		int level = 100;

		if (strcmp(state.mode, "pulse") == 0) {
			int phase = step % 20;
			level = phase <= 10 ? 20 + phase * 8 : 20 + (20 - phase) * 8;
		} else if (strcmp(state.mode, "chase") == 0) {
			int active = step % state.led_count;
			level = i == active ? 100 :
				(i == (active + state.led_count - 1) % state.led_count ? 25 : 4);
		} else if (strcmp(state.mode, "wave") == 0) {
			int phase = (step + i * 4) % 24;
			level = phase <= 12 ? 12 + phase * 7 : 12 + (24 - phase) * 7;
		} else if (strcmp(state.mode, "sparkle") == 0) {
			unsigned int hash =
				(unsigned int)(step * 1103515245u + (i + 1) * 2654435761u);
			level = (hash % 7u) == 0u ? 100 : 6;
		} else if (strcmp(state.mode, "screen") == 0) {
			r = state.screen_r;
			g = state.screen_g;
			b = state.screen_b;
			level = state.screen_sample_seen ? 100 : 35;
		}

		level = (level * brightness) / 100;
		r = scale_channel(r, level);
		g = scale_channel(g, level);
		b = scale_channel(b, level);

		if (used + 20 >= sizeof(command))
			return -1;
		used += (size_t)snprintf(command + used, sizeof(command) - used,
					" %d %d %d", r, g, b);
	}

	return rgb_command(command, reply, sizeof(reply));
}

static void system_color(const char *context, uint8_t *r, uint8_t *g, uint8_t *b)
{
	unsigned int hash = 2166136261u;
	const unsigned char *p = (const unsigned char *)context;
	static const uint8_t palette[][3] = {
		{ 58, 134, 255 }, { 171, 71, 188 }, { 76, 175, 80 },
		{ 255, 152, 0 }, { 0, 188, 212 }, { 244, 67, 54 },
	};

	while (p && *p) {
		hash ^= *p++;
		hash *= 16777619u;
	}
	hash %= (unsigned int)(sizeof(palette) / sizeof(palette[0]));
	*r = palette[hash][0];
	*g = palette[hash][1];
	*b = palette[hash][2];
}

static int send_effect_frame(void)
{
	char command[4096];
	char reply[1024];
	size_t used = 0;
	int i;
	bool on = true;
	int active = 0;

	if (!state.supported || state.led_count <= 0)
		return 0;

	used += (size_t)snprintf(command + used, sizeof(command) - used, "FRAME");

	if (state.effect == EFFECT_BOOT || state.effect == EFFECT_SYSTEM)
		active = state.effect_step % state.led_count;
	else if (state.effect == EFFECT_LOW_BATTERY)
		on = ((state.effect_step / 3) % 2) == 0;

	for (i = 0; i < state.led_count; i++) {
		int r = 0, g = 0, b = 0;

		if (state.effect == EFFECT_LOW_BATTERY) {
			if (on) r = 255;
		} else if (i == active) {
			r = state.effect_r;
			g = state.effect_g;
			b = state.effect_b;
		} else if (state.effect == EFFECT_SYSTEM && i == (active + state.led_count - 1) % state.led_count) {
			r = state.effect_r / 4;
			g = state.effect_g / 4;
			b = state.effect_b / 4;
		}

		if (used + 16 >= sizeof(command))
			return -1;
		used += (size_t)snprintf(command + used, sizeof(command) - used,
					" %d %d %d", r, g, b);
	}
	return rgb_command(command, reply, sizeof(reply));
}

static void start_effect(enum effect_kind kind, const char *name, const char *context)
{
	state.effect = kind;
	state.effect_step = 0;
	snprintf(state.active_effect, sizeof(state.active_effect), "%s", name ? name : "");

	if (kind == EFFECT_BOOT) {
		state.effect_steps_total = 20;
		state.effect_r = 58;
		state.effect_g = 134;
		state.effect_b = 255;
	} else if (kind == EFFECT_LOW_BATTERY) {
		state.effect_steps_total = 30;
		state.effect_r = 255;
		state.effect_g = 0;
		state.effect_b = 0;
	} else if (kind == EFFECT_SYSTEM) {
		state.effect_steps_total = 15;
		system_color(context ? context : "", &state.effect_r, &state.effect_g, &state.effect_b);
	} else {
		state.effect_steps_total = 0;
	}
	(void)send_effect_frame();
}

static void stop_effect(void)
{
	state.effect = EFFECT_NONE;
	state.effect_step = 0;
	state.effect_steps_total = 0;
	state.active_effect[0] = '\0';
	(void)apply_baseline();
}

static int read_battery_percent(void)
{
	FILE *fp = fopen(STATUS_STATE, "r");
	char line[128];
	int value = -1;
	if (!fp)
		return -1;
	while (fgets(line, sizeof(line), fp))
		if (sscanf(line, "BATTERY_PERCENT=%d", &value) == 1)
			break;
	fclose(fp);
	return value;
}

static int connect_status(void)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", STATUS_SOCKET);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	status_used = 0;
	return fd;
}

static void process_battery_change(void)
{
	int percent = read_battery_percent();

	if (percent < 0)
		return;
	if (percent > 20)
		low_battery_latched = false;
	if (state.system_effects && percent <= 15 && !low_battery_latched) {
		start_effect(EFFECT_LOW_BATTERY, "low-battery", "");
		low_battery_latched = true;
	}
	last_battery = percent;
}

static void process_status_stream(void)
{
	char buf[256];
	ssize_t n;
	size_t i;

	n = read(status_fd, buf, sizeof(buf));
	if (n <= 0) {
		close(status_fd);
		status_fd = -1;
		status_used = 0;
		return;
	}

	for (i = 0; i < (size_t)n; i++) {
		char c = buf[i];
		if (c == '\n') {
			status_buf[status_used] = '\0';
			if (strcmp(status_buf, "changed") == 0)
				process_battery_change();
			status_used = 0;
		} else if (status_used + 1 < sizeof(status_buf)) {
			status_buf[status_used++] = c;
		} else {
			status_used = 0;
		}
	}
}

static void send_reply(DBusConnection *conn, DBusMessage *reply)
{
	if (!reply)
		return;
	dbus_connection_send(conn, reply, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(reply);
}

static void emit_state(DBusConnection *conn)
{
	DBusMessage *signal = dbus_message_new_signal(OBJECT_PATH, INTERFACE_NAME, "StateChanged");
	dbus_bool_t supported = state.supported;
	const char *mode = state.mode;
	dbus_int32_t brightness = state.brightness;
	dbus_int32_t r = state.r, g = state.g, b = state.b;
	dbus_bool_t effects = state.system_effects;
	const char *active = state.active_effect;

	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_BOOLEAN, &supported,
				 DBUS_TYPE_STRING, &mode,
				 DBUS_TYPE_INT32, &brightness,
				 DBUS_TYPE_INT32, &r,
				 DBUS_TYPE_INT32, &g,
				 DBUS_TYPE_INT32, &b,
				 DBUS_TYPE_BOOLEAN, &effects,
				 DBUS_TYPE_STRING, &active,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static int signal_array_count(DBusMessage *message)
{
	DBusMessageIter iter;
	DBusMessageIter array;
	int count = 0;

	if (!dbus_message_iter_init(message, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return 0;

	dbus_message_iter_recurse(&iter, &array);
	while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID) {
		count++;
		dbus_message_iter_next(&array);
	}
	return count;
}

static DBusHandlerResult product_signal_filter(DBusConnection *conn,
					       DBusMessage *message,
					       void *data)
{
	static int last_controller_count = -1;
	(void)data;

	if (dbus_message_is_signal(message,
				   "org.nuubOS.Controllers1",
				   "DevicesChanged")) {
		int count = signal_array_count(message);
		if (last_controller_count >= 0 &&
		    count > last_controller_count &&
		    state.system_effects &&
		    state.effect != EFFECT_LOW_BATTERY) {
			start_effect(EFFECT_SYSTEM, "controller-connected", "");
			state.effect_r = 76;
			state.effect_g = 175;
			state.effect_b = 80;
			(void)send_effect_frame();
			emit_state(conn);
		}
		last_controller_count = count;
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_signal(message,
				   "org.nuubOS.Wifi1",
				   "ScanStateChanged")) {
		dbus_bool_t scanning = FALSE;
		DBusError error;
		dbus_error_init(&error);
		if (dbus_message_get_args(message, &error,
					  DBUS_TYPE_BOOLEAN, &scanning,
					  DBUS_TYPE_INVALID) &&
		    scanning &&
		    state.system_effects &&
		    state.effect != EFFECT_LOW_BATTERY) {
			start_effect(EFFECT_SYSTEM, "wifi-search", "");
			state.effect_r = 0;
			state.effect_g = 188;
			state.effect_b = 212;
			(void)send_effect_frame();
			emit_state(conn);
		}
		dbus_error_free(&error);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool valid_mode(const char *mode)
{
	return strcmp(mode, "off") == 0 ||
	       strcmp(mode, "static") == 0 ||
	       strcmp(mode, "breathe") == 0 ||
	       strcmp(mode, "rainbow") == 0 ||
	       strcmp(mode, "pulse") == 0 ||
	       strcmp(mode, "chase") == 0 ||
	       strcmp(mode, "wave") == 0 ||
	       strcmp(mode, "sparkle") == 0 ||
	       strcmp(mode, "screen") == 0;
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

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "GetState")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		dbus_bool_t supported = state.supported;
		const char *mode = state.mode;
		dbus_int32_t brightness = state.brightness;
		dbus_int32_t r = state.r, g = state.g, b = state.b;
		dbus_bool_t effects = state.system_effects;
		const char *active = state.active_effect;
		if (reply)
			dbus_message_append_args(reply,
				DBUS_TYPE_BOOLEAN, &supported,
				DBUS_TYPE_STRING, &mode,
				DBUS_TYPE_INT32, &brightness,
				DBUS_TYPE_INT32, &r,
				DBUS_TYPE_INT32, &g,
				DBUS_TYPE_INT32, &b,
				DBUS_TYPE_BOOLEAN, &effects,
				DBUS_TYPE_STRING, &active,
				DBUS_TYPE_INVALID);
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetMode")) {
		const char *mode;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &mode,
					   DBUS_TYPE_INVALID) ||
		    !valid_mode(mode)) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Lighting.Error.InvalidArgument",
				"Unsupported lighting mode"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		snprintf(state.mode, sizeof(state.mode), "%s", mode);
		state.baseline_step = 0;
		stop_effect();
		if (state.effect == EFFECT_NONE)
			(void)send_baseline_frame();
		(void)save_settings();
		send_reply(conn, dbus_message_new_method_return(message));
		emit_state(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetColor")) {
		dbus_int32_t r, g, b;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_INT32, &r,
					   DBUS_TYPE_INT32, &g,
					   DBUS_TYPE_INT32, &b,
					   DBUS_TYPE_INVALID) ||
		    r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Lighting.Error.InvalidArgument",
				"RGB values must be 0..255"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		state.r = r; state.g = g; state.b = b;
		if (strcmp(state.mode, "off") == 0)
			snprintf(state.mode, sizeof(state.mode), "static");
		state.baseline_step = 0;
		stop_effect();
		if (state.effect == EFFECT_NONE)
			(void)send_baseline_frame();
		(void)save_settings();
		send_reply(conn, dbus_message_new_method_return(message));
		emit_state(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetBrightness")) {
		dbus_int32_t value;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_INT32, &value,
					   DBUS_TYPE_INVALID) ||
		    value < 0 || value > 100) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Lighting.Error.InvalidArgument",
				"Brightness must be 0..100"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		state.brightness = value;
		if (state.effect == EFFECT_NONE) {
			(void)apply_baseline();
			(void)send_baseline_frame();
		}
		(void)save_settings();
		send_reply(conn, dbus_message_new_method_return(message));
		emit_state(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetSystemEffects")) {
		dbus_bool_t enabled;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_BOOLEAN, &enabled,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Lighting.Error.InvalidArgument",
				"Enabled state is required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		state.system_effects = enabled != FALSE;
		(void)save_settings();
		send_reply(conn, dbus_message_new_method_return(message));
		emit_state(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetScreenColor")) {
		dbus_int32_t r, g, b;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_INT32, &r,
					   DBUS_TYPE_INT32, &g,
					   DBUS_TYPE_INT32, &b,
					   DBUS_TYPE_INVALID) ||
		    r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Lighting.Error.InvalidArgument",
				"Screen color values must be 0..255"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		state.screen_r = r;
		state.screen_g = g;
		state.screen_b = b;
		state.screen_sample_seen = true;
		if (state.effect == EFFECT_NONE && strcmp(state.mode, "screen") == 0)
			(void)send_baseline_frame();
		send_reply(conn, dbus_message_new_method_return(message));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "StopEffect")) {
		stop_effect();
		send_reply(conn, dbus_message_new_method_return(message));
		emit_state(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "PlayEffect")) {
		const char *effect;
		const char *context;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &effect,
					   DBUS_TYPE_STRING, &context,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Lighting.Error.InvalidArgument",
				"Effect and context are required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);

		if (strcmp(effect, "boot") == 0)
			start_effect(EFFECT_BOOT, "boot", context);
		else if (strcmp(effect, "low-battery") == 0)
			start_effect(EFFECT_LOW_BATTERY, "low-battery", context);
		else if (strcmp(effect, "controller-connected") == 0) {
			start_effect(EFFECT_SYSTEM, "controller-connected", context);
			state.effect_r = 76; state.effect_g = 175; state.effect_b = 80;
			(void)send_effect_frame();
		} else if (strcmp(effect, "wifi-search") == 0) {
			start_effect(EFFECT_SYSTEM, "wifi-search", context);
			state.effect_r = 0; state.effect_g = 188; state.effect_b = 212;
			(void)send_effect_frame();
		} else if (strcmp(effect, "system") == 0)
			start_effect(EFFECT_SYSTEM, "system", context);
		else {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Lighting.Error.InvalidArgument",
				"Unknown effect"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		send_reply(conn, dbus_message_new_method_return(message));
		emit_state(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable object_vtable = {
	.unregister_function = NULL,
	.message_function = handle_message,
};

/* The frame timer runs only while something animates; idle costs no wakeups. */
static void sync_frame_timer(int fd, bool *armed)
{
	struct itimerspec spec;
	bool want = state.supported && state.led_count > 0 &&
		    (state.effect != EFFECT_NONE || frame_mode_active());

	if (fd < 0 || want == *armed)
		return;
	memset(&spec, 0, sizeof(spec));
	if (want) {
		spec.it_value.tv_nsec = 100000000;
		spec.it_interval.tv_nsec = 100000000;
	}
	if (timerfd_settime(fd, 0, &spec, NULL) == 0)
		*armed = want;
}

int main(void)
{
	DBusConnection *conn;
	DBusError error;
	int request;
	int dbus_fd = -1;
	int inotify_fd = -1;
	int runtime_watch = -1;
	int user_watch = -1;
	int timer_fd = -1;
	bool timer_armed = false;

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	refresh_active_user();
	load_settings();
	detect_rgb();
	(void)apply_baseline();
	(void)send_baseline_frame();

	dbus_error_init(&error);
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (!conn)
		return 1;

	request = dbus_bus_request_name(conn, SERVICE_NAME,
					DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
	if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		dbus_connection_unref(conn);
		return 1;
	}
	if (!dbus_connection_register_object_path(conn, OBJECT_PATH, &object_vtable, NULL)) {
		dbus_connection_unref(conn);
		return 1;
	}
	(void)dbus_connection_get_unix_fd(conn, &dbus_fd);
	dbus_bus_add_match(conn,
		"type='signal',interface='org.nuubOS.Controllers1',member='DevicesChanged'",
		&error);
	if (dbus_error_is_set(&error))
		dbus_error_free(&error);
	dbus_bus_add_match(conn,
		"type='signal',interface='org.nuubOS.Wifi1',member='ScanStateChanged'",
		&error);
	if (dbus_error_is_set(&error))
		dbus_error_free(&error);
	if (!dbus_connection_add_filter(conn, product_signal_filter, NULL, NULL)) {
		dbus_connection_unregister_object_path(conn, OBJECT_PATH);
		dbus_connection_unref(conn);
		return 1;
	}

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0) {
		runtime_watch = inotify_add_watch(inotify_fd, "/run/nuubos",
			IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
		user_watch = inotify_add_watch(inotify_fd, USER_DIR,
			IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
			IN_CLOSE_WRITE | IN_ATTRIB);
	}
	timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	status_fd = connect_status();
	process_battery_change();

	if (state.system_effects && state.supported)
		start_effect(EFFECT_BOOT, "boot", "");

	while (running) {
		struct pollfd fds[4];
		int kinds[4];
		nfds_t count = 0;
		int rc;
		nfds_t i;

		sync_frame_timer(timer_fd, &timer_armed);
		if (dbus_fd >= 0) {
			fds[count].fd = dbus_fd; fds[count].events = POLLIN; kinds[count++] = 1;
		}
		if (inotify_fd >= 0) {
			fds[count].fd = inotify_fd; fds[count].events = POLLIN; kinds[count++] = 2;
		}
		if (timer_fd >= 0) {
			fds[count].fd = timer_fd; fds[count].events = POLLIN; kinds[count++] = 3;
		}
		if (status_fd >= 0) {
			fds[count].fd = status_fd; fds[count].events = POLLIN | POLLHUP | POLLERR; kinds[count++] = 4;
		}

		rc = poll(fds, count, -1);
		if (rc < 0) {
			if (errno == EINTR) continue;
			break;
		}

		for (i = 0; i < count; i++) {
			if (!fds[i].revents) continue;
			if (kinds[i] == 1) {
				(void)dbus_connection_read_write_dispatch(conn, 0);
			} else if (kinds[i] == 2) {
				char buf[4096];
				ssize_t n;
				bool runtime_changed = false;
				bool user_changed = false;
				while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
					size_t off = 0;
					while (off < (size_t)n) {
						struct inotify_event *ev = (struct inotify_event *)(buf + off);
						if (ev->wd == runtime_watch) runtime_changed = true;
						if (ev->wd == user_watch) user_changed = true;
						off += sizeof(*ev) + ev->len;
					}
				}
				if (user_changed) {
					refresh_active_user();
					load_settings();
					state.baseline_step = 0;
					stop_effect();
					(void)send_baseline_frame();
					emit_state(conn);
				}
				if (runtime_changed) {
					detect_rgb();
					if (status_fd < 0)
						status_fd = connect_status();
					emit_state(conn);
				}
			} else if (kinds[i] == 3) {
				uint64_t expirations;
				if (read(timer_fd, &expirations, sizeof(expirations)) > 0) {
					if (state.effect != EFFECT_NONE) {
						state.effect_step += (int)expirations;
						if (state.effect_step >= state.effect_steps_total) {
							stop_effect();
							emit_state(conn);
						} else {
							(void)send_effect_frame();
						}
					} else {
						state.baseline_step += (int)expirations - 1;
						(void)send_baseline_frame();
					}
				}
			} else if (kinds[i] == 4) {
				if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
					close(status_fd);
					status_fd = -1;
					status_used = 0;
				} else if (fds[i].revents & POLLIN) {
					process_status_stream();
				}
			}
		}
		while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
			;
	}

	if (status_fd >= 0) close(status_fd);
	if (timer_fd >= 0) close(timer_fd);
	if (inotify_fd >= 0) close(inotify_fd);
	stop_effect();
	dbus_connection_unregister_object_path(conn, OBJECT_PATH);
	dbus_connection_unref(conn);
	return 0;
}
