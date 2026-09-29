/* SPDX-License-Identifier: MIT */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define SOCKET_PATH "/run/nuubos/inputd.sock"
#define LOCK_PATH "/run/nuubos/inputd.lock"
#define PIDFILE "/run/nuubos-inputd.pid"
#define LOG_PATH "/run/nuubos/inputd.log"
#define SUPPRESS_START_PATH "/run/nuubos/input/suppress-start"
#define DEFAULT_CONFIG "/etc/nuubos/input-bindings.conf"
#define STATE_CONFIG "/state/config/nuubos-input.conf"
#define STATE_CONFIG_TMP "/state/config/.nuubos-input.conf.tmp"

#define MAX_INPUTS 32
#define MAX_CLIENTS 16
#define MAX_LINE 256

#define HORIZONTAL_REPEAT_DELAY_MS 300
#define HORIZONTAL_REPEAT_INTERVAL_MS 60

#define BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define NBITS(n) (((n) + BITS_PER_LONG - 1U) / BITS_PER_LONG)

struct binding {
	const char *name;
	int code;
};

static struct binding bindings[] = {
	{ "quick_menu", 316 },
	{ "settings", 315 },
	{ "menu_up", 544 },
	{ "menu_down", 545 },
	{ "menu_left", 546 },
	{ "menu_right", 547 },
	{ "menu_confirm", 305 },
	{ "menu_back", 304 },
};

struct nav_axis {
	bool present;
	bool horizontal;
	bool discrete;
	int code;
	int minimum;
	int maximum;
	int center;
	int enter_delta;
	int release_delta;
	int direction;
};

struct input_dev {
	int fd;
	bool grabbed;
	char path[64];
	char name[128];

	/*
	 * Navigation is described exclusively through Linux input semantics,
	 * never controller names.  ABS_X/ABS_Y cover the left stick while
	 * ABS_HAT0X/ABS_HAT0Y cover the D-pad used by common Bluetooth pads.
	 */
	struct nav_axis abs_x;
	struct nav_axis abs_y;
	struct nav_axis hat_x;
	struct nav_axis hat_y;
};

enum client_role {
	CLIENT_ROLE_GENERIC = 0,
	CLIENT_ROLE_HOME,
	CLIENT_ROLE_QUICK_MENU,
};

struct client {
	int fd;
	bool subscribed;
	enum client_role role;
	char buf[MAX_LINE];
	size_t used;
};

static struct input_dev inputs[MAX_INPUTS];
static size_t input_count;
static struct client clients[MAX_CLIENTS];
static bool menu_open;
static bool settings_open;
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t reload_requested;
static int service_lock_fd = -1;

static unsigned long long key_event_count;
static unsigned long long mapped_press_count;
static int last_code = -1;
static int last_value = -1;
static char last_action[64] = "none";
static char last_device[128] = "none";
static FILE *log_file;
static char repeat_action[32];
static long long repeat_next_ms;

static void log_message(const char *fmt, ...)
{
	va_list ap;

	if (!log_file) {
		log_file = fopen(LOG_PATH, "a");
		if (log_file)
			setvbuf(log_file, NULL, _IOLBF, 0);
	}

	if (!log_file)
		return;

	va_start(ap, fmt);
	vfprintf(log_file, fmt, ap);
	va_end(ap);

	fputc('\n', log_file);
	fflush(log_file);
}

static void handle_signal(int sig)
{
	if (sig == SIGHUP)
		reload_requested = 1;
	else
		stop_requested = 1;
}

static bool bit_is_set(const unsigned long *bits, unsigned int bit)
{
	unsigned int word = bit / BITS_PER_LONG;
	unsigned int offset = bit % BITS_PER_LONG;

	return (bits[word] & (1UL << offset)) != 0;
}

static bool suppress_start_marker_present(void)
{
	return access(SUPPRESS_START_PATH, F_OK) == 0;
}

static void clear_suppress_start_marker(const char *reason)
{
	if (unlink(SUPPRESS_START_PATH) == 0 || errno == ENOENT) {
		log_message("wake-guard clear reason=%s", reason);
		return;
	}

	log_message("wake-guard clear-failed reason=%s errno=%d",
		    reason, errno);
}

static bool start_is_pressed_on_fd(int fd)
{
	unsigned long keys[NBITS(KEY_MAX + 1)];

	memset(keys, 0, sizeof(keys));

	if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
		return false;

	return bit_is_set(keys, BTN_START);
}

