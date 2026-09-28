/* SPDX-License-Identifier: MIT */

#include <alsa/asoundlib.h>
#include <ctype.h>
#include <dirent.h>
#include <dbus/dbus.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/nuubos/audiod.sock"
#define CONFIG_PATH "/state/config/nuubos.conf"
#define CONFIG_TMP  "/state/config/.nuubos.conf.audio.tmp"
#define INPUT_DIR   "/dev/input"
#define RUNTIME_DIR "/run/nuubos"
#define DISPLAY_STATE_PATH "/run/nuubos/displayd.state"

#define BLUEALSA_SERVICE "org.bluealsa"
#define BLUEALSA_PATH "/org/bluealsa"
#define BLUEALSA_PCM_INTERFACE "org.bluealsa.PCM1"
#define DBUS_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define DBUS_PROPERTIES "org.freedesktop.DBus.Properties"

#define NUUBOS_BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define NUUBOS_NBITS(n) \
	(((n) + NUUBOS_BITS_PER_LONG - 1U) / NUUBOS_BITS_PER_LONG)

struct audio_state {
	char mode[16];
	char selected[16];

	int volume_speaker;
	int volume_headphones;
	int volume_bluetooth;

	bool headphones_known;
	bool headphones_available;

	bool hdmi_known;
	bool hdmi_available;

	bool bluetooth_available;
	char bluetooth_pcm_path[256];
	char bluetooth_device_path[256];
};

static void apply_selected_bluetooth_volume(
	DBusConnection *conn,
	const struct audio_state *state);

static volatile sig_atomic_t stop_requested;

static void handle_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static bool valid_output(const char *output)
{
	return strcmp(output, "auto") == 0 ||
	       strcmp(output, "bluetooth") == 0 ||
	       strcmp(output, "headphones") == 0 ||
	       strcmp(output, "hdmi") == 0 ||
	       strcmp(output, "speaker") == 0;
}

static bool volume_supported(const char *output)
{
	return strcmp(output, "speaker") == 0 ||
	       strcmp(output, "headphones") == 0 ||
	       strcmp(output, "bluetooth") == 0;
}

static void lowercase(char *s)
{
	for (; *s; s++)
		*s = (char)tolower((unsigned char)*s);
}

static bool copy_string(char *dst, size_t size, const char *src)
{
	size_t len;

	if (size == 0)
		return false;

	len = strlen(src);
	if (len >= size)
		return false;

	memcpy(dst, src, len + 1);
	return true;
}

static int set_codec_dac_volume(int percent)
{
	snd_mixer_t *mixer = NULL;
	snd_mixer_elem_t *elem;
	snd_mixer_selem_id_t *sid;
	long min;
	long max;
	long value;
	int rc;

	if (percent < 0 || percent > 100)
		return -1;

	rc = snd_mixer_open(&mixer, 0);
	if (rc < 0)
		return -1;

	rc = snd_mixer_attach(mixer, "hw:Codec");
	if (rc < 0)
		goto out;

	rc = snd_mixer_selem_register(mixer, NULL, NULL);
	if (rc < 0)
		goto out;

	rc = snd_mixer_load(mixer);
	if (rc < 0)
		goto out;

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, "DAC");

	elem = snd_mixer_find_selem(mixer, sid);
	if (!elem) {
		rc = -1;
		goto out;
	}

	rc = snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	if (rc < 0)
		goto out;

	value = min + ((max - min) * percent + 50) / 100;

	rc = snd_mixer_selem_set_playback_volume_all(elem, value);
	if (rc < 0)
		goto out;

	rc = snd_mixer_selem_set_playback_switch_all(elem, 1);

out:
	snd_mixer_close(mixer);
	return rc < 0 ? -1 : 0;
}

static void apply_selected_analog_volume(const struct audio_state *state)
{
	int volume;

	if (strcmp(state->selected, "speaker") == 0)
		volume = state->volume_speaker;
	else if (strcmp(state->selected, "headphones") == 0)
		volume = state->volume_headphones;
	else
		return;

	if (set_codec_dac_volume(volume) < 0)
		fprintf(stderr,
			"nuubos-audiod: failed to set Codec DAC volume\n");
}

static void reconcile_selection(struct audio_state *state)
{
	const char *selected;

	if (strcmp(state->mode, "auto") != 0) {
		selected = state->mode;
	} else if (state->bluetooth_available) {
		selected = "bluetooth";
	} else if (state->headphones_known &&
		   state->headphones_available) {
		selected = "headphones";
	} else if (state->hdmi_known &&
		   state->hdmi_available) {
		selected = "hdmi";
	} else {
		selected = "speaker";
	}

	(void)copy_string(state->selected,
			  sizeof(state->selected),
			  selected);

	apply_selected_analog_volume(state);
}

