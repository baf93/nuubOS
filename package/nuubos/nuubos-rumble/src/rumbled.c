/* SPDX-License-Identifier: MIT */

#include <dbus/dbus.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define SERVICE_NAME "org.nuubOS.Rumble"
#define OBJECT_PATH "/org/nuubOS/Rumble"
#define INTERFACE_NAME "org.nuubOS.Rumble1"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"
#define USER_DIR "/run/nuubos/user"
#define ACTIVE_USER USER_DIR "/active"

static volatile sig_atomic_t running = 1;
static char active_user[128] = "default";
static bool supported;
static bool enabled = true;
static char rumble_path[PATH_MAX];
static int test_fd = -1;
static int test_effect_id = -1;
static int test_timer_fd = -1;

static const char introspection_xml[] =
	"<node>"
	"<interface name='org.nuubOS.Rumble1'>"
	"<method name='GetState'>"
	"<arg name='supported' type='b' direction='out'/>"
	"<arg name='enabled' type='b' direction='out'/>"
	"</method>"
	"<method name='SetEnabled'>"
	"<arg name='enabled' type='b' direction='in'/>"
	"</method>"
	"<method name='Test'/>"
	"<signal name='StateChanged'>"
	"<arg name='supported' type='b'/>"
	"<arg name='enabled' type='b'/>"
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
	snprintf(out, size, "/state/users/%s/input-feedback.conf", active_user);
}

static void load_settings(void)
{
	char path[384];
	FILE *fp;
	char line[128];

	enabled = true;
	settings_path(path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		int value;
		if (sscanf(line, "INTERNAL_RUMBLE=%d", &value) == 1)
			enabled = value != 0;
	}
	fclose(fp);
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
	if (fprintf(fp, "INTERNAL_RUMBLE=%d\n", enabled ? 1 : 0) < 0 ||
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

static void detect_rumble(void)
{
	DIR *dir;
	struct dirent *entry;

	supported = false;
	rumble_path[0] = '\0';

	dir = opendir("/dev/input");
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		char name[128] = "";
		unsigned long evbits[(EV_MAX + (sizeof(unsigned long) * 8)) /
				     (sizeof(unsigned long) * 8)];
		unsigned long ffbits[(FF_MAX + (sizeof(unsigned long) * 8)) /
				     (sizeof(unsigned long) * 8)];
		int fd;
		unsigned int word;
		unsigned int bit;

		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
			name[0] = '\0';
		if (strcmp(name, "pwm-vibrator") != 0) {
			close(fd);
			continue;
		}

		memset(evbits, 0, sizeof(evbits));
		memset(ffbits, 0, sizeof(ffbits));
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0 &&
		    ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits) >= 0) {
			word = EV_FF / (sizeof(unsigned long) * 8);
			bit = EV_FF % (sizeof(unsigned long) * 8);
			if ((evbits[word] & (1UL << bit)) != 0) {
				word = FF_RUMBLE / (sizeof(unsigned long) * 8);
				bit = FF_RUMBLE % (sizeof(unsigned long) * 8);
				if ((ffbits[word] & (1UL << bit)) != 0) {
					supported = true;
					snprintf(rumble_path, sizeof(rumble_path), "%s", path);
				}
			}
		}
		close(fd);
		if (supported)
			break;
	}
	closedir(dir);
}