static void reconcile_start_wake_guard(void)
{
	size_t i;

	if (!suppress_start_marker_present())
		return;

	for (i = 0; i < input_count; i++) {
		if (start_is_pressed_on_fd(inputs[i].fd)) {
			log_message(
				"wake-guard active start-already-pressed path=%s name=%s",
				inputs[i].path,
				inputs[i].name);
			return;
		}
	}

	/*
	 * M2.5.3.7/v0.4 behavior: if evdev is recreated after resume and
	 * START is no longer physically down, the wake event has already been
	 * consumed by the kernel/input stack.  End the guard rather than leak
	 * suppression into the next genuine START press.
	 */
	clear_suppress_start_marker("start-not-physically-down-after-rescan");
}

static struct binding *binding_by_name(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
		if (strcmp(bindings[i].name, name) == 0)
			return &bindings[i];
	}

	return NULL;
}

static const char *action_for_code(int code)
{
	size_t i;

	for (i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
		if (bindings[i].code == code)
			return bindings[i].name;
	}

	return NULL;
}

static void load_config_file(const char *path)
{
	FILE *fp;
	char line[MAX_LINE];

	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char name[64];
		char value[64];
		char *newline;
		char *end;
		long code;
		struct binding *binding;

		newline = strchr(line, '\n');
		if (newline)
			*newline = '\0';

		if (line[0] == '#' || line[0] == '\0')
			continue;

		if (sscanf(line, "%63[^=]=%63s", name, value) != 2)
			continue;

		binding = binding_by_name(name);
		if (!binding)
			continue;

		errno = 0;
		code = strtol(value, &end, 10);

		if (errno != 0 || *value == '\0' || *end != '\0' ||
		    code < 0 || code > KEY_MAX)
			continue;

		binding->code = (int)code;
	}

	fclose(fp);
}

static void log_bindings(void)
{
	size_t i;

	for (i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
		log_message("binding action=%s code=%d",
			    bindings[i].name, bindings[i].code);
	}
}

static void load_bindings(void)
{
	bindings[0].code = 316;
	bindings[1].code = 315;
	bindings[2].code = 544;
	bindings[3].code = 545;
	bindings[4].code = 546;
	bindings[5].code = 547;
	bindings[6].code = 305;
	bindings[7].code = 304;

	load_config_file(DEFAULT_CONFIG);
	load_config_file(STATE_CONFIG);
	log_bindings();
}

static int persist_bindings(void)
{
	FILE *fp;
	size_t i;
	int fd;

	if (mkdir("/state/config", 0755) != 0 && errno != EEXIST)
		return -1;

	fp = fopen(STATE_CONFIG_TMP, "w");
	if (!fp)
		return -1;

	for (i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
		if (fprintf(fp, "%s=%d\n",
			    bindings[i].name, bindings[i].code) < 0)
			goto fail;
	}

	if (fflush(fp) != 0)
		goto fail;

	fd = fileno(fp);
	if (fd >= 0 && fsync(fd) != 0)
		goto fail;

	if (fclose(fp) != 0) {
		unlink(STATE_CONFIG_TMP);
		return -1;
	}

	if (chmod(STATE_CONFIG_TMP, 0644) != 0 ||
	    rename(STATE_CONFIG_TMP, STATE_CONFIG) != 0) {
		unlink(STATE_CONFIG_TMP);
		return -1;
	}

	return 0;

fail:
	fclose(fp);
	unlink(STATE_CONFIG_TMP);
	return -1;
}

static int acquire_service_lock(bool handoff)
{
	int flags = LOCK_EX;

	service_lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (service_lock_fd < 0)
		return -1;

	if (!handoff)
		flags |= LOCK_NB;

	if (flock(service_lock_fd, flags) != 0) {
		close(service_lock_fd);
		service_lock_fd = -1;
		return -1;
	}

	return 0;
}

static int write_pidfile(void)
{
	char tmp[128];
	FILE *fp;

	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld",
		 PIDFILE, (long)getpid());

	fp = fopen(tmp, "w");
	if (!fp)
		return -1;

	if (fprintf(fp, "%ld\n", (long)getpid()) < 0 ||
	    fflush(fp) != 0) {
		fclose(fp);
		unlink(tmp);
		return -1;
	}

	if (fclose(fp) != 0) {
		unlink(tmp);
		return -1;
	}

	if (rename(tmp, PIDFILE) != 0) {
		unlink(tmp);
		return -1;
	}

	return 0;
}

static void remove_own_pidfile(void)
{
	FILE *fp;
	long pid = -1;

	fp = fopen(PIDFILE, "r");
	if (!fp)
		return;

	(void)fscanf(fp, "%ld", &pid);
	fclose(fp);

	if (pid == (long)getpid())
		unlink(PIDFILE);
}

static bool input_has_mapped_key(int fd)
{
	unsigned long bits[NBITS(KEY_MAX + 1)];
	size_t i;

	memset(bits, 0, sizeof(bits));

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
		return false;

	for (i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
		if (bit_is_set(bits, (unsigned int)bindings[i].code))
			return true;
	}

	return false;
}