static void uppercase_copy(char *dst, size_t size, const char *src)
{
	size_t i;

	for (i = 0; src[i] != '\0' && i + 1 < size; i++)
		dst[i] = (char)toupper((unsigned char)src[i]);

	dst[i] = '\0';
}

static int parse_volume(const char *value)
{
	char *end;
	long volume;

	errno = 0;
	volume = strtol(value, &end, 10);

	if (errno != 0 || *value == '\0' || *end != '\0' ||
	    volume < 0 || volume > 100)
		return -1;

	return (int)volume;
}

static void load_config(struct audio_state *state)
{
	FILE *fp;
	char line[256];

	snprintf(state->mode, sizeof(state->mode), "auto");
	snprintf(state->selected, sizeof(state->selected), "unresolved");

	state->volume_speaker = 100;
	state->volume_headphones = 100;
	state->volume_bluetooth = 100;

	state->headphones_known = false;
	state->headphones_available = false;

	state->hdmi_known = false;
	state->hdmi_available = false;

	state->bluetooth_available = false;
	state->bluetooth_pcm_path[0] = '\0';
	state->bluetooth_device_path[0] = '\0';

	fp = fopen(CONFIG_PATH, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char *newline = strchr(line, '\n');

		if (newline)
			*newline = '\0';

		if (strncmp(line, "AUDIO_OUTPUT=", 13) == 0) {
			char mode[sizeof(state->mode)];

			if (copy_string(mode, sizeof(mode), line + 13)) {
				lowercase(mode);

				if (valid_output(mode))
					(void)copy_string(state->mode,
							  sizeof(state->mode),
							  mode);
			}
		} else if (strncmp(line, "AUDIO_VOLUME_SPEAKER=", 21) == 0) {
			int v = parse_volume(line + 21);
			if (v >= 0)
				state->volume_speaker = v;
		} else if (strncmp(line, "AUDIO_VOLUME_HEADPHONES=", 24) == 0) {
			int v = parse_volume(line + 24);
			if (v >= 0)
				state->volume_headphones = v;
		} else if (strncmp(line, "AUDIO_VOLUME_BLUETOOTH=", 24) == 0) {
			int v = parse_volume(line + 24);
			if (v >= 0)
				state->volume_bluetooth = v;
		}
	}

	fclose(fp);
}

static int update_config(const char *key, const char *value)
{
	FILE *in;
	FILE *out;
	char line[256];
	bool replaced = false;
	size_t key_len = strlen(key);
	int fd;

	in = fopen(CONFIG_PATH, "r");
	if (!in)
		return -1;

	out = fopen(CONFIG_TMP, "w");
	if (!out) {
		fclose(in);
		return -1;
	}

	while (fgets(line, sizeof(line), in)) {
		if (strncmp(line, key, key_len) == 0 &&
		    line[key_len] == '=') {
			if (fprintf(out, "%s=%s\n", key, value) < 0)
				goto fail;
			replaced = true;
		} else {
			if (fputs(line, out) == EOF)
				goto fail;
		}
	}

	if (!replaced && fprintf(out, "%s=%s\n", key, value) < 0)
		goto fail;

	if (fflush(out) != 0)
		goto fail;

	fd = fileno(out);
	if (fd >= 0 && fsync(fd) != 0)
		goto fail;

	if (fclose(out) != 0) {
		fclose(in);
		unlink(CONFIG_TMP);
		return -1;
	}

	fclose(in);

	if (chmod(CONFIG_TMP, 0600) != 0) {
		unlink(CONFIG_TMP);
		return -1;
	}

	if (rename(CONFIG_TMP, CONFIG_PATH) != 0) {
		unlink(CONFIG_TMP);
		return -1;
	}

	return 0;

fail:
	fclose(out);
	fclose(in);
	unlink(CONFIG_TMP);
	return -1;
}