static void stop_test(void)
{
	struct input_event event;

	if (test_fd >= 0 && test_effect_id >= 0) {
		memset(&event, 0, sizeof(event));
		event.type = EV_FF;
		event.code = (uint16_t)test_effect_id;
		event.value = 0;
		if (write(test_fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
			/* Cleanup must continue even if the stop event cannot be delivered. */
		}
		(void)ioctl(test_fd, EVIOCRMFF, test_effect_id);
	}
	if (test_fd >= 0)
		close(test_fd);
	test_fd = -1;
	test_effect_id = -1;
}

static int arm_test_timer(int milliseconds)
{
	struct itimerspec spec;

	if (test_timer_fd < 0)
		return -1;
	memset(&spec, 0, sizeof(spec));
	spec.it_value.tv_sec = milliseconds / 1000;
	spec.it_value.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
	return timerfd_settime(test_timer_fd, 0, &spec, NULL);
}

static int play_test(void)
{
	struct ff_effect effect;
	struct input_event event;

	if (!supported || !enabled) {
		errno = supported ? EPERM : ENODEV;
		return -1;
	}

	stop_test();

	test_fd = open(rumble_path, O_RDWR | O_CLOEXEC);
	if (test_fd < 0)
		return -1;

	memset(&effect, 0, sizeof(effect));
	effect.type = FF_RUMBLE;
	effect.id = -1;
	effect.u.rumble.strong_magnitude = 0xd000;
	effect.u.rumble.weak_magnitude = 0xa000;
	effect.replay.length = 450;
	effect.replay.delay = 0;

	if (ioctl(test_fd, EVIOCSFF, &effect) < 0) {
		stop_test();
		return -1;
	}
	test_effect_id = effect.id;

	memset(&event, 0, sizeof(event));
	event.type = EV_FF;
	event.code = (uint16_t)test_effect_id;
	event.value = 1;

	if (write(test_fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
		stop_test();
		return -1;
	}

	if (arm_test_timer(500) != 0) {
		stop_test();
		return -1;
	}
	return 0;
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
	dbus_bool_t s = supported;
	dbus_bool_t e = enabled;
	if (!signal)
		return;
	dbus_message_append_args(signal,
				 DBUS_TYPE_BOOLEAN, &s,
				 DBUS_TYPE_BOOLEAN, &e,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
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
		dbus_bool_t s = supported;
		dbus_bool_t e = enabled;
		if (reply)
			dbus_message_append_args(reply,
						 DBUS_TYPE_BOOLEAN, &s,
						 DBUS_TYPE_BOOLEAN, &e,
						 DBUS_TYPE_INVALID);
		send_reply(conn, reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "SetEnabled")) {
		dbus_bool_t value;
		DBusError error;
		dbus_error_init(&error);
		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_BOOLEAN, &value,
					   DBUS_TYPE_INVALID)) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Rumble.Error.InvalidArgument",
				"Enabled state is required"));
			dbus_error_free(&error);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		dbus_error_free(&error);
		enabled = value != FALSE;
		if (save_settings() != 0) {
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Rumble.Error.PersistFailed",
				"Unable to persist rumble preference"));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		send_reply(conn, dbus_message_new_method_return(message));
		emit_state(conn);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (dbus_message_is_method_call(message, INTERFACE_NAME, "Test")) {
		if (play_test() != 0) {
			char text[192];
			if (!supported)
				snprintf(text, sizeof(text), "Internal rumble is not supported");
			else if (!enabled)
				snprintf(text, sizeof(text), "Internal rumble is disabled");
			else
				snprintf(text, sizeof(text), "Internal rumble test failed: %s",
					 strerror(errno));
			send_reply(conn, dbus_message_new_error(message,
				"org.nuubOS.Rumble.Error.TestFailed", text));
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		send_reply(conn, dbus_message_new_method_return(message));
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable object_vtable = {
	.unregister_function = NULL,
	.message_function = handle_message,
};

int main(void)
{
	DBusConnection *conn;
	DBusError error;
	int request;
	int dbus_fd = -1;
	int inotify_fd = -1;
	int user_watch = -1;
	int input_watch = -1;

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	refresh_active_user();
	load_settings();
	detect_rumble();
	test_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (test_timer_fd < 0)
		return 1;

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

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0) {
		user_watch = inotify_add_watch(inotify_fd, USER_DIR,
			IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
			IN_CLOSE_WRITE | IN_ATTRIB);
		input_watch = inotify_add_watch(inotify_fd, "/dev/input",
			IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
	}

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
		if (inotify_fd >= 0) {
			fds[count].fd = inotify_fd;
			fds[count].events = POLLIN;
			kinds[count++] = 2;
		}
		if (test_timer_fd >= 0) {
			fds[count].fd = test_timer_fd;
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
			} else if (kinds[i] == 3) {
				uint64_t expirations;
				if (read(test_timer_fd, &expirations, sizeof(expirations)) > 0)
					stop_test();
			} else {
				char buf[4096];
				ssize_t n;
				bool user_changed = false;
				bool input_changed = false;
				while ((n = read(inotify_fd, buf, sizeof(buf))) > 0) {
					size_t off = 0;
					while (off < (size_t)n) {
						struct inotify_event *ev =
							(struct inotify_event *)(buf + off);
						if (ev->wd == user_watch)
							user_changed = true;
						if (ev->wd == input_watch)
							input_changed = true;
						off += sizeof(*ev) + ev->len;
					}
				}
				if (user_changed) {
					refresh_active_user();
					load_settings();
					emit_state(conn);
				}
				if (input_changed) {
					detect_rumble();
					emit_state(conn);
				}
			}
		}
		while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
			;
	}

	stop_test();
	if (test_timer_fd >= 0)
		close(test_timer_fd);
	if (inotify_fd >= 0)
		close(inotify_fd);
	dbus_connection_unregister_object_path(conn, OBJECT_PATH);
	dbus_connection_unref(conn);
	return 0;
}