static const char *axis_name(int code)
{
	switch (code) {
	case ABS_X:
		return "ABS_X";
	case ABS_Y:
		return "ABS_Y";
	case ABS_HAT0X:
		return "ABS_HAT0X";
	case ABS_HAT0Y:
		return "ABS_HAT0Y";
	default:
		return "ABS_UNKNOWN";
	}
}

static bool configure_nav_axis(struct input_dev *input,
			       struct nav_axis *axis,
			       int code,
			       bool horizontal,
			       bool discrete)
{
	unsigned long bits[NBITS(ABS_MAX + 1)];
	struct input_absinfo info;
	int range;
	int flat;

	memset(axis, 0, sizeof(*axis));
	axis->code = code;
	axis->horizontal = horizontal;
	axis->discrete = discrete;

	memset(bits, 0, sizeof(bits));

	if (ioctl(input->fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0)
		return false;

	if (!bit_is_set(bits, (unsigned int)code))
		return false;

	memset(&info, 0, sizeof(info));

	if (ioctl(input->fd, EVIOCGABS(code), &info) < 0)
		return false;

	range = info.maximum - info.minimum;
	if (range <= 0)
		return false;

	axis->present = true;
	axis->minimum = info.minimum;
	axis->maximum = info.maximum;
	axis->center = info.minimum + range / 2;
	axis->direction = 0;

	if (discrete) {
		/*
		 * Linux D-pad hats are normally -1/0/+1. Keep them discrete
		 * rather than applying analog hysteresis, which would collapse
		 * with such a small range.
		 */
		axis->enter_delta = 0;
		axis->release_delta = 0;
	} else {
		flat = info.flat;
		if (flat < 0)
			flat = 0;

		/*
		 * 20% activation + 10% release hysteresis matches the H700 ADC
		 * stick nominal 0..4096 range while remaining generic for
		 * Bluetooth controllers with different ranges.
		 */
		axis->enter_delta = range / 5;
		if (axis->enter_delta < flat * 2)
			axis->enter_delta = flat * 2;

		axis->release_delta = range / 10;
		if (axis->release_delta < flat)
			axis->release_delta = flat;
	}

	log_message(
		"nav-axis path=%s name=%s axis=%s min=%d max=%d center=%d enter=%d release=%d discrete=%d",
		input->path,
		input->name,
		axis_name(code),
		axis->minimum,
		axis->maximum,
		axis->center,
		axis->enter_delta,
		axis->release_delta,
		discrete ? 1 : 0);

	return true;
}

static bool input_has_navigation_axis(const struct input_dev *input)
{
	return input->abs_x.present ||
	       input->abs_y.present ||
	       input->hat_x.present ||
	       input->hat_y.present;
}

static bool input_has_menu_capability(const struct input_dev *input)
{
	return input_has_mapped_key(input->fd) ||
	       input_has_navigation_axis(input);
}

static size_t analog_navigation_device_count(void)
{
	size_t i;
	size_t count = 0;

	for (i = 0; i < input_count; i++) {
		if (input_has_navigation_axis(&inputs[i]))
			count++;
	}

	return count;
}

static size_t subscriber_count(void)
{
	size_t i;
	size_t count = 0;

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0 && clients[i].subscribed)
			count++;
	}

	return count;
}

static size_t role_subscriber_count(enum client_role role)
{
	size_t i;
	size_t count = 0;

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0 &&
		    clients[i].subscribed &&
		    clients[i].role == role)
			count++;
	}

	return count;
}

static bool capture_active(void)
{
	return menu_open || settings_open;
}

static void sync_input_grab(void)
{
	size_t i;
	bool enabled = capture_active();

	log_message(
		"capture-sync quick-menu=%d settings=%d enabled=%d inputs=%zu",
		menu_open ? 1 : 0,
		settings_open ? 1 : 0,
		enabled ? 1 : 0,
		input_count);

	for (i = 0; i < input_count; i++) {
		int rc;

		if (!input_has_menu_capability(&inputs[i]))
			continue;

		if (inputs[i].grabbed == enabled)
			continue;

		rc = ioctl(inputs[i].fd, EVIOCGRAB, enabled ? 1 : 0);

		log_message("grab path=%s name=%s on=%d rc=%d errno=%d",
			    inputs[i].path,
			    inputs[i].name,
			    enabled ? 1 : 0,
			    rc,
			    rc < 0 ? errno : 0);

		if (rc == 0)
			inputs[i].grabbed = enabled;
	}

	if (!enabled) {
		repeat_action[0] = '\0';
		repeat_next_ms = 0;
	}
}