static int write_reply(int fd, const char *reply)
{
	size_t remaining = strlen(reply);
	const char *p = reply;

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

static int get_volume(const struct audio_state *state, const char *output)
{
	if (strcmp(output, "speaker") == 0)
		return state->volume_speaker;
	if (strcmp(output, "headphones") == 0)
		return state->volume_headphones;
	if (strcmp(output, "bluetooth") == 0)
		return state->volume_bluetooth;

	return -1;
}

static void set_volume_memory(struct audio_state *state,
			      const char *output, int volume)
{
	if (strcmp(output, "speaker") == 0)
		state->volume_speaker = volume;
	else if (strcmp(output, "headphones") == 0)
		state->volume_headphones = volume;
	else if (strcmp(output, "bluetooth") == 0)
		state->volume_bluetooth = volume;
}

static const char *volume_key(const char *output)
{
	if (strcmp(output, "speaker") == 0)
		return "AUDIO_VOLUME_SPEAKER";
	if (strcmp(output, "headphones") == 0)
		return "AUDIO_VOLUME_HEADPHONES";
	if (strcmp(output, "bluetooth") == 0)
		return "AUDIO_VOLUME_BLUETOOTH";

	return NULL;
}

static void status_reply(const struct audio_state *state,
			 char *reply, size_t size)
{
	const char *headphones;
	const char *hdmi;

	if (!state->headphones_known)
		headphones = "unknown";
	else
		headphones = state->headphones_available ? "1" : "0";

	if (!state->hdmi_known)
		hdmi = "unknown";
	else
		hdmi = state->hdmi_available ? "1" : "0";

	snprintf(reply, size,
		 "mode=%s\n"
		 "selected=%s\n"
		 "available.bluetooth=%d\n"
		 "available.headphones=%s\n"
		 "available.hdmi=%s\n"
		 "available.speaker=1\n"
		 "volume.speaker=%d\n"
		 "volume.headphones=%d\n"
		 "volume.bluetooth=%d\n"
		 "volume.hdmi=unsupported\n",
		 state->mode,
		 state->selected,
		 state->bluetooth_available ? 1 : 0,
		 headphones,
		 hdmi,
		 state->volume_speaker,
		 state->volume_headphones,
		 state->volume_bluetooth);
}

static bool bluetooth_device_path_to_mac(const char *path,
					 char *mac,
					 size_t size)
{
	const char *p;
	size_t i;

	if (path == NULL)
		return false;

	p = strstr(path, "/dev_");
	if (p == NULL)
		return false;

	p += 5;

	if (strlen(p) != 17 || size < 18)
		return false;

	for (i = 0; i < 17; i++) {
		char c = p[i];

		if ((i + 1) % 3 == 0) {
			if (c != '_')
				return false;
			mac[i] = ':';
		} else {
			if (!isxdigit((unsigned char)c))
				return false;
			mac[i] = c;
		}
	}

	mac[17] = '\0';
	return true;
}

static void route_reply(const struct audio_state *state,
			char *reply,
			size_t reply_size)
{
	char mac[18];

	if (strcmp(state->selected, "speaker") == 0) {
		snprintf(reply, reply_size,
			 "plughw:CARD=Codec,DEV=0\n");
		return;
	}

	if (strcmp(state->selected, "headphones") == 0) {
		if (!state->headphones_known ||
		    !state->headphones_available) {
			snprintf(reply, reply_size, "unavailable\n");
			return;
		}

		snprintf(reply, reply_size,
			 "plughw:CARD=Codec,DEV=0\n");
		return;
	}

	if (strcmp(state->selected, "hdmi") == 0) {
		if (!state->hdmi_known ||
		    !state->hdmi_available) {
			snprintf(reply, reply_size, "unavailable\n");
			return;
		}

		snprintf(reply, reply_size,
			 "plughw:CARD=HDMI,DEV=0\n");
		return;
	}

	if (strcmp(state->selected, "bluetooth") == 0) {
		if (!state->bluetooth_available ||
		    !bluetooth_device_path_to_mac(
			    state->bluetooth_device_path,
			    mac, sizeof(mac))) {
			snprintf(reply, reply_size, "unavailable\n");
			return;
		}

		snprintf(reply, reply_size,
			 "bluealsa:DEV=%s,PROFILE=a2dp\n",
			 mac);
		return;
	}

	snprintf(reply, reply_size, "unavailable\n");
}

static void handle_command(struct audio_state *state,
			   DBusConnection *dbus_conn,
			   const char *command,
			   char *reply, size_t reply_size)
{
	char action[32];
	char arg1[32];
	char arg2[32];

	if (strcmp(command, "STATUS") == 0) {
		status_reply(state, reply, reply_size);
		return;
	}

	if (strcmp(command, "OUTPUT GET") == 0) {
		snprintf(reply, reply_size, "%s\n", state->mode);
		return;
	}

	if (strcmp(command, "ROUTE GET") == 0) {
		route_reply(state, reply, reply_size);
		return;
	}

	if (sscanf(command, "OUTPUT SET %31s", arg1) == 1) {
		char persistent[32];

		lowercase(arg1);

		if (!valid_output(arg1)) {
			snprintf(reply, reply_size,
				 "ERR invalid output\n");
			return;
		}

		uppercase_copy(persistent, sizeof(persistent), arg1);

		if (update_config("AUDIO_OUTPUT", persistent) != 0) {
			snprintf(reply, reply_size,
				 "ERR cannot persist output: %s\n",
				 strerror(errno));
			return;
		}

		if (!copy_string(state->mode, sizeof(state->mode), arg1)) {
			snprintf(reply, reply_size,
				 "ERR output name too long\n");
			return;
		}

		reconcile_selection(state);
		apply_selected_bluetooth_volume(dbus_conn, state);

		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "VOLUME GET") == 0) {
		snprintf(reply, reply_size,
			 "speaker=%d\n"
			 "headphones=%d\n"
			 "bluetooth=%d\n"
			 "hdmi=unsupported\n",
			 state->volume_speaker,
			 state->volume_headphones,
			 state->volume_bluetooth);
		return;
	}

	if (sscanf(command, "VOLUME GET %31s", arg1) == 1) {
		int volume;

		lowercase(arg1);

		if (strcmp(arg1, "hdmi") == 0) {
			snprintf(reply, reply_size, "unsupported\n");
			return;
		}

		if (!volume_supported(arg1)) {
			snprintf(reply, reply_size,
				 "ERR invalid output\n");
			return;
		}

		volume = get_volume(state, arg1);
		snprintf(reply, reply_size, "%d\n", volume);
		return;
	}

	if (sscanf(command, "VOLUME SET %31s %31s",
		   arg1, arg2) == 2) {
		const char *key;
		char value[8];
		int volume;

		lowercase(arg1);

		if (strcmp(arg1, "hdmi") == 0) {
			snprintf(reply, reply_size,
				 "ERR HDMI volume unsupported\n");
			return;
		}

		if (!volume_supported(arg1)) {
			snprintf(reply, reply_size,
				 "ERR invalid output\n");
			return;
		}

		volume = parse_volume(arg2);
		if (volume < 0) {
			snprintf(reply, reply_size,
				 "ERR volume must be 0..100\n");
			return;
		}

		key = volume_key(arg1);
		snprintf(value, sizeof(value), "%d", volume);

		if (update_config(key, value) != 0) {
			snprintf(reply, reply_size,
				 "ERR cannot persist volume: %s\n",
				 strerror(errno));
			return;
		}

		set_volume_memory(state, arg1, volume);

		if (strcmp(state->selected, arg1) == 0) {
			apply_selected_analog_volume(state);

			if (strcmp(arg1, "bluetooth") == 0)
				apply_selected_bluetooth_volume(
					dbus_conn, state);
		}

		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (sscanf(command, "%31s", action) == 1) {
		snprintf(reply, reply_size,
			 "ERR unknown command\n");
		return;
	}

	snprintf(reply, reply_size, "ERR empty command\n");
}

static bool input_bit_is_set(const unsigned long *bits,
			     unsigned int bit)
{
	return (bits[bit / NUUBOS_BITS_PER_LONG] &
		(1UL << (bit % NUUBOS_BITS_PER_LONG))) != 0;
}

static int open_headphone_jack(struct audio_state *state)
{
	DIR *dir;
	struct dirent *entry;
	int jack_fd = -1;

	state->headphones_known = false;
	state->headphones_available = false;

	dir = opendir(INPUT_DIR);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		unsigned long ev_bits[NUUBOS_NBITS(EV_MAX + 1)] = { 0 };
		unsigned long sw_bits[NUUBOS_NBITS(SW_MAX + 1)] = { 0 };
		unsigned long current[NUUBOS_NBITS(SW_MAX + 1)] = { 0 };
		char path[256];
		int fd;
		int n;

		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;

		n = snprintf(path, sizeof(path),
			     INPUT_DIR "/%s", entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(path))
			continue;

		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 ||
		    !input_bit_is_set(ev_bits, EV_SW)) {
			close(fd);
			continue;
		}

		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits) < 0 ||
		    !input_bit_is_set(sw_bits, SW_HEADPHONE_INSERT)) {
			close(fd);
			continue;
		}

		if (ioctl(fd, EVIOCGSW(sizeof(current)), current) < 0) {
			close(fd);
			continue;
		}

		state->headphones_known = true;
		state->headphones_available =
			input_bit_is_set(current, SW_HEADPHONE_INSERT);

		jack_fd = fd;
		break;
	}

	closedir(dir);
	return jack_fd;
}

static bool drain_headphone_jack(int fd, struct audio_state *state)
{
	struct input_event events[16];

	for (;;) {
		ssize_t n;
		size_t count;
		size_t i;

		n = read(fd, events, sizeof(events));

		if (n < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return true;

			state->headphones_known = false;
			state->headphones_available = false;
			return false;
		}

		if (n == 0) {
			state->headphones_known = false;
			state->headphones_available = false;
			return false;
		}

		count = (size_t)n / sizeof(events[0]);

		for (i = 0; i < count; i++) {
			if (events[i].type == EV_SW &&
			    events[i].code == SW_HEADPHONE_INSERT) {
				state->headphones_known = true;
				state->headphones_available =
					events[i].value != 0;
				reconcile_selection(state);
			}
		}
	}
}

static void refresh_hdmi_state(struct audio_state *state)
{
	FILE *fp;
	char line[128];
	bool found = false;

	fp = fopen(DISPLAY_STATE_PATH, "r");
	if (!fp) {
		state->hdmi_known = false;
		state->hdmi_available = false;
		return;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (strcmp(line, "hdmi=connected\n") == 0 ||
		    strcmp(line, "hdmi=connected") == 0) {
			state->hdmi_known = true;
			state->hdmi_available = true;
			found = true;
			break;
		}

		if (strcmp(line, "hdmi=disconnected\n") == 0 ||
		    strcmp(line, "hdmi=disconnected") == 0) {
			state->hdmi_known = true;
			state->hdmi_available = false;
			found = true;
			break;
		}
	}

	fclose(fp);

	if (!found) {
		state->hdmi_known = false;
		state->hdmi_available = false;
	}

	reconcile_selection(state);
}