static void set_menu_grab(bool enabled)
{
	menu_open = enabled;
	sync_input_grab();
}

static void set_settings_grab(bool enabled)
{
	settings_open = enabled;
	sync_input_grab();
}

static void close_inputs(void)
{
	size_t i;

	for (i = 0; i < input_count; i++) {
		if (inputs[i].grabbed)
			(void)ioctl(inputs[i].fd, EVIOCGRAB, 0);

		close(inputs[i].fd);
	}

	input_count = 0;
}

static void rescan_inputs(void)
{
	DIR *dir;
	struct dirent *entry;
	bool restore_grab = capture_active();

	close_inputs();

	dir = opendir(INPUT_DIR);
	if (!dir) {
		log_message("rescan opendir failed errno=%d", errno);
		return;
	}

	while ((entry = readdir(dir)) != NULL && input_count < MAX_INPUTS) {
		char path[64];
		char name[128];
		int fd;

		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s",
			 INPUT_DIR, entry->d_name);

		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			log_message("open failed path=%s errno=%d",
				    path, errno);
			continue;
		}

		memset(name, 0, sizeof(name));

		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
			snprintf(name, sizeof(name), "unknown");

		memset(&inputs[input_count], 0, sizeof(inputs[input_count]));
		inputs[input_count].fd = fd;
		inputs[input_count].grabbed = false;

		snprintf(inputs[input_count].path,
			 sizeof(inputs[input_count].path), "%s", path);
		snprintf(inputs[input_count].name,
			 sizeof(inputs[input_count].name), "%s", name);

		(void)configure_nav_axis(&inputs[input_count],
					 &inputs[input_count].abs_x,
					 ABS_X, true, false);
		(void)configure_nav_axis(&inputs[input_count],
					 &inputs[input_count].abs_y,
					 ABS_Y, false, false);
		(void)configure_nav_axis(&inputs[input_count],
					 &inputs[input_count].hat_x,
					 ABS_HAT0X, true, true);
		(void)configure_nav_axis(&inputs[input_count],
					 &inputs[input_count].hat_y,
					 ABS_HAT0Y, false, true);

		log_message(
			"input-open index=%zu path=%s name=%s key-capability=%d abs-x=%d abs-y=%d hat-x=%d hat-y=%d",
			input_count, path, name,
			input_has_mapped_key(fd) ? 1 : 0,
			inputs[input_count].abs_x.present ? 1 : 0,
			inputs[input_count].abs_y.present ? 1 : 0,
			inputs[input_count].hat_x.present ? 1 : 0,
			inputs[input_count].hat_y.present ? 1 : 0);

		input_count++;
	}

	closedir(dir);

	log_message("rescan complete inputs=%zu", input_count);

	reconcile_start_wake_guard();

	if (restore_grab)
		sync_input_grab();
}

static int write_all(int fd, const char *text)
{
	const char *p = text;
	size_t remaining = strlen(text);

	while (remaining > 0) {
		ssize_t n = write(fd, p, remaining);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		p += n;
		remaining -= (size_t)n;
	}

	return 0;
}

static bool action_is_navigation(const char *action)
{
	return strcmp(action, "menu_up") == 0 ||
	       strcmp(action, "menu_down") == 0 ||
	       strcmp(action, "menu_left") == 0 ||
	       strcmp(action, "menu_right") == 0 ||
	       strcmp(action, "menu_confirm") == 0 ||
	       strcmp(action, "menu_back") == 0;
}

static bool client_wants_action(const struct client *client,
				const char *action)
{
	if (action_is_navigation(action)) {
		if (menu_open)
			return client->role == CLIENT_ROLE_QUICK_MENU ||
			       client->role == CLIENT_ROLE_GENERIC;

		if (settings_open)
			return client->role == CLIENT_ROLE_HOME ||
			       client->role == CLIENT_ROLE_GENERIC;
	}

	if (strcmp(action, "quick_menu") == 0)
		return client->role == CLIENT_ROLE_QUICK_MENU ||
		       client->role == CLIENT_ROLE_GENERIC;

	if (strcmp(action, "settings") == 0) {
		if (menu_open)
			return false;

		return client->role == CLIENT_ROLE_HOME ||
		       client->role == CLIENT_ROLE_GENERIC;
	}

	return true;
}

static void reconcile_capture_owners(void)
{
	if (menu_open &&
	    role_subscriber_count(CLIENT_ROLE_QUICK_MENU) == 0)
		set_menu_grab(false);

	if (settings_open &&
	    role_subscriber_count(CLIENT_ROLE_HOME) == 0)
		set_settings_grab(false);
}