static int open_display_watch(struct audio_state *state)
{
	int fd;

	if (mkdir(RUNTIME_DIR, 0755) < 0 && errno != EEXIST)
		return -1;

	fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0)
		return -1;

	if (inotify_add_watch(fd, RUNTIME_DIR,
			      IN_CLOSE_WRITE |
			      IN_MOVED_TO |
			      IN_CREATE |
			      IN_DELETE) < 0) {
		close(fd);
		return -1;
	}

	/*
	 * Add the watch before reading the initial state: if displayd replaces
	 * its atomic state file concurrently, the subsequent inotify event
	 * causes another refresh and no transition is lost.
	 */
	refresh_hdmi_state(state);

	return fd;
}

static bool drain_display_watch(int fd, struct audio_state *state)
{
	char buffer[4096]
		__attribute__((aligned(__alignof__(struct inotify_event))));
	bool refresh = false;

	for (;;) {
		ssize_t n;
		size_t offset;

		n = read(fd, buffer, sizeof(buffer));

		if (n < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			state->hdmi_known = false;
			state->hdmi_available = false;
			return false;
		}

		if (n == 0) {
			state->hdmi_known = false;
			state->hdmi_available = false;
			return false;
		}

		for (offset = 0; offset < (size_t)n; ) {
			const struct inotify_event *event =
				(const struct inotify_event *)(buffer + offset);

			if (event->mask & IN_IGNORED)
				return false;

			if (event->len > 0 &&
			    strcmp(event->name, "displayd.state") == 0)
				refresh = true;

			offset += sizeof(*event) + event->len;
		}
	}

	if (refresh)
		refresh_hdmi_state(state);

	return true;
}


static bool dbus_variant_get_string(DBusMessageIter *variant,
				    const char **value)
{
	DBusMessageIter inner;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
		return false;

	dbus_message_iter_recurse(variant, &inner);

	if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_STRING)
		return false;

	dbus_message_iter_get_basic(&inner, value);
	return true;
}

static bool dbus_variant_get_object_path(DBusMessageIter *variant,
					 const char **value)
{
	DBusMessageIter inner;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_VARIANT)
		return false;

	dbus_message_iter_recurse(variant, &inner);

	if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_OBJECT_PATH)
		return false;

	dbus_message_iter_get_basic(&inner, value);
	return true;
}

static bool bluealsa_pcm_is_a2dp_sink(DBusMessageIter *iface_entry,
				       const char **device_path)
{
	DBusMessageIter entry;
	DBusMessageIter props;
	const char *iface;
	const char *transport = NULL;
	const char *mode = NULL;
	const char *device = NULL;

	if (dbus_message_iter_get_arg_type(iface_entry) != DBUS_TYPE_DICT_ENTRY)
		return false;

	dbus_message_iter_recurse(iface_entry, &entry);

	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
		return false;

	dbus_message_iter_get_basic(&entry, &iface);

	if (strcmp(iface, BLUEALSA_PCM_INTERFACE) != 0)
		return false;

	if (!dbus_message_iter_next(&entry))
		return false;

	if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_ARRAY)
		return false;

	dbus_message_iter_recurse(&entry, &props);

	for (; dbus_message_iter_get_arg_type(&props) != DBUS_TYPE_INVALID;
	     dbus_message_iter_next(&props)) {
		DBusMessageIter prop_entry;
		const char *key;

		if (dbus_message_iter_get_arg_type(&props) != DBUS_TYPE_DICT_ENTRY)
			continue;

		dbus_message_iter_recurse(&props, &prop_entry);

		if (dbus_message_iter_get_arg_type(&prop_entry) != DBUS_TYPE_STRING)
			continue;

		dbus_message_iter_get_basic(&prop_entry, &key);

		if (!dbus_message_iter_next(&prop_entry))
			continue;

		if (strcmp(key, "Transport") == 0) {
			(void)dbus_variant_get_string(&prop_entry, &transport);
		} else if (strcmp(key, "Mode") == 0) {
			(void)dbus_variant_get_string(&prop_entry, &mode);
		} else if (strcmp(key, "Device") == 0) {
			(void)dbus_variant_get_object_path(&prop_entry, &device);
		}
	}

	if (device_path != NULL)
		*device_path = device;

	return transport != NULL &&
	       mode != NULL &&
	       device != NULL &&
	       strcmp(transport, "A2DP-source") == 0 &&
	       strcmp(mode, "sink") == 0;
}