static void broadcast_action_state(const char *action, const char *state)
{
	char message[128];
	size_t i;

	snprintf(message, sizeof(message),
		 "EVENT 1 %s %s\n", action, state);

	log_message(
		"emit action=%s state=%s subscribers=%zu quick-menu=%d settings=%d",
		action,
		state,
		subscriber_count(),
		menu_open ? 1 : 0,
		settings_open ? 1 : 0);

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd < 0 || !clients[i].subscribed)
			continue;

		if (!client_wants_action(&clients[i], action))
			continue;

		if (write_all(clients[i].fd, message) != 0) {
			close(clients[i].fd);
			clients[i].fd = -1;
			clients[i].subscribed = false;
			clients[i].role = CLIENT_ROLE_GENERIC;
			clients[i].used = 0;
		}
	}

	reconcile_capture_owners();
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static bool repeatable_horizontal_action(const char *action)
{
	return strcmp(action, "menu_left") == 0 ||
	       strcmp(action, "menu_right") == 0;
}

static void emit_action_state(const char *action, const char *state)
{
	broadcast_action_state(action, state);

	if (!repeatable_horizontal_action(action))
		return;

	if (strcmp(state, "pressed") == 0) {
		snprintf(repeat_action, sizeof(repeat_action), "%s", action);
		repeat_next_ms = monotonic_ms() + HORIZONTAL_REPEAT_DELAY_MS;
	} else if (strcmp(state, "released") == 0 &&
		   strcmp(repeat_action, action) == 0) {
		repeat_action[0] = '\0';
		repeat_next_ms = 0;
	}
}

static int horizontal_repeat_timeout_ms(void)
{
	long long remaining;

	if (!capture_active() || repeat_action[0] == '\0')
		return -1;

	remaining = repeat_next_ms - monotonic_ms();
	if (remaining <= 0)
		return 0;
	if (remaining > 1000)
		return 1000;

	return (int)remaining;
}

static void horizontal_repeat_tick(void)
{
	long long now;

	if (!capture_active() || repeat_action[0] == '\0')
		return;

	now = monotonic_ms();
	if (now < repeat_next_ms)
		return;

	mapped_press_count++;
	log_message("horizontal-repeat action=%s", repeat_action);
	snprintf(last_action, sizeof(last_action), "%s", repeat_action);
	snprintf(last_device, sizeof(last_device), "%s", "repeat-timer");
	last_value = 2;
	broadcast_action_state(repeat_action, "pressed");
	repeat_next_ms = now + HORIZONTAL_REPEAT_INTERVAL_MS;
}

static void status_reply(int fd)
{
	char reply[1024];

	snprintf(reply, sizeof(reply),
		 "protocol=1\n"
		 "menu_open=%d\n"
		 "settings_open=%d\n"
		 "inputs=%zu\n"
		 "subscribers=%zu\n"
		 "analog_navigation_devices=%zu\n"
		 "key_events=%llu\n"
		 "mapped_presses=%llu\n"
		 "last_code=%d\n"
		 "last_value=%d\n"
		 "last_action=%s\n"
		 "last_device=%s\n"
		 "quick_menu=%d\n"
		 "settings=%d\n"
		 "menu_up=%d\n"
		 "menu_down=%d\n"
		 "menu_left=%d\n"
		 "menu_right=%d\n"
		 "menu_confirm=%d\n"
		 "menu_back=%d\n",
		 menu_open ? 1 : 0,
		 settings_open ? 1 : 0,
		 input_count,
		 subscriber_count(),
		 analog_navigation_device_count(),
		 key_event_count,
		 mapped_press_count,
		 last_code,
		 last_value,
		 last_action,
		 last_device,
		 binding_by_name("quick_menu")->code,
		 binding_by_name("settings")->code,
		 binding_by_name("menu_up")->code,
		 binding_by_name("menu_down")->code,
		 binding_by_name("menu_left")->code,
		 binding_by_name("menu_right")->code,
		 binding_by_name("menu_confirm")->code,
		 binding_by_name("menu_back")->code);

	(void)write_all(fd, reply);
}

static void handle_command(struct client *client, const char *line)
{
	char action[64];
	int code;
	struct binding *binding;

	if (strcmp(line, "SUBSCRIBE") == 0 ||
	    strcmp(line, "SUBSCRIBE GENERIC") == 0) {
		client->subscribed = true;
		client->role = CLIENT_ROLE_GENERIC;
		(void)write_all(client->fd, "OK protocol=1\n");
		return;
	}

	if (strcmp(line, "SUBSCRIBE HOME") == 0) {
		client->subscribed = true;
		client->role = CLIENT_ROLE_HOME;
		(void)write_all(client->fd, "OK protocol=1 role=home\n");
		return;
	}

	if (strcmp(line, "SUBSCRIBE QUICK_MENU") == 0) {
		client->subscribed = true;
		client->role = CLIENT_ROLE_QUICK_MENU;
		(void)write_all(client->fd, "OK protocol=1 role=quick-menu\n");
		return;
	}

	if (strcmp(line, "STATUS") == 0) {
		status_reply(client->fd);
		return;
	}

	if (strcmp(line, "MENU OPEN") == 0) {
		set_menu_grab(true);
		(void)write_all(client->fd, "OK\n");
		return;
	}

	if (strcmp(line, "MENU CLOSE") == 0) {
		set_menu_grab(false);
		(void)write_all(client->fd, "OK\n");
		return;
	}

	if (strcmp(line, "SETTINGS OPEN") == 0) {
		set_settings_grab(true);
		(void)write_all(client->fd, "OK\n");
		return;
	}

	if (strcmp(line, "SETTINGS CLOSE") == 0) {
		set_settings_grab(false);
		(void)write_all(client->fd, "OK\n");
		return;
	}

	if (sscanf(line, "BIND GET %63s", action) == 1) {
		binding = binding_by_name(action);

		if (!binding) {
			(void)write_all(client->fd,
					"ERR unknown action\n");
			return;
		}

		{
			char reply[64];

			snprintf(reply, sizeof(reply),
				 "%d\n", binding->code);
			(void)write_all(client->fd, reply);
		}

		return;
	}

	if (sscanf(line, "BIND SET %63s %d", action, &code) == 2) {
		binding = binding_by_name(action);

		if (!binding || code < 0 || code > KEY_MAX) {
			(void)write_all(client->fd,
					"ERR invalid binding\n");
			return;
		}

		binding->code = code;

		if (persist_bindings() != 0) {
			(void)write_all(client->fd,
					"ERR persist failed\n");
			return;
		}

		if (menu_open) {
			set_menu_grab(false);
			set_menu_grab(true);
		}

		(void)write_all(client->fd, "OK\n");
		return;
	}

	(void)write_all(client->fd, "ERR unknown command\n");
}

static void process_client(struct client *client)
{
	char temp[128];
	ssize_t n;
	size_t i;

	n = read(client->fd, temp, sizeof(temp));

	if (n <= 0) {
		enum client_role role = client->role;

		close(client->fd);
		client->fd = -1;
		client->subscribed = false;
		client->role = CLIENT_ROLE_GENERIC;
		client->used = 0;

		if (role == CLIENT_ROLE_QUICK_MENU &&
		    role_subscriber_count(CLIENT_ROLE_QUICK_MENU) == 0 &&
		    menu_open)
			set_menu_grab(false);

		if (role == CLIENT_ROLE_HOME &&
		    role_subscriber_count(CLIENT_ROLE_HOME) == 0 &&
		    settings_open)
			set_settings_grab(false);

		return;
	}

	for (i = 0; i < (size_t)n; i++) {
		char c = temp[i];

		if (c == '\n') {
			client->buf[client->used] = '\0';
			handle_command(client, client->buf);
			client->used = 0;
			continue;
		}

		if (client->used + 1 < sizeof(client->buf))
			client->buf[client->used++] = c;
		else
			client->used = 0;
	}
}

static int make_server_socket(void)
{
	int fd;
	struct sockaddr_un addr;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	snprintf(addr.sun_path, sizeof(addr.sun_path),
		 "%s", SOCKET_PATH);

	unlink(SOCKET_PATH);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}

	if (chmod(SOCKET_PATH, 0660) != 0 ||
	    listen(fd, 8) != 0) {
		close(fd);
		unlink(SOCKET_PATH);
		return -1;
	}

	return fd;
}

static void accept_client(int server_fd)
{
	int fd;
	size_t i;

	fd = accept4(server_fd, NULL, NULL,
		     SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return;

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd < 0) {
			clients[i].fd = fd;
			clients[i].subscribed = false;
			clients[i].role = CLIENT_ROLE_GENERIC;
			clients[i].used = 0;
			return;
		}
	}

	close(fd);
}