static int set_bluealsa_pcm_volume(DBusConnection *conn,
				    const char *pcm_path,
				    int percent)
{
	DBusMessage *msg = NULL;
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	DBusMessageIter variant;
	DBusError error = DBUS_ERROR_INIT;
	const char *iface = BLUEALSA_PCM_INTERFACE;
	const char *property = "Volume";
	uint8_t channel;
	uint16_t packed;
	int level;

	if (conn == NULL || pcm_path == NULL || pcm_path[0] == '\0')
		return -1;

	if (percent < 0 || percent > 100)
		return -1;

	/*
	 * BlueALSA A2DP Volume uses 7 bits per channel:
	 *
	 *   0..127 = level
	 *   bit 7  = mute
	 *
	 * PCM1.Volume is a uint16 with:
	 *
	 *   high byte = channel 1
	 *   low byte  = channel 2
	 */
	level = (percent * 127 + 50) / 100;

	channel = (uint8_t)level;

	if (percent == 0)
		channel |= 0x80;

	packed = ((uint16_t)channel << 8) | channel;

	msg = dbus_message_new_method_call(
		BLUEALSA_SERVICE,
		pcm_path,
		DBUS_INTERFACE_PROPERTIES,
		"Set");

	if (msg == NULL)
		return -1;

	dbus_message_iter_init_append(msg, &iter);

	if (!dbus_message_iter_append_basic(
		    &iter, DBUS_TYPE_STRING, &iface))
		goto fail;

	if (!dbus_message_iter_append_basic(
		    &iter, DBUS_TYPE_STRING, &property))
		goto fail;

	if (!dbus_message_iter_open_container(
		    &iter,
		    DBUS_TYPE_VARIANT,
		    DBUS_TYPE_UINT16_AS_STRING,
		    &variant))
		goto fail;

	if (!dbus_message_iter_append_basic(
		    &variant, DBUS_TYPE_UINT16, &packed))
		goto fail;

	if (!dbus_message_iter_close_container(&iter, &variant))
		goto fail;

	reply = dbus_connection_send_with_reply_and_block(
		conn, msg, 1000, &error);

	dbus_message_unref(msg);

	if (reply == NULL) {
		if (dbus_error_is_set(&error))
			dbus_error_free(&error);
		return -1;
	}

	dbus_message_unref(reply);

	if (dbus_error_is_set(&error))
		dbus_error_free(&error);

	return 0;

fail:
	dbus_message_unref(msg);

	if (dbus_error_is_set(&error))
		dbus_error_free(&error);

	return -1;
}

static void apply_selected_bluetooth_volume(
	DBusConnection *conn,
	const struct audio_state *state)
{
	if (strcmp(state->selected, "bluetooth") != 0)
		return;

	if (!state->bluetooth_available)
		return;

	if (state->bluetooth_pcm_path[0] == '\0')
		return;

	if (set_bluealsa_pcm_volume(
		    conn,
		    state->bluetooth_pcm_path,
		    state->volume_bluetooth) < 0)
		fprintf(stderr,
			"nuubos-audiod: failed to set Bluetooth volume\n");
}

static void refresh_bluetooth_state(DBusConnection *conn,
				    struct audio_state *state)
{
	DBusMessage *msg = NULL;
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	DBusMessageIter objects;
	DBusError error = DBUS_ERROR_INIT;
	bool available = false;

	state->bluetooth_available = false;
	state->bluetooth_pcm_path[0] = '\0';
	state->bluetooth_device_path[0] = '\0';

	if (!conn) {
		reconcile_selection(state);
		return;
	}

	msg = dbus_message_new_method_call(BLUEALSA_SERVICE,
					   BLUEALSA_PATH,
					   DBUS_OBJECT_MANAGER,
					   "GetManagedObjects");
	if (!msg) {
		reconcile_selection(state);
		return;
	}

	reply = dbus_connection_send_with_reply_and_block(
		conn, msg, 1000, &error);

	dbus_message_unref(msg);

	if (!reply) {
		if (dbus_error_is_set(&error))
			dbus_error_free(&error);

		reconcile_selection(state);
		return;
	}

	if (!dbus_message_iter_init(reply, &iter))
		goto out;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		goto out;

	dbus_message_iter_recurse(&iter, &objects);