static void process_nav_axis(struct input_dev *input,
			     struct nav_axis *axis,
			     int value)
{
	int previous;
	int next;
	int enter_low;
	int enter_high;
	int release_low;
	int release_high;
	const char *action;

	if (!axis->present)
		return;

	previous = axis->direction;
	next = previous;

	if (axis->discrete) {
		if (value < axis->center)
			next = -1;
		else if (value > axis->center)
			next = 1;
		else
			next = 0;
	} else {
		enter_low = axis->center - axis->enter_delta;
		enter_high = axis->center + axis->enter_delta;
		release_low = axis->center - axis->release_delta;
		release_high = axis->center + axis->release_delta;

		if (value <= enter_low)
			next = -1;
		else if (value >= enter_high)
			next = 1;
		else if (previous < 0 && value < release_low)
			next = -1;
		else if (previous > 0 && value > release_high)
			next = 1;
		else
			next = 0;
	}

	if (next == previous)
		return;

	axis->direction = next;

	log_message(
		"nav-axis-event device=%s axis=%s value=%d previous=%d next=%d quick-menu=%d settings=%d",
		input->name,
		axis_name(axis->code),
		value,
		previous,
		next,
		menu_open ? 1 : 0,
		settings_open ? 1 : 0);

	/*
	 * Analog/hat navigation belongs to the active controller UI capture
	 * domain. Quick Menu has routing priority over Settings. A held axis
	 * is represented as one logical press followed by one logical release.
	 */
	if (!capture_active())
		return;

	if (previous != 0) {
		if (axis->horizontal)
			action = previous < 0 ? "menu_left" : "menu_right";
		else
			action = previous < 0 ? "menu_up" : "menu_down";

		emit_action_state(action, "released");
	}

	if (next == 0)
		return;

	if (axis->horizontal)
		action = next < 0 ? "menu_left" : "menu_right";
	else
		action = next < 0 ? "menu_up" : "menu_down";

	mapped_press_count++;

	snprintf(last_action, sizeof(last_action), "%s", action);
	snprintf(last_device, sizeof(last_device), "%s", input->name);
	last_code = axis->code;
	last_value = value;
	emit_action_state(action, "pressed");
}

static bool process_navigation_abs(struct input_dev *input,
				   const struct input_event *event)
{
	switch (event->code) {
	case ABS_X:
		process_nav_axis(input, &input->abs_x, event->value);
		return true;
	case ABS_Y:
		process_nav_axis(input, &input->abs_y, event->value);
		return true;
	case ABS_HAT0X:
		process_nav_axis(input, &input->hat_x, event->value);
		return true;
	case ABS_HAT0Y:
		process_nav_axis(input, &input->hat_y, event->value);
		return true;
	default:
		return false;
	}
}

static void process_input(struct input_dev *input)
{
	struct input_event events[16];
	ssize_t n;
	size_t count;
	size_t i;

	n = read(input->fd, events, sizeof(events));
	if (n <= 0)
		return;

	count = (size_t)n / sizeof(events[0]);

	for (i = 0; i < count; i++) {
		const char *action;

		if (events[i].type == EV_ABS) {
			if (process_navigation_abs(input, &events[i]))
				continue;
		}

		if (events[i].type != EV_KEY)
			continue;

		key_event_count++;
		last_code = events[i].code;
		last_value = events[i].value;

		snprintf(last_device, sizeof(last_device),
			 "%s", input->name);

		/*
		 * Qualified M2.5.3.7/v0.4 wake-key suppression:
		 * when START was the s2idle wake source, consume its press/repeat/
		 * release sequence.  The physical release ends the session-bound
		 * guard and is not forwarded to Quick Menu/Home.
		 */
		if (events[i].code == BTN_START &&
		    suppress_start_marker_present()) {
			log_message(
				"wake-guard suppress-start value=%d device=%s",
				events[i].value,
				input->name);

			snprintf(last_action, sizeof(last_action),
				 "suppressed_start");

			if (events[i].value == 0)
				clear_suppress_start_marker("physical-start-release");

			continue;
		}

		action = action_for_code(events[i].code);

		snprintf(last_action, sizeof(last_action),
			 "%s", action ? action : "none");

		if (!action)
			continue;

		log_message(
			"key-action device=%s code=%d value=%d action=%s quick-menu=%d settings=%d",
			input->name,
			events[i].code,
			events[i].value,
			action,
			menu_open ? 1 : 0,
			settings_open ? 1 : 0);

		if (events[i].value == 1) {
			mapped_press_count++;
			emit_action_state(action, "pressed");
		} else if (events[i].value == 0) {
			emit_action_state(action, "released");
		}
	}
}

static void cleanup_runtime(int server_fd,
			    int inotify_fd,
			    int watch_fd)
{
	size_t i;

	menu_open = false;
	settings_open = false;
	sync_input_grab();
	close_inputs();

	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd >= 0)
			close(clients[i].fd);
	}

	if (watch_fd >= 0 && inotify_fd >= 0)
		inotify_rm_watch(inotify_fd, watch_fd);

	if (inotify_fd >= 0)
		close(inotify_fd);

	if (server_fd >= 0)
		close(server_fd);

	/*
	 * The service lock is still held here. A --handoff successor cannot
	 * acquire it and bind the same path until all old-runtime cleanup,
	 * including socket removal, has completed.
	 */
	unlink(SOCKET_PATH);
	remove_own_pidfile();

	if (service_lock_fd >= 0) {
		flock(service_lock_fd, LOCK_UN);
		close(service_lock_fd);
		service_lock_fd = -1;
	}
}