	for (; dbus_message_iter_get_arg_type(&objects) != DBUS_TYPE_INVALID;
	     dbus_message_iter_next(&objects)) {
		DBusMessageIter object_entry;
		DBusMessageIter ifaces;
		const char *object_path = NULL;

		if (dbus_message_iter_get_arg_type(&objects) != DBUS_TYPE_DICT_ENTRY)
			continue;

		dbus_message_iter_recurse(&objects, &object_entry);

		if (dbus_message_iter_get_arg_type(&object_entry) != DBUS_TYPE_OBJECT_PATH)
			continue;

		dbus_message_iter_get_basic(&object_entry, &object_path);

		if (!dbus_message_iter_next(&object_entry))
			continue;

		if (dbus_message_iter_get_arg_type(&object_entry) != DBUS_TYPE_ARRAY)
			continue;

		dbus_message_iter_recurse(&object_entry, &ifaces);

		for (; dbus_message_iter_get_arg_type(&ifaces) != DBUS_TYPE_INVALID;
		     dbus_message_iter_next(&ifaces)) {
			const char *device_path = NULL;

			if (bluealsa_pcm_is_a2dp_sink(&ifaces,
						     &device_path)) {
				available = true;

				if (object_path != NULL)
					(void)copy_string(
						state->bluetooth_pcm_path,
						sizeof(state->bluetooth_pcm_path),
						object_path);

				if (device_path != NULL)
					(void)copy_string(
						state->bluetooth_device_path,
						sizeof(state->bluetooth_device_path),
						device_path);

				break;
			}
		}

		if (available)
			break;
	}

out:
	state->bluetooth_available = available;

	if (!available) {
		state->bluetooth_pcm_path[0] = '\0';
		state->bluetooth_device_path[0] = '\0';
	}

	reconcile_selection(state);
	apply_selected_bluetooth_volume(conn, state);

	dbus_message_unref(reply);

	if (dbus_error_is_set(&error))
		dbus_error_free(&error);
}

static bool dbus_message_is_bluealsa_event(DBusMessage *message)
{
	const char *interface;
	const char *member;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return false;

	interface = dbus_message_get_interface(message);
	member = dbus_message_get_member(message);

	if (!interface || !member)
		return false;

	if (strcmp(interface, DBUS_OBJECT_MANAGER) == 0 &&
	    (strcmp(member, "InterfacesAdded") == 0 ||
	     strcmp(member, "InterfacesRemoved") == 0))
		return true;

	if (strcmp(interface, DBUS_INTERFACE_DBUS) == 0 &&
	    strcmp(member, "NameOwnerChanged") == 0) {
		DBusMessageIter iter;
		const char *name = NULL;

		if (!dbus_message_iter_init(message, &iter))
			return false;

		if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
			return false;

		dbus_message_iter_get_basic(&iter, &name);

		return name && strcmp(name, BLUEALSA_SERVICE) == 0;
	}

	return false;
}

static void drain_dbus(DBusConnection *conn, struct audio_state *state)
{
	DBusMessage *message;
	bool refresh = false;

	if (!conn)
		return;

	dbus_connection_read_write(conn, 0);

	while ((message = dbus_connection_pop_message(conn)) != NULL) {
		if (dbus_message_is_bluealsa_event(message))
			refresh = true;

		dbus_message_unref(message);
	}

	if (refresh)
		refresh_bluetooth_state(conn, state);
}

static DBusConnection *open_system_bus(struct audio_state *state)
{
	DBusConnection *conn;
	DBusError error = DBUS_ERROR_INIT;

	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (!conn) {
		if (dbus_error_is_set(&error))
			dbus_error_free(&error);
		return NULL;
	}

	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	dbus_bus_add_match(
		conn,
		"type='signal',"
		"sender='org.bluealsa',"
		"interface='org.freedesktop.DBus.ObjectManager',"
		"member='InterfacesAdded',"
		"path_namespace='/org/bluealsa'",
		&error);

	if (dbus_error_is_set(&error))
		goto fail;

	dbus_bus_add_match(
		conn,
		"type='signal',"
		"sender='org.bluealsa',"
		"interface='org.freedesktop.DBus.ObjectManager',"
		"member='InterfacesRemoved',"
		"path_namespace='/org/bluealsa'",
		&error);

	if (dbus_error_is_set(&error))
		goto fail;

	dbus_bus_add_match(
		conn,
		"type='signal',"
		"sender='org.freedesktop.DBus',"
		"interface='org.freedesktop.DBus',"
		"member='NameOwnerChanged',"
		"arg0='org.bluealsa'",
		&error);

	if (dbus_error_is_set(&error))
		goto fail;

	dbus_connection_flush(conn);

	refresh_bluetooth_state(conn, state);

	return conn;

fail:
	if (dbus_error_is_set(&error))
		dbus_error_free(&error);

	dbus_connection_unref(conn);
	return NULL;
}

static int create_socket(void)
{
	struct sockaddr_un addr;
	int fd;

	if (mkdir("/run/nuubos", 0755) < 0 && errno != EEXIST)
		return -1;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);

	unlink(SOCKET_PATH);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	if (chmod(SOCKET_PATH, 0660) < 0) {
		close(fd);
		unlink(SOCKET_PATH);
		return -1;
	}

	if (listen(fd, 8) < 0) {
		close(fd);
		unlink(SOCKET_PATH);
		return -1;
	}

	return fd;
}

int main(int argc, char **argv)
{
	struct audio_state state;
	DBusConnection *dbus_conn;
	int dbus_fd = -1;
	int display_watch_fd;
	int jack_fd;
	int server;

	if (argc == 2 && strcmp(argv[1], "--supported") == 0)
		return 0;

	if (argc != 1) {
		fprintf(stderr, "Usage: %s [--supported]\n", argv[0]);
		return 2;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	load_config(&state);
	jack_fd = open_headphone_jack(&state);
	display_watch_fd = open_display_watch(&state);

	dbus_conn = open_system_bus(&state);
	if (dbus_conn != NULL &&
	    !dbus_connection_get_unix_fd(dbus_conn, &dbus_fd))
		dbus_fd = -1;

	reconcile_selection(&state);

	server = create_socket();
	if (server < 0) {
		fprintf(stderr,
			"nuubos-audiod: cannot create socket: %s\n",
			strerror(errno));

		if (jack_fd >= 0)
			close(jack_fd);

		if (display_watch_fd >= 0)
			close(display_watch_fd);

		if (dbus_conn != NULL)
			dbus_connection_unref(dbus_conn);

		return 1;
	}

	while (!stop_requested) {
		struct pollfd fds[4];
		nfds_t nfds = 1;
		int jack_index = -1;
		int display_index = -1;
		int dbus_index = -1;
		int rc;

		memset(fds, 0, sizeof(fds));

		fds[0].fd = server;
		fds[0].events = POLLIN;

		if (jack_fd >= 0) {
			jack_index = (int)nfds;
			fds[nfds].fd = jack_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (display_watch_fd >= 0) {
			display_index = (int)nfds;
			fds[nfds].fd = display_watch_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (dbus_fd >= 0) {
			dbus_index = (int)nfds;
			fds[nfds].fd = dbus_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		rc = poll(fds, nfds, -1);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (jack_index >= 0 &&
		    fds[jack_index].revents != 0) {
			bool alive = true;

			if (fds[jack_index].revents & POLLIN)
				alive = drain_headphone_jack(jack_fd, &state);

			if (!alive ||
			    (fds[jack_index].revents &
			     (POLLERR | POLLHUP | POLLNVAL))) {
				close(jack_fd);
				jack_fd = -1;
				state.headphones_known = false;
				state.headphones_available = false;
			}
		}

		if (display_index >= 0 &&
		    fds[display_index].revents != 0) {
			bool alive = true;

			if (fds[display_index].revents & POLLIN)
				alive = drain_display_watch(display_watch_fd,
							    &state);

			if (!alive ||
			    (fds[display_index].revents &
			     (POLLERR | POLLHUP | POLLNVAL))) {
				close(display_watch_fd);
				display_watch_fd = -1;
				state.hdmi_known = false;
				state.hdmi_available = false;
			}
		}

		if (dbus_index >= 0 &&
		    fds[dbus_index].revents != 0) {
			if (fds[dbus_index].revents & POLLIN)
				drain_dbus(dbus_conn, &state);

			if (fds[dbus_index].revents &
			    (POLLERR | POLLHUP | POLLNVAL)) {
				state.bluetooth_available = false;
				dbus_fd = -1;
			}
		}

		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			break;

		if (fds[0].revents & POLLIN) {
			char command[256];
			char reply[1024];
			int client;
			ssize_t n;

			client = accept(server, NULL, NULL);
			if (client < 0) {
				if (errno == EINTR)
					continue;
				break;
			}

			n = read(client, command, sizeof(command) - 1);
			if (n > 0) {
				command[n] = '\0';

				while (n > 0 &&
				       (command[n - 1] == '\n' ||
					command[n - 1] == '\r')) {
					command[--n] = '\0';
				}

				handle_command(&state, dbus_conn,
					       command,
					       reply, sizeof(reply));
				(void)write_reply(client, reply);
			}

			close(client);
		}
	}

	if (jack_fd >= 0)
		close(jack_fd);

	if (display_watch_fd >= 0)
		close(display_watch_fd);

	if (dbus_conn != NULL)
		dbus_connection_unref(dbus_conn);

	close(server);
	unlink(SOCKET_PATH);

	return 0;
}