int main(int argc, char **argv)
{
	int server_fd = -1;
	int inotify_fd = -1;
	int watch_fd = -1;
	size_t i;
	bool handoff = false;

	if (argc == 2 && strcmp(argv[1], "--handoff") == 0)
		handoff = true;
	else if (argc != 1) {
		fprintf(stderr,
			"Usage: %s [--handoff]\n", argv[0]);
		return 2;
	}

	if (mkdir("/run/nuubos", 0755) != 0 && errno != EEXIST) {
		perror("nuubos-inputd: mkdir /run/nuubos");
		return 1;
	}

	if (acquire_service_lock(handoff) != 0) {
		if (!handoff && errno == EWOULDBLOCK)
			return 0;

		perror("nuubos-inputd: service lock");
		return 1;
	}

	/*
	 * A normal start is single-instance and non-blocking. A restart starts
	 * a --handoff successor: the kernel blocks it on flock until the old
	 * generation has completely cleaned up and released the service lock.
	 * No sleep/retry/rebind loop is required.
	 */
	log_message("nuubos-inputd start pid=%ld handoff=%d",
		    (long)getpid(), handoff ? 1 : 0);

	if (write_pidfile() != 0) {
		perror("nuubos-inputd: pidfile");
		cleanup_runtime(server_fd, inotify_fd, watch_fd);
		return 1;
	}

	for (i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
		clients[i].role = CLIENT_ROLE_GENERIC;
	}

	signal(SIGTERM, handle_signal);
	signal(SIGINT, handle_signal);
	signal(SIGHUP, handle_signal);

	load_bindings();
	rescan_inputs();

	server_fd = make_server_socket();
	if (server_fd < 0) {
		perror("nuubos-inputd: socket");
		cleanup_runtime(server_fd, inotify_fd, watch_fd);
		return 1;
	}

	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd < 0) {
		perror("nuubos-inputd: inotify");
		cleanup_runtime(server_fd, inotify_fd, watch_fd);
		return 1;
	}

	watch_fd = inotify_add_watch(inotify_fd, INPUT_DIR,
				    IN_CREATE | IN_DELETE |
				    IN_MOVED_FROM | IN_MOVED_TO);
	if (watch_fd < 0) {
		perror("nuubos-inputd: inotify watch");
		cleanup_runtime(server_fd, inotify_fd, watch_fd);
		return 1;
	}

	while (!stop_requested) {
		struct pollfd pfds[2 + MAX_INPUTS + MAX_CLIENTS];
		size_t kinds[2 + MAX_INPUTS + MAX_CLIENTS];
		size_t indexes[2 + MAX_INPUTS + MAX_CLIENTS];
		nfds_t count = 0;
		int rc;

		if (reload_requested) {
			reload_requested = 0;
			load_bindings();

			if (menu_open) {
				set_menu_grab(false);
				set_menu_grab(true);
			}
		}

		pfds[count].fd = server_fd;
		pfds[count].events = POLLIN;
		kinds[count] = 0;
		indexes[count] = 0;
		count++;

		pfds[count].fd = inotify_fd;
		pfds[count].events = POLLIN;
		kinds[count] = 1;
		indexes[count] = 0;
		count++;

		for (i = 0; i < input_count; i++) {
			pfds[count].fd = inputs[i].fd;
			pfds[count].events = POLLIN;
			kinds[count] = 2;
			indexes[count] = i;
			count++;
		}

		for (i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;

			pfds[count].fd = clients[i].fd;
			pfds[count].events = POLLIN | POLLHUP | POLLERR;
			kinds[count] = 3;
			indexes[count] = i;
			count++;
		}

		rc = poll(pfds, count, horizontal_repeat_timeout_ms());
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		horizontal_repeat_tick();

		for (i = 0; i < (size_t)count; i++) {
			if (pfds[i].revents == 0)
				continue;

			switch (kinds[i]) {
			case 0:
				accept_client(server_fd);
				break;
			case 1: {
				char buffer[512];

				while (read(inotify_fd,
					    buffer,
					    sizeof(buffer)) > 0)
					;

				rescan_inputs();
				break;
			}
			case 2:
				if (indexes[i] < input_count)
					process_input(&inputs[indexes[i]]);
				break;
			case 3:
				if (indexes[i] < MAX_CLIENTS)
					process_client(&clients[indexes[i]]);
				break;
			default:
				break;
			}
		}
	}

	log_message("nuubos-inputd stop pid=%ld",
		    (long)getpid());

	cleanup_runtime(server_fd, inotify_fd, watch_fd);

	if (log_file)
		fclose(log_file);

	return 0;
}
