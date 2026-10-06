/* SPDX-License-Identifier: MIT */

#include <alsa/asoundlib.h>
#include <ctype.h>
#include <dirent.h>
#include <dbus/dbus.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/un.h>
#include <unistd.h>

#include <nuubos/notify.h>
#include <pipewire/pipewire.h>

#define SOCKET_PATH "/run/nuubos/audiod.sock"
/* Socket clients that sent SUBSCRIBE (Quick Menu volume OSD). */
#define MAX_SUBSCRIBERS 4
#define CONFIG_PATH "/state/config/nuubos.conf"
#define CONFIG_TMP  "/state/config/.nuubos.conf.audio.tmp"
#define INPUT_DIR   "/dev/input"
#define DRM_CLASS_DIR "/sys/class/drm"
#define TEST_SOUND_PATH "/usr/share/nuubos/audio/nuubos-test-jingle.wav"
#define SYSTEM_SOUND_DIR "/usr/share/nuubos/audio/system"
#define HOME_MUSIC_DIR "/run/nuubos/userdata/music"
#define DEFAULT_HOME_MUSIC_DIR "/usr/share/nuubos/home-music"
#define WAV_PLAYER "/usr/bin/nuubos-audio-wav-player"
#define MP3_PLAYER "/usr/bin/nuubos-audio-mp3-player"
#define PW_ROUTE_HELPER "/usr/bin/nuubos-pw-route"
#define PW_APP_VOLUME_HELPER "/usr/bin/nuubos-pw-app-volume"
#define WPCTL_PATH "/usr/bin/wpctl"
#define PIPEWIRE_RUNTIME "/run/nuubos/pipewire"
#define VOLUME_PERSIST_DEBOUNCE_MS 400ULL

/* Product Audio defaults. They apply when a key is missing from STATE and
 * on Reset System Settings; nothing else may carry its own copy. Analog
 * and Bluetooth masters start below full scale to protect hearing on
 * headphones and Bluetooth absolute-volume sinks. */
#define DEFAULT_OUTPUT_MODE "auto"
#define DEFAULT_VOLUME_SPEAKER 70
#define DEFAULT_VOLUME_HEADPHONES 50
#define DEFAULT_VOLUME_BLUETOOTH 50
#define DEFAULT_VOLUME_SYSTEM 60
#define DEFAULT_VOLUME_HOME_MUSIC 60
#define DEFAULT_VOLUME_APPLICATIONS 100
#define DEFAULT_NAVIGATION_SOUNDS true
#define DEFAULT_POWER_SOUNDS true

#define AUDIO_SERVICE_NAME "org.nuubOS.Audio"
#define AUDIO_OBJECT_PATH "/org/nuubOS/Audio"
#define AUDIO_INTERFACE "org.nuubOS.Audio1"
#define INTROSPECT_INTERFACE "org.freedesktop.DBus.Introspectable"

#define BLUETOOTH_SERVICE "org.nuubOS.Bluetooth"
#define BLUETOOTH_PATH "/org/nuubOS/Bluetooth"
#define BLUETOOTH_INTERFACE "org.nuubOS.Bluetooth1"
#define BLUEZ_TRANSPORT_INTERFACE "org.bluez.MediaTransport1"

#define NUUBOS_BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define NUUBOS_NBITS(n) \
	(((n) + NUUBOS_BITS_PER_LONG - 1U) / NUUBOS_BITS_PER_LONG)

struct audio_state {
	char mode[16];
	char selected[16];
	/* Route whose default sink + master volume were last applied to
	 * PipeWire; empty until an apply succeeds. */
	char applied_route[16];

	int volume_speaker;
	int volume_headphones;
	int volume_bluetooth;
	int volume_system;
	int volume_home_music;
	int volume_applications;
	bool navigation_sounds_enabled;
	bool power_sounds_enabled;

	pid_t test_pid;
	/* Persistent system-sound player (wav-player "sfx-server") and the
	 * write end of its command pipe. */
	pid_t system_pid;
	int system_ctl_fd;
	pid_t music_pid;
	/* Write end of the running Home Music player control pipe. */
	int music_ctl_fd;
	pid_t ui_anchor_pid;
	pid_t app_volume_pid;
	bool test_requested;
	bool music_requested;
	bool home_session_active;
	char system_file[256];
	char music_file[512];
	unsigned long long test_offset_ms;
	unsigned long long music_offset_ms;
	struct timespec test_started;
	struct timespec music_started;

	bool headphones_known;
	bool headphones_available;

	bool hdmi_known;
	bool hdmi_available;

	bool bluetooth_available;

	bool volume_config_dirty;
	struct timespec volume_config_due;
	bool pipewire_volume_dirty;
	struct timespec pipewire_volume_due;

	int subscribers[MAX_SUBSCRIBERS];
	size_t subscriber_count;
};

static void emit_state_changed(DBusConnection *conn,
			       const struct audio_state *state);
static void reroute_owned_streams(struct audio_state *state);
static void service_owned_streams(struct audio_state *state);
static bool start_music_new(struct audio_state *state);
static void stop_music(struct audio_state *state);
static void stop_child(pid_t *pid);
static void stop_system_sound(struct audio_state *state);
static void stop_test(struct audio_state *state);
static void ensure_ui_anchor(struct audio_state *state);
static void restart_ui_anchor(struct audio_state *state);
static int update_config(const char *key, const char *value);
static int selected_volume(const struct audio_state *state);


static volatile sig_atomic_t stop_requested;


static const char audio_introspection_xml[] =
	"<node>"
	"<interface name='org.nuubOS.Audio1'>"
	"<method name='GetSnapshot'>"
	"<arg name='mode' type='s' direction='out'/>"
	"<arg name='selected' type='s' direction='out'/>"
	"<arg name='volume' type='i' direction='out'/>"
	"<arg name='volume_supported' type='b' direction='out'/>"
	"<arg name='bluetooth_available' type='b' direction='out'/>"
	"<arg name='headphones_available' type='b' direction='out'/>"
	"<arg name='hdmi_available' type='b' direction='out'/>"
	"<arg name='speaker_available' type='b' direction='out'/>"
	"<arg name='system_volume' type='i' direction='out'/>"
	"<arg name='home_music_volume' type='i' direction='out'/>"
	"<arg name='home_music_playing' type='b' direction='out'/>"
	"</method>"
	"<method name='SetOutput'>"
	"<arg name='output' type='s' direction='in'/>"
	"</method>"
	"<method name='SetVolume'>"
	"<arg name='volume' type='i' direction='in'/>"
	"</method>"
	"<method name='AdjustVolume'>"
	"<arg name='delta' type='i' direction='in'/>"
	"</method>"
	"<method name='StartHomeMusic'/>"
	"<method name='StopHomeMusic'/>"
	"<signal name='StateChanged'>"
	"<arg name='mode' type='s'/>"
	"<arg name='selected' type='s'/>"
	"<arg name='volume' type='i'/>"
	"<arg name='volume_supported' type='b'/>"
	"<arg name='bluetooth_available' type='b'/>"
	"<arg name='headphones_available' type='b'/>"
	"<arg name='hdmi_available' type='b'/>"
	"<arg name='speaker_available' type='b'/>"
	"<arg name='system_volume' type='i'/>"
	"<arg name='home_music_volume' type='i'/>"
	"<arg name='home_music_playing' type='b'/>"
	"</signal>"
	"</interface>"
	"<interface name='org.freedesktop.DBus.Introspectable'>"
	"<method name='Introspect'>"
	"<arg name='xml_data' type='s' direction='out'/>"
	"</method>"
	"</interface>"
	"</node>";


static void handle_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

/* Self-pipe woken by SIGCHLD: player/helper exits are handled as events, so
 * the main loop sleeps without a timeout when nothing is pending. */
static int sigchld_pipe[2] = { -1, -1 };

static void handle_sigchld(int sig)
{
	int saved_errno = errno;

	(void)sig;
	if (sigchld_pipe[1] >= 0)
		if (write(sigchld_pipe[1], "c", 1) < 0) {
			/* Pipe full: a wakeup is already pending. */
		}
	errno = saved_errno;
}

static int open_sigchld_pipe(void)
{
	struct sigaction sa;

	if (pipe(sigchld_pipe) < 0)
		return -1;
	for (int i = 0; i < 2; i++) {
		(void)fcntl(sigchld_pipe[i], F_SETFL, O_NONBLOCK);
		(void)fcntl(sigchld_pipe[i], F_SETFD, FD_CLOEXEC);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGCHLD, &sa, NULL) < 0)
		return -1;
	return sigchld_pipe[0];
}

static void drain_sigchld_pipe(int fd)
{
	char buf[64];

	while (read(fd, buf, sizeof(buf)) > 0)
		;
}

static bool valid_output(const char *output)
{
	return strcmp(output, "auto") == 0 ||
	       strcmp(output, "bluetooth") == 0 ||
	       strcmp(output, "analog") == 0 ||
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

static int apply_pipewire_volume(const char *selected, int volume)
{
	char value[16];
	pid_t pid;
	int status;

	if (selected == NULL || volume < 0 || volume > 100)
		return -1;

	if (strcmp(selected, "speaker") != 0 &&
	    strcmp(selected, "headphones") != 0 &&
	    strcmp(selected, "bluetooth") != 0)
		return 0;

	if (access(WPCTL_PATH, X_OK) != 0)
		return -1;

	/* Avoid the old shell helper + awk + wpctl chain for every 1% tick.
	 * Product Audio already owns the selected default sink, so one direct
	 * wpctl transaction is sufficient. */
	snprintf(value, sizeof(value), "%d%%", volume);
	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		(void)setenv("XDG_RUNTIME_DIR", PIPEWIRE_RUNTIME, 1);
		(void)setenv("PIPEWIRE_RUNTIME_DIR", PIPEWIRE_RUNTIME, 1);
		execl(WPCTL_PATH, WPCTL_PATH,
		      "set-volume", "@DEFAULT_AUDIO_SINK@", value,
		      (char *)NULL);
		_exit(127);
	}

	/* wpctl normally completes in a few milliseconds. Poll at 1 ms instead
	 * of the old 10 ms granularity while retaining a bounded timeout. */
	for (int i = 0; i < 100; i++) {
		pid_t rc = waitpid(pid, &status, WNOHANG);

		if (rc == pid)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
		if (rc < 0)
			return -1;
		usleep(1000);
	}

	(void)kill(pid, SIGKILL);
	(void)waitpid(pid, &status, 0);
	return -1;
}

/* Aligning the Applications gain needs several wpctl round trips (~0.3 s on
 * H700). Run it in the background so the control socket never blocks on it;
 * a newer request supersedes a running one, and the child is reaped by
 * service_owned_streams(). */
static int run_applications_volume_helper(struct audio_state *state, int volume)
{
	char value[16];
	pid_t pid;
	int status;

	if (volume < 0 || volume > 100 ||
	    access(PW_APP_VOLUME_HELPER, X_OK) != 0)
		return -1;

	if (state->app_volume_pid > 0) {
		(void)kill(state->app_volume_pid, SIGTERM);
		(void)waitpid(state->app_volume_pid, &status, 0);
		state->app_volume_pid = -1;
	}

	snprintf(value, sizeof(value), "%d", volume);
	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		execl(PW_APP_VOLUME_HELPER, PW_APP_VOLUME_HELPER,
		      value, (char *)NULL);
		_exit(127);
	}

	state->app_volume_pid = pid;
	return 0;
}

static int persist_applications_volume(struct audio_state *state,
				       DBusConnection *conn,
				       int volume)
{
	char value[16];

	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;

	snprintf(value, sizeof(value), "%d", volume);
	if (update_config("AUDIO_VOLUME_APPLICATIONS", value) != 0)
		return -1;

	state->volume_applications = volume;
	if (run_applications_volume_helper(state, volume) != 0)
		fprintf(stderr, "nuubos-audiod: failed to apply Applications volume policy\n");
	emit_state_changed(conn, state);
	return 0;
}

static int persist_sound_toggle(struct audio_state *state,
				DBusConnection *conn,
				bool navigation,
				bool enabled)
{
	const char *key = navigation ? "AUDIO_SYSTEM_NAVIGATION_ENABLED" :
				       "AUDIO_SYSTEM_POWER_ENABLED";

	if (update_config(key, enabled ? "1" : "0") != 0)
		return -1;

	if (navigation)
		state->navigation_sounds_enabled = enabled;
	else
		state->power_sounds_enabled = enabled;

	emit_state_changed(conn, state);
	return 0;
}

static bool system_sound_allowed(const struct audio_state *state,
				 const char *name)
{
	if (state->volume_system <= 0)
		return false;

	if (strcmp(name, "boot") == 0 ||
	    strcmp(name, "restart") == 0 ||
	    strcmp(name, "poweroff") == 0)
		return state->power_sounds_enabled;

	if (strcmp(name, "select") == 0 ||
	    strcmp(name, "back") == 0 ||
	    strcmp(name, "navigation") == 0 ||
	    strcmp(name, "quick-settings") == 0)
		return state->navigation_sounds_enabled;

	return false;
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

	if (apply_pipewire_volume(state->selected, volume) < 0)
		fprintf(stderr,
			"nuubos-audiod: failed to set PipeWire analog volume\n");
}

static int apply_pipewire_default(const char *selected)
{
	pid_t pid;
	int status;

	/* Every physical output, including Bluetooth, is a PipeWire target. */
	if (selected == NULL)
		return 0;

	if (access(PW_ROUTE_HELPER, X_OK) != 0)
		return -1;

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		execl(PW_ROUTE_HELPER, PW_ROUTE_HELPER,
		      "select", selected, (char *)NULL);
		_exit(127);
	}

	/* Never let a policy helper stall audiod (and therefore the overlay).
	 * It takes ~300 ms idle on H700 and longer while WirePlumber is busy
	 * creating a just-connected Bluetooth sink. */
	for (int i = 0; i < 150; i++) {
		pid_t rc = waitpid(pid, &status, WNOHANG);

		if (rc == pid) {
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				return -1;
			return 0;
		}

		if (rc < 0)
			return -1;

		usleep(10000);
	}

	(void)kill(pid, SIGKILL);
	(void)waitpid(pid, &status, 0);
	return -1;
}

static const char *fallback_route(const struct audio_state *state)
{
	if (state->headphones_known && state->headphones_available)
		return "headphones";

	if (state->hdmi_known && state->hdmi_available)
		return "hdmi";

	return "speaker";
}

static void reconcile_selection(struct audio_state *state)
{
	const char *selected;

	if (strcmp(state->mode, "analog") == 0) {
		/* Speaker vs headphones is a physical jack route. "analog"
		 * deliberately follows that hardware state. */
		selected = (state->headphones_known &&
			    state->headphones_available)
			? "headphones" : "speaker";
	} else if (strcmp(state->mode, "auto") == 0) {
		if (state->bluetooth_available)
			selected = "bluetooth";
		else
			selected = fallback_route(state);
	} else if (strcmp(state->mode, "bluetooth") == 0) {
		/* Keep the requested mode persisted, but never expose a dead
		 * selected route. Bluetooth resumes automatically on reconnect. */
		selected = state->bluetooth_available
			? "bluetooth" : fallback_route(state);
	} else if (strcmp(state->mode, "hdmi") == 0) {
		selected = (state->hdmi_known && state->hdmi_available)
			? "hdmi" : fallback_route(state);
	} else if (strcmp(state->mode, "headphones") == 0) {
		selected = (state->headphones_known &&
			    state->headphones_available)
			? "headphones" : "speaker";
	} else if (strcmp(state->mode, "speaker") == 0) {
		selected = "speaker";
	} else {
		selected = fallback_route(state);
	}

	(void)copy_string(state->selected,
			  sizeof(state->selected),
			  selected);

	/* Re-apply only when the route changes or the last apply failed (e.g.
	 * the Bluetooth sink appears in PipeWire after BlueZ reports the device
	 * connected). Unrelated Bluetooth events must not force the stored
	 * master back over a volume the headset changed itself (AVRCP absolute
	 * volume). */
	if (strcmp(state->applied_route, state->selected) == 0)
		return;
	state->applied_route[0] = '\0';

	if (apply_pipewire_default(state->selected) != 0) {
		fprintf(stderr,
			"nuubos-audiod: PipeWire default route sync failed for %s\n",
			state->selected);
		return;
	}
	if (strcmp(state->selected, "bluetooth") == 0) {
		if (apply_pipewire_volume("bluetooth", state->volume_bluetooth) < 0) {
			fprintf(stderr, "nuubos-audiod: failed to set Bluetooth PipeWire volume\n");
			return;
		}
	} else {
		apply_selected_analog_volume(state);
	}
	(void)copy_string(state->applied_route,
			  sizeof(state->applied_route),
			  state->selected);
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

	snprintf(state->mode, sizeof(state->mode), DEFAULT_OUTPUT_MODE);
	snprintf(state->selected, sizeof(state->selected), "unresolved");
	state->applied_route[0] = '\0';
	state->subscriber_count = 0;

	state->volume_speaker = DEFAULT_VOLUME_SPEAKER;
	state->volume_headphones = DEFAULT_VOLUME_HEADPHONES;
	state->volume_bluetooth = DEFAULT_VOLUME_BLUETOOTH;
	state->volume_system = DEFAULT_VOLUME_SYSTEM;
	state->volume_home_music = DEFAULT_VOLUME_HOME_MUSIC;
	state->volume_applications = DEFAULT_VOLUME_APPLICATIONS;
	state->navigation_sounds_enabled = DEFAULT_NAVIGATION_SOUNDS;
	state->power_sounds_enabled = DEFAULT_POWER_SOUNDS;
	state->test_pid = -1;
	state->system_pid = -1;
	state->system_ctl_fd = -1;
	state->music_pid = -1;
	state->music_ctl_fd = -1;
	state->ui_anchor_pid = -1;
	state->app_volume_pid = -1;
	state->test_requested = false;
	state->music_requested = false;
	state->home_session_active = false;
	state->system_file[0] = '\0';
	state->music_file[0] = '\0';
	state->test_offset_ms = 0;
	state->music_offset_ms = 0;

	state->headphones_known = false;
	state->headphones_available = false;

	state->hdmi_known = false;
	state->hdmi_available = false;

	state->bluetooth_available = false;
	state->volume_config_dirty = false;
	state->volume_config_due.tv_sec = 0;
	state->volume_config_due.tv_nsec = 0;
	state->pipewire_volume_dirty = false;
	state->pipewire_volume_due.tv_sec = 0;
	state->pipewire_volume_due.tv_nsec = 0;

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
		} else if (strncmp(line, "AUDIO_VOLUME_BLUETOOTH=", 23) == 0) {
			int v = parse_volume(line + 23);
			if (v >= 0)
				state->volume_bluetooth = v;
		} else if (strncmp(line, "AUDIO_VOLUME_SYSTEM=", 20) == 0) {
			int v = parse_volume(line + 20);
			if (v >= 0)
				state->volume_system = v;
		} else if (strncmp(line, "AUDIO_VOLUME_HOME_MUSIC=", 24) == 0) {
			int v = parse_volume(line + 24);
			if (v >= 0)
				state->volume_home_music = v;
		} else if (strncmp(line, "AUDIO_VOLUME_APPLICATIONS=", 26) == 0) {
			int v = parse_volume(line + 26);
			if (v >= 0)
				state->volume_applications = v;
		} else if (strncmp(line, "AUDIO_SYSTEM_NAVIGATION_ENABLED=", 32) == 0) {
			state->navigation_sounds_enabled = strcmp(line + 32, "0") != 0;
		} else if (strncmp(line, "AUDIO_SYSTEM_POWER_ENABLED=", 27) == 0) {
			state->power_sounds_enabled = strcmp(line + 27, "0") != 0;
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


static unsigned long long monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;

	return (unsigned long long)now.tv_sec * 1000ULL +
	       (unsigned long long)now.tv_nsec / 1000000ULL;
}

static void schedule_pipewire_volume_apply(struct audio_state *state)
{
	unsigned long long due = monotonic_ms() + 18ULL;

	state->pipewire_volume_dirty = true;
	state->pipewire_volume_due.tv_sec = (time_t)(due / 1000ULL);
	state->pipewire_volume_due.tv_nsec = (long)((due % 1000ULL) * 1000000ULL);
}

static bool pipewire_volume_apply_is_due(const struct audio_state *state)
{
	unsigned long long due;

	if (!state->pipewire_volume_dirty)
		return false;
	due = (unsigned long long)state->pipewire_volume_due.tv_sec * 1000ULL +
	      (unsigned long long)state->pipewire_volume_due.tv_nsec / 1000000ULL;
	return monotonic_ms() >= due;
}

static long long deadline_remaining_ms(const struct timespec *due)
{
	long long due_ms = (long long)due->tv_sec * 1000LL +
			   (long long)due->tv_nsec / 1000000LL;
	long long remaining = due_ms - (long long)monotonic_ms();

	return remaining > 0 ? remaining : 0;
}

/* Wait only until the next deferred volume apply/persist; otherwise forever. */
static int main_loop_timeout_ms(const struct audio_state *state)
{
	long long timeout = -1;
	long long remaining;

	if (state->pipewire_volume_dirty) {
		remaining = deadline_remaining_ms(&state->pipewire_volume_due);
		timeout = remaining;
	}
	if (state->volume_config_dirty) {
		remaining = deadline_remaining_ms(&state->volume_config_due);
		if (timeout < 0 || remaining < timeout)
			timeout = remaining;
	}
	return timeout > 60000 ? 60000 : (int)timeout;
}

static int flush_pipewire_volume(struct audio_state *state)
{
	int volume;

	if (!state->pipewire_volume_dirty)
		return 0;
	volume = selected_volume(state);
	if (!volume_supported(state->selected) || volume < 0) {
		state->pipewire_volume_dirty = false;
		return 0;
	}
	if (apply_pipewire_volume(state->selected, volume) < 0)
		return -1;
	state->pipewire_volume_dirty = false;
	return 0;
}

static void schedule_volume_persist(struct audio_state *state)
{
	unsigned long long due = monotonic_ms() + VOLUME_PERSIST_DEBOUNCE_MS;

	state->volume_config_dirty = true;
	state->volume_config_due.tv_sec = (time_t)(due / 1000ULL);
	state->volume_config_due.tv_nsec = (long)((due % 1000ULL) * 1000000ULL);
}

static bool volume_persist_is_due(const struct audio_state *state)
{
	unsigned long long due;

	if (!state->volume_config_dirty)
		return false;

	due = (unsigned long long)state->volume_config_due.tv_sec * 1000ULL +
	      (unsigned long long)state->volume_config_due.tv_nsec / 1000000ULL;
	return monotonic_ms() >= due;
}

static int flush_volume_config(struct audio_state *state)
{
	char value[16];

	if (!state->volume_config_dirty)
		return 0;

	snprintf(value, sizeof(value), "%d", state->volume_speaker);
	if (update_config("AUDIO_VOLUME_SPEAKER", value) != 0)
		return -1;

	snprintf(value, sizeof(value), "%d", state->volume_headphones);
	if (update_config("AUDIO_VOLUME_HEADPHONES", value) != 0)
		return -1;

	snprintf(value, sizeof(value), "%d", state->volume_bluetooth);
	if (update_config("AUDIO_VOLUME_BLUETOOTH", value) != 0)
		return -1;

	state->volume_config_dirty = false;
	return 0;
}

static int write_reply(int fd, const char *reply)
{
	size_t remaining = strlen(reply);
	const char *p = reply;

	while (remaining > 0) {
		ssize_t n = send(fd, p, remaining, MSG_NOSIGNAL);

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

static int selected_volume(const struct audio_state *state)
{
	if (!volume_supported(state->selected))
		return -1;
	return get_volume(state, state->selected);
}

static bool product_audio_master_enabled(const struct audio_state *state)
{
	int volume = selected_volume(state);

	/* HDMI volume is owned by the TV/receiver. For nuubOS-controlled
	 * outputs, 0% is a hard Product Audio mute: no owned stream or
	 * keepalive is scheduled. */
	return volume < 0 || volume > 0;
}

/* apply_sink is false when the sink already carries the new gain, i.e. the
 * Bluetooth headset changed its own absolute volume. */
static int set_selected_volume(struct audio_state *state,
			       DBusConnection *conn,
			       int volume,
			       bool apply_sink)
{
	int old_volume;

	if (!volume_supported(state->selected))
		return -2;

	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;

	old_volume = selected_volume(state);

	/* Interactive master changes are runtime operations. Persist them only
	 * after the user stops stepping the control, rather than fsync()ing
	 * STATE for every 1% event. */
	set_volume_memory(state, state->selected, volume);
	schedule_volume_persist(state);
	/* Coalesce repeated 1% key events. Spawning and synchronously waiting for
	 * wpctl on every tick caused OSD/input lag and CPU bursts large enough to
	 * underrun Home Music on H700. The Product state changes immediately; the
	 * final sink gain is applied once the short input burst settles. */
	if (apply_sink)
		schedule_pipewire_volume_apply(state);

	/* PipeWire owns analog gain. Ordinary 1% master changes must not
	 * restart playback processes: doing so caused an audible pop on every
	 * step. Only the hard-mute boundary changes stream scheduling. */
	if (volume == 0 && old_volume > 0) {
		stop_test(state);
		stop_system_sound(state);
		stop_music(state);
	} else if (volume > 0 && old_volume == 0 &&
		   state->music_requested && state->volume_home_music > 0) {
		state->music_file[0] = '\0';
		state->music_offset_ms = 0;
		(void)start_music_new(state);
	}

	emit_state_changed(conn, state);
	return 0;
}

static int persist_selected_volume(struct audio_state *state,
				   DBusConnection *conn,
				   int volume)
{
	return set_selected_volume(state, conn, volume, true);
}

static int persist_output_mode(struct audio_state *state,
			       DBusConnection *conn,
			       const char *output)
{
	char mode[32];
	char persistent[32];

	if (!copy_string(mode, sizeof(mode), output))
		return -1;

	lowercase(mode);
	if (!valid_output(mode))
		return -1;

	uppercase_copy(persistent, sizeof(persistent), mode);
	if (update_config("AUDIO_OUTPUT", persistent) != 0)
		return -1;

	if (!copy_string(state->mode, sizeof(state->mode), mode))
		return -1;

	reconcile_selection(state);
	reroute_owned_streams(state);
	emit_state_changed(conn, state);
	return 0;
}

static void append_state_args(DBusMessage *message,
			      const struct audio_state *state)
{
	const char *mode = state->mode;
	const char *selected = state->selected;
	dbus_int32_t volume = selected_volume(state);
	dbus_bool_t can_volume = volume_supported(state->selected);
	dbus_bool_t bluetooth = state->bluetooth_available;
	dbus_bool_t headphones = state->headphones_known &&
				 state->headphones_available;
	dbus_bool_t hdmi = state->hdmi_known && state->hdmi_available;
	dbus_bool_t speaker = TRUE;
	dbus_int32_t system_volume = state->volume_system;
	dbus_int32_t home_music_volume = state->volume_home_music;
	dbus_bool_t home_music_playing = state->music_pid > 0;

	dbus_message_append_args(message,
				 DBUS_TYPE_STRING, &mode,
				 DBUS_TYPE_STRING, &selected,
				 DBUS_TYPE_INT32, &volume,
				 DBUS_TYPE_BOOLEAN, &can_volume,
				 DBUS_TYPE_BOOLEAN, &bluetooth,
				 DBUS_TYPE_BOOLEAN, &headphones,
				 DBUS_TYPE_BOOLEAN, &hdmi,
				 DBUS_TYPE_BOOLEAN, &speaker,
				 DBUS_TYPE_INT32, &system_volume,
				 DBUS_TYPE_INT32, &home_music_volume,
				 DBUS_TYPE_BOOLEAN, &home_music_playing,
				 DBUS_TYPE_INVALID);
}

static void emit_state_changed(DBusConnection *conn,
			       const struct audio_state *state)
{
	DBusMessage *signal;

	if (!conn)
		return;

	signal = dbus_message_new_signal(AUDIO_OBJECT_PATH,
					 AUDIO_INTERFACE,
					 "StateChanged");
	if (!signal)
		return;

	append_state_args(signal, state);
	dbus_connection_send(conn, signal, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(signal);
}

static void send_dbus_reply(DBusConnection *conn, DBusMessage *reply)
{
	if (!reply)
		return;
	dbus_connection_send(conn, reply, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(reply);
}

static void send_dbus_error(DBusConnection *conn,
			    DBusMessage *request,
			    const char *name,
			    const char *message)
{
	send_dbus_reply(conn, dbus_message_new_error(request, name, message));
}

static bool handle_audio_method(DBusConnection *conn,
				DBusMessage *message,
				struct audio_state *state)
{
	if (dbus_message_is_method_call(message,
					INTROSPECT_INTERFACE,
					"Introspect")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		const char *xml = audio_introspection_xml;
		if (reply)
			dbus_message_append_args(reply,
						 DBUS_TYPE_STRING, &xml,
						 DBUS_TYPE_INVALID);
		send_dbus_reply(conn, reply);
		return true;
	}

	if (dbus_message_is_method_call(message,
					AUDIO_INTERFACE,
					"GetSnapshot")) {
		DBusMessage *reply = dbus_message_new_method_return(message);
		if (reply)
			append_state_args(reply, state);
		send_dbus_reply(conn, reply);
		return true;
	}

	if (dbus_message_is_method_call(message,
					AUDIO_INTERFACE,
					"SetOutput")) {
		DBusError error = DBUS_ERROR_INIT;
		const char *output = NULL;

		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_STRING, &output,
					   DBUS_TYPE_INVALID)) {
			send_dbus_error(conn, message,
					"org.nuubOS.Audio.Error.InvalidArgument",
					"SetOutput requires an output name");
			if (dbus_error_is_set(&error))
				dbus_error_free(&error);
			return true;
		}

		if (persist_output_mode(state, conn, output) != 0) {
			send_dbus_error(conn, message,
					"org.nuubOS.Audio.Error.InvalidOutput",
					"Audio output is invalid or could not be persisted");
			return true;
		}

		send_dbus_reply(conn, dbus_message_new_method_return(message));
		return true;
	}

	if (dbus_message_is_method_call(message,
					AUDIO_INTERFACE,
					"StartHomeMusic")) {
		state->home_session_active = true;
		state->music_requested = true;
		ensure_ui_anchor(state);

		if (state->volume_home_music > 0 &&
		    state->music_pid <= 0)
			(void)start_music_new(state);

		emit_state_changed(conn, state);
		send_dbus_reply(conn, dbus_message_new_method_return(message));
		return true;
	}

	if (dbus_message_is_method_call(message,
					AUDIO_INTERFACE,
					"StopHomeMusic")) {
		state->home_session_active = false;
		state->music_requested = false;
		stop_music(state);
		ensure_ui_anchor(state);
		emit_state_changed(conn, state);
		send_dbus_reply(conn, dbus_message_new_method_return(message));
		return true;
	}

	if (dbus_message_is_method_call(message,
					AUDIO_INTERFACE,
					"SetVolume") ||
	    dbus_message_is_method_call(message,
					AUDIO_INTERFACE,
					"AdjustVolume")) {
		DBusError error = DBUS_ERROR_INIT;
		dbus_int32_t value = 0;
		int volume;
		int rc;

		if (!dbus_message_get_args(message, &error,
					   DBUS_TYPE_INT32, &value,
					   DBUS_TYPE_INVALID)) {
			send_dbus_error(conn, message,
					"org.nuubOS.Audio.Error.InvalidArgument",
					"Volume method requires one int32 value");
			if (dbus_error_is_set(&error))
				dbus_error_free(&error);
			return true;
		}

		if (dbus_message_is_method_call(message,
						AUDIO_INTERFACE,
						"AdjustVolume")) {
			volume = selected_volume(state);
			if (volume < 0)
				rc = -2;
			else
				rc = persist_selected_volume(
					state, conn,
					(volume + (int)value < 0) ? 0 :
					(volume + (int)value > 100) ? 100 :
					volume + (int)value);
		} else {
			rc = persist_selected_volume(state, conn, (int)value);
		}

		if (rc == -2) {
			send_dbus_error(conn, message,
					"org.nuubOS.Audio.Error.VolumeUnsupported",
					"Active output does not support nuubOS volume control");
			return true;
		}
		if (rc != 0) {
			send_dbus_error(conn, message,
					"org.nuubOS.Audio.Error.Failed",
					"Volume update failed");
			return true;
		}

		send_dbus_reply(conn, dbus_message_new_method_return(message));
		return true;
	}

	return false;
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
		 "volume.hdmi=unsupported\n"
		 "volume.system=%d\n"
		 "volume.applications=%d\n"
		 "system.navigation_enabled=%d\n"
		 "system.power_enabled=%d\n"
		 "volume.home_music=%d\n"
		 "home_music.playing=%d\n"
		 "home_music.track=%s\n",
		 state->mode,
		 state->selected,
		 state->bluetooth_available ? 1 : 0,
		 headphones,
		 hdmi,
		 state->volume_speaker,
		 state->volume_headphones,
		 state->volume_bluetooth,
		 state->volume_system,
		 state->volume_applications,
		 state->navigation_sounds_enabled ? 1 : 0,
		 state->power_sounds_enabled ? 1 : 0,
		 state->volume_home_music,
		 state->music_pid > 0 ? 1 : 0,
		 state->music_file[0] != '\0' ? state->music_file : "");
}

static void route_reply(const struct audio_state *state,
			char *reply,
			size_t reply_size)
{
	if (strcmp(state->selected, "speaker") == 0) {
		snprintf(reply, reply_size,
			 "default\n");
		return;
	}

	if (strcmp(state->selected, "headphones") == 0) {
		if (!state->headphones_known ||
		    !state->headphones_available) {
			snprintf(reply, reply_size, "unavailable\n");
			return;
		}

		snprintf(reply, reply_size,
			 "default\n");
		return;
	}

	if (strcmp(state->selected, "hdmi") == 0) {
		if (!state->hdmi_known ||
		    !state->hdmi_available) {
			snprintf(reply, reply_size, "unavailable\n");
			return;
		}

		snprintf(reply, reply_size,
			 "default\n");
		return;
	}

	if (strcmp(state->selected, "bluetooth") == 0) {
		if (!state->bluetooth_available) {
			snprintf(reply, reply_size, "unavailable\n");
			return;
		}
		snprintf(reply, reply_size, "default\n");
		return;
	}

	snprintf(reply, reply_size, "unavailable\n");
}

static unsigned long long elapsed_ms(const struct timespec *started)
{
	unsigned long long start =
		(unsigned long long)started->tv_sec * 1000ULL +
		(unsigned long long)started->tv_nsec / 1000000ULL;
	unsigned long long now = monotonic_ms();

	return now > start ? now - start : 0;
}

static void stop_child(pid_t *pid)
{
	int status;
	int i;

	if (*pid <= 0)
		return;

	(void)kill(*pid, SIGTERM);

	for (i = 0; i < 10; i++) {
		pid_t rc = waitpid(*pid, &status, WNOHANG);

		if (rc == *pid || rc < 0) {
			*pid = -1;
			return;
		}

		usleep(10000);
	}

	(void)kill(*pid, SIGKILL);
	(void)waitpid(*pid, &status, 0);
	*pid = -1;
}

static int current_route(const struct audio_state *state,
			 char *route, size_t size)
{
	route_reply(state, route, size);
	route[strcspn(route, "\r\n")] = '\0';

	if (strcmp(route, "unavailable") == 0 || route[0] == '\0')
		return -1;

	return 0;
}

static pid_t launch_wav(const struct audio_state *state,
			const char *path,
			int volume,
			unsigned long long skip_ms,
			const char *profile,
			int *ctl_fd)
{
	char route[128];
	char vol[16];
	char skip[32];
	int ctl[2] = { -1, -1 };
	pid_t pid;

	if (volume <= 0 || path == NULL || path[0] == '\0' ||
	    !product_audio_master_enabled(state))
		return -1;

	if (current_route(state, route, sizeof(route)) != 0)
		return -1;

	snprintf(vol, sizeof(vol), "%d", volume);
	snprintf(skip, sizeof(skip), "%llu", skip_ms);

	/* Optional command pipe on the player's stdin (sfx-server). */
	if (ctl_fd != NULL && pipe2(ctl, O_CLOEXEC) != 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		if (ctl[0] >= 0) close(ctl[0]);
		if (ctl[1] >= 0) close(ctl[1]);
		return -1;
	}

	if (pid == 0) {
		if (ctl[0] >= 0)
			(void)dup2(ctl[0], STDIN_FILENO);
		execl(WAV_PLAYER, WAV_PLAYER,
		      route, vol, skip,
		      profile != NULL ? profile : "stream",
		      path, (char *)NULL);
		_exit(127);
	}

	if (ctl[0] >= 0)
		close(ctl[0]);
	if (ctl_fd != NULL) {
		(void)fcntl(ctl[1], F_SETFL, O_NONBLOCK);
		*ctl_fd = ctl[1];
	}
	return pid;
}

static void ensure_ui_anchor(struct audio_state *state)
{
	/* PipeWire owns the shared graph. Never schedule a fake silence stream. */
	stop_child(&state->ui_anchor_pid);
}

static void restart_ui_anchor(struct audio_state *state)
{
	ensure_ui_anchor(state);
}


static bool mp3_name(const char *name)
{
	size_t len;
	if (name == NULL) return false;
	len = strlen(name);
	return len > 4 && strcasecmp(name + len - 4, ".mp3") == 0;
}

static pid_t launch_mp3(const struct audio_state *state,
			const char *path, int volume,
			unsigned long long skip_ms, const char *profile,
			int *ctl_fd)
{
	char route[128], vol[16], skip[32];
	int ctl[2] = { -1, -1 };
	pid_t pid;
	if (volume <= 0 || path == NULL || path[0] == '\0' ||
	    !product_audio_master_enabled(state)) return -1;
	if (current_route(state, route, sizeof(route)) != 0) return -1;
	snprintf(vol, sizeof(vol), "%d", volume);
	snprintf(skip, sizeof(skip), "%llu", skip_ms);
	/* Optional control pipe: the player reads live volume updates on stdin,
	 * so a volume step never restarts the stream. */
	if (ctl_fd != NULL && pipe2(ctl, O_CLOEXEC) != 0) {
		ctl[0] = -1;
		ctl[1] = -1;
	}
	pid = fork();
	if (pid < 0) {
		if (ctl[0] >= 0) close(ctl[0]);
		if (ctl[1] >= 0) close(ctl[1]);
		return -1;
	}
	if (pid == 0) {
		if (ctl[0] >= 0)
			(void)dup2(ctl[0], STDIN_FILENO);
		execl(MP3_PLAYER, MP3_PLAYER, route, vol, skip,
		      profile != NULL ? profile : "stream", path, (char *)NULL);
		_exit(127);
	}
	if (ctl[0] >= 0)
		close(ctl[0]);
	if (ctl_fd != NULL && ctl[1] >= 0) {
		(void)fcntl(ctl[1], F_SETFL, O_NONBLOCK);
		*ctl_fd = ctl[1];
	}
	return pid;
}

static void close_music_ctl(struct audio_state *state)
{
	if (state->music_ctl_fd >= 0) {
		close(state->music_ctl_fd);
		state->music_ctl_fd = -1;
	}
}

static void stop_music_player(struct audio_state *state)
{
	close_music_ctl(state);
	stop_child(&state->music_pid);
}

/* Apply a Home Music volume to the running player without restarting it. */
static bool music_set_live_volume(struct audio_state *state, int volume)
{
	char line[16];
	int n;

	if (state->music_pid <= 0 || state->music_ctl_fd < 0)
		return false;

	n = snprintf(line, sizeof(line), "%d\n", volume);
	return n > 0 && write(state->music_ctl_fd, line, (size_t)n) == n;
}

static bool choose_music_track_in(const char *music_dir, char *path,
				  size_t size)
{
	DIR *dir;
	struct dirent *entry;
	unsigned int count = 0;
	bool selected = false;

	dir = opendir(music_dir);
	if (dir == NULL)
		return false;

	while ((entry = readdir(dir)) != NULL) {
		if (!mp3_name(entry->d_name))
			continue;

		count++;

		if ((unsigned int)(rand() % (int)count) == 0) {
			int n = snprintf(path, size, "%s/%s",
					 music_dir, entry->d_name);

			if (n > 0 && (size_t)n < size)
				selected = true;
		}
	}

	closedir(dir);
	return selected;
}

/* The active user's own music wins; the bundled nuubOS selection is the
 * default only when the user has no MP3 files of their own. */
static bool choose_music_track(char *path, size_t size)
{
	return choose_music_track_in(HOME_MUSIC_DIR, path, size) ||
	       choose_music_track_in(DEFAULT_HOME_MUSIC_DIR, path, size);
}

static void start_test(struct audio_state *state,
		       unsigned long long offset_ms)
{
	if (!product_audio_master_enabled(state)) {
		stop_child(&state->test_pid);
		state->test_requested = false;
		state->test_offset_ms = 0;
		return;
	}

	stop_child(&state->test_pid);

	state->test_pid = launch_wav(
		state, TEST_SOUND_PATH, 100, offset_ms, "stream", NULL);

	if (state->test_pid > 0) {
		state->test_offset_ms = offset_ms;
		(void)clock_gettime(CLOCK_MONOTONIC, &state->test_started);
		return;
	}

	state->test_requested = false;
	state->test_offset_ms = 0;
}

static void stop_test(struct audio_state *state)
{
	stop_child(&state->test_pid);
	state->test_requested = false;
	state->test_offset_ms = 0;
}

static void start_system_sound(struct audio_state *state,
			       const char *name)
{
	char path[256];

	if (!system_sound_allowed(state, name) ||
	    !product_audio_master_enabled(state))
		return;

	if (strcmp(name, "boot") != 0 &&
	    strcmp(name, "poweroff") != 0 &&
	    strcmp(name, "restart") != 0 &&
	    strcmp(name, "select") != 0 &&
	    strcmp(name, "back") != 0 &&
	    strcmp(name, "navigation") != 0 &&
	    strcmp(name, "quick-settings") != 0)
		return;

	snprintf(path, sizeof(path), "%s/%s.wav",
		 SYSTEM_SOUND_DIR, name);

	if (!copy_string(state->system_file,
			 sizeof(state->system_file), path))
		return;

	/* One persistent player/stream for all cues: spawning a player per cue
	 * made WirePlumber set up and link a new stream node every time. A new
	 * cue interrupts the playing one inside the player. Retried once with
	 * a fresh player if the old one has gone away. */
	for (int attempt = 0; attempt < 2; attempt++) {
		char line[320];
		int n;

		if (state->system_pid <= 0 || state->system_ctl_fd < 0) {
			stop_system_sound(state);
			state->system_pid = launch_wav(
				state, "-", state->volume_system, 0,
				"sfx-server", &state->system_ctl_fd);
			if (state->system_pid <= 0)
				return;
		}

		n = snprintf(line, sizeof(line), "PLAY %d %s\n",
			     state->volume_system, state->system_file);
		if (n > 0 && (size_t)n < sizeof(line) &&
		    write(state->system_ctl_fd, line, (size_t)n) == n)
			return;

		stop_system_sound(state);
	}
}

static void stop_system_sound(struct audio_state *state)
{
	if (state->system_ctl_fd >= 0) {
		close(state->system_ctl_fd);
		state->system_ctl_fd = -1;
	}
	stop_child(&state->system_pid);
}

static bool start_music_current(struct audio_state *state)
{
	if (!state->music_requested ||
	    state->volume_home_music <= 0 ||
	    !product_audio_master_enabled(state) ||
	    state->music_file[0] == '\0')
		return false;

	stop_music_player(state);

	state->music_pid = launch_mp3(
		state,
		state->music_file,
		state->volume_home_music,
		state->music_offset_ms,
		"stream",
		&state->music_ctl_fd);

	if (state->music_pid <= 0)
		return false;

	(void)clock_gettime(CLOCK_MONOTONIC, &state->music_started);
	return true;
}

/* Append one code point as UTF-8; stops silently when out is full. */
static void utf8_put(char *out, size_t size, size_t *len, unsigned int cp)
{
	char enc[4];
	size_t n;

	if (cp < 0x80) {
		enc[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		enc[0] = (char)(0xc0 | (cp >> 6));
		enc[1] = (char)(0x80 | (cp & 0x3f));
		n = 2;
	} else if (cp < 0x10000) {
		enc[0] = (char)(0xe0 | (cp >> 12));
		enc[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		enc[2] = (char)(0x80 | (cp & 0x3f));
		n = 3;
	} else {
		enc[0] = (char)(0xf0 | (cp >> 18));
		enc[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		enc[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		enc[3] = (char)(0x80 | (cp & 0x3f));
		n = 4;
	}
	if (*len + n >= size)
		return;
	memcpy(out + *len, enc, n);
	*len += n;
	out[*len] = '\0';
}

/* ID3 text frame payload: encoding byte + text, NUL terminated or not. */
static void id3_text(const unsigned char *data, size_t size,
		     char *out, size_t out_size)
{
	size_t len = 0;
	unsigned char encoding;
	size_t i = 1;

	out[0] = '\0';
	if (size < 2)
		return;
	encoding = data[0];

	if (encoding == 1 || encoding == 2) {
		bool big_endian = encoding == 2;

		if (encoding == 1 && size >= 3) {
			if (data[1] == 0xfe && data[2] == 0xff) {
				big_endian = true;
				i = 3;
			} else if (data[1] == 0xff && data[2] == 0xfe) {
				i = 3;
			}
		}
		for (; i + 1 < size; i += 2) {
			unsigned int u = big_endian ?
				(unsigned int)(data[i] << 8 | data[i + 1]) :
				(unsigned int)(data[i + 1] << 8 | data[i]);

			if (u == 0)
				break;
			if (u >= 0xd800 && u < 0xdc00 && i + 3 < size) {
				unsigned int lo = big_endian ?
					(unsigned int)(data[i + 2] << 8 | data[i + 3]) :
					(unsigned int)(data[i + 3] << 8 | data[i + 2]);

				if (lo >= 0xdc00 && lo < 0xe000) {
					u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
					i += 2;
				}
			}
			utf8_put(out, out_size, &len, u);
		}
	} else {
		for (; i < size && data[i] != 0; i++) {
			if (encoding != 3) {
				/* ISO-8859-1 maps 1:1 onto code points. */
				utf8_put(out, out_size, &len, data[i]);
			} else if (len + 1 < out_size) {
				out[len++] = (char)data[i];
				out[len] = '\0';
			}
		}
	}

	while (len > 0 && out[len - 1] == ' ')
		out[--len] = '\0';
}

/*
 * Title and artist of an MP3 for the "Now Playing" notification. Reads only
 * the leading ID3v2 tag (bounded) or the trailing ID3v1 block; the file name
 * is the fallback title.
 */
static void read_track_metadata(const char *path, char *title, size_t title_size,
				char *artist, size_t artist_size)
{
	unsigned char header[10];
	FILE *fp;
	const char *base;
	size_t base_len;

	title[0] = '\0';
	artist[0] = '\0';

	fp = fopen(path, "rb");
	if (fp && fread(header, 1, sizeof(header), fp) == sizeof(header) &&
	    memcmp(header, "ID3", 3) == 0 && header[3] >= 2 && header[3] <= 4) {
		unsigned int version = header[3];
		size_t tag_size = ((size_t)(header[6] & 0x7f) << 21) |
				  ((size_t)(header[7] & 0x7f) << 14) |
				  ((size_t)(header[8] & 0x7f) << 7) |
				  (size_t)(header[9] & 0x7f);
		unsigned char *tag;

		if (tag_size > 256 * 1024)
			tag_size = 256 * 1024;
		tag = malloc(tag_size);
		if (tag && fread(tag, 1, tag_size, fp) == tag_size) {
			size_t pos = 0;
			size_t id_len = version == 2 ? 3 : 4;
			size_t frame_header = version == 2 ? 6 : 10;

			/* v2.3/v2.4 extended header. */
			if (version > 2 && (header[5] & 0x40) && tag_size >= 4) {
				size_t ext = version == 4 ?
					((size_t)(tag[0] & 0x7f) << 21 | (size_t)(tag[1] & 0x7f) << 14 |
					 (size_t)(tag[2] & 0x7f) << 7 | (size_t)(tag[3] & 0x7f)) :
					((size_t)tag[0] << 24 | (size_t)tag[1] << 16 |
					 (size_t)tag[2] << 8 | (size_t)tag[3]) + 4;
				pos = ext < tag_size ? ext : tag_size;
			}

			while (pos + frame_header <= tag_size && tag[pos] != 0) {
				const unsigned char *f = tag + pos;
				size_t frame_size;

				if (version == 2)
					frame_size = (size_t)f[3] << 16 | (size_t)f[4] << 8 | f[5];
				else if (version == 3)
					frame_size = (size_t)f[4] << 24 | (size_t)f[5] << 16 |
						     (size_t)f[6] << 8 | f[7];
				else
					frame_size = (size_t)(f[4] & 0x7f) << 21 |
						     (size_t)(f[5] & 0x7f) << 14 |
						     (size_t)(f[6] & 0x7f) << 7 | (f[7] & 0x7f);
				if (frame_size == 0 ||
				    frame_size > tag_size - pos - frame_header)
					break;

				if (!memcmp(f, version == 2 ? "TT2" : "TIT2", id_len))
					id3_text(f + frame_header, frame_size, title, title_size);
				else if (!memcmp(f, version == 2 ? "TP1" : "TPE1", id_len))
					id3_text(f + frame_header, frame_size, artist, artist_size);

				pos += frame_header + frame_size;
			}
		}
		free(tag);
	}

	if (fp && title[0] == '\0') {
		unsigned char v1[128];

		if (fseek(fp, -128, SEEK_END) == 0 &&
		    fread(v1, 1, sizeof(v1), fp) == sizeof(v1) &&
		    memcmp(v1, "TAG", 3) == 0) {
			unsigned char field[31];

			field[0] = 0;
			memcpy(field + 1, v1 + 3, 30);
			id3_text(field, sizeof(field), title, title_size);
			memcpy(field + 1, v1 + 33, 30);
			id3_text(field, sizeof(field), artist, artist_size);
		}
	}
	if (fp)
		fclose(fp);

	if (title[0] == '\0') {
		base = strrchr(path, '/');
		base = base ? base + 1 : path;
		base_len = strlen(base);
		if (base_len > 4 && strcasecmp(base + base_len - 4, ".mp3") == 0)
			base_len -= 4;
		if (base_len >= title_size)
			base_len = title_size - 1;
		memcpy(title, base, base_len);
		title[base_len] = '\0';
	}
}

static void notify_track(const char *path)
{
	struct nuubos_notify n;
	char title[160];
	char artist[160];

	read_track_metadata(path, title, sizeof(title), artist, sizeof(artist));
	nuubos_notify_begin(&n, "POST", "music", "music.track");
	nuubos_notify_str(&n, "title", title);
	if (artist[0] != '\0')
		nuubos_notify_str(&n, "artist", artist);
	(void)nuubos_notify_send(&n);
}

static bool start_music_new(struct audio_state *state)
{
	state->music_offset_ms = 0;

	if (!state->music_requested ||
	    state->volume_home_music <= 0 ||
	    !product_audio_master_enabled(state)) {
		state->music_file[0] = '\0';
		return false;
	}

	if (!choose_music_track(state->music_file,
				sizeof(state->music_file))) {
		state->music_file[0] = '\0';
		return false;
	}

	if (!start_music_current(state))
		return false;
	/* Only a newly chosen track is announced; resuming the same track
	 * after a reroute goes through start_music_current() directly. */
	notify_track(state->music_file);
	return true;
}

static void stop_music(struct audio_state *state)
{
	stop_music_player(state);
	state->music_offset_ms = 0;
	state->music_file[0] = '\0';
}

static void reroute_owned_streams(struct audio_state *state)
{
	restart_ui_anchor(state);

	if (!product_audio_master_enabled(state)) {
		stop_test(state);
		stop_system_sound(state);
		stop_music(state);
		return;
	}

	if (state->test_requested) {
		state->test_offset_ms += elapsed_ms(&state->test_started);
		start_test(state, state->test_offset_ms);
	}

	/* The system-sound player starts again with the next cue, on a stream
	 * bound to the new route; at most the tail of a cue in flight is cut. */
	if (state->system_pid > 0)
		stop_system_sound(state);

	if (state->music_requested &&
	    state->volume_home_music > 0) {
		if (state->music_file[0] != '\0') {
			state->music_offset_ms += elapsed_ms(&state->music_started);
			(void)start_music_current(state);
		} else {
			(void)start_music_new(state);
		}
	}
}

static int persist_stream_volume(struct audio_state *state,
				 DBusConnection *conn,
				 bool music,
				 int volume)
{
	const char *key;
	char value[16];
	int old;

	if (volume < 0)
		volume = 0;
	if (volume > 100)
		volume = 100;

	key = music ? "AUDIO_VOLUME_HOME_MUSIC" :
		      "AUDIO_VOLUME_SYSTEM";
	old = music ? state->volume_home_music :
		      state->volume_system;

	snprintf(value, sizeof(value), "%d", volume);

	if (update_config(key, value) != 0)
		return -1;

	if (music) {
		state->volume_home_music = volume;

		if (volume == 0) {
			stop_music(state);
		} else if (old == 0 && state->music_requested) {
			state->music_file[0] = '\0';
			state->music_offset_ms = 0;
			(void)start_music_new(state);
		} else if (state->music_pid > 0 &&
			   !music_set_live_volume(state, volume)) {
			state->music_offset_ms += elapsed_ms(&state->music_started);
			(void)start_music_current(state);
		}
	} else {
		state->volume_system = volume;

		if (volume == 0)
			stop_system_sound(state);
	}

	ensure_ui_anchor(state);
	emit_state_changed(conn, state);
	return 0;
}

/* Reset System Settings: restore every device-global Product Audio setting
 * through the same persistence/apply paths used by interactive changes, so
 * routing, hard-mute boundaries and owned streams stay coherent. */
static int reset_audio_defaults(struct audio_state *state,
				DBusConnection *conn)
{
	int rc = 0;

	if (persist_output_mode(state, conn, DEFAULT_OUTPUT_MODE) != 0)
		rc = -1;

	if (volume_supported(state->selected)) {
		int volume = strcmp(state->selected, "speaker") == 0 ?
				     DEFAULT_VOLUME_SPEAKER :
			     strcmp(state->selected, "headphones") == 0 ?
				     DEFAULT_VOLUME_HEADPHONES :
				     DEFAULT_VOLUME_BLUETOOTH;

		if (persist_selected_volume(state, conn, volume) != 0)
			rc = -1;
	}
	state->volume_speaker = DEFAULT_VOLUME_SPEAKER;
	state->volume_headphones = DEFAULT_VOLUME_HEADPHONES;
	state->volume_bluetooth = DEFAULT_VOLUME_BLUETOOTH;
	state->volume_config_dirty = true;
	if (flush_volume_config(state) != 0)
		rc = -1;

	if (persist_stream_volume(state, conn, false, DEFAULT_VOLUME_SYSTEM) != 0)
		rc = -1;
	if (persist_stream_volume(state, conn, true, DEFAULT_VOLUME_HOME_MUSIC) != 0)
		rc = -1;
	if (persist_applications_volume(state, conn,
					DEFAULT_VOLUME_APPLICATIONS) != 0)
		rc = -1;
	if (persist_sound_toggle(state, conn, true, DEFAULT_NAVIGATION_SOUNDS) != 0)
		rc = -1;
	if (persist_sound_toggle(state, conn, false, DEFAULT_POWER_SOUNDS) != 0)
		rc = -1;

	emit_state_changed(conn, state);
	return rc;
}

static void service_owned_streams(struct audio_state *state)
{
	int status;
	pid_t rc;

	if (state->app_volume_pid > 0) {
		rc = waitpid(state->app_volume_pid, &status, WNOHANG);
		if (rc == state->app_volume_pid) {
			state->app_volume_pid = -1;
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				fprintf(stderr, "nuubos-audiod: Applications volume policy helper failed\n");
		}
	}

	if (state->ui_anchor_pid > 0) {
		rc = waitpid(state->ui_anchor_pid, &status, WNOHANG);
		if (rc == state->ui_anchor_pid)
			state->ui_anchor_pid = -1;
	}

	if (state->test_pid > 0) {
		rc = waitpid(state->test_pid, &status, WNOHANG);

		if (rc == state->test_pid) {
			state->test_pid = -1;
			state->test_requested = false;
			state->test_offset_ms = 0;
		}
	}

	if (state->system_pid > 0) {
		rc = waitpid(state->system_pid, &status, WNOHANG);

		if (rc == state->system_pid) {
			state->system_pid = -1;
			if (state->system_ctl_fd >= 0) {
				close(state->system_ctl_fd);
				state->system_ctl_fd = -1;
			}
		}
	}

	if (state->music_pid > 0) {
		rc = waitpid(state->music_pid, &status, WNOHANG);

		if (rc == state->music_pid) {
			state->music_pid = -1;
			close_music_ctl(state);
			state->music_offset_ms = 0;

			if (state->music_requested &&
			    state->volume_home_music > 0 &&
			    product_audio_master_enabled(state))
				(void)start_music_new(state);
		}
	} else if (state->music_requested &&
		   state->volume_home_music > 0 &&
		   product_audio_master_enabled(state) &&
		   state->music_file[0] == '\0') {
		(void)start_music_new(state);
	}
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

	if (strcmp(command, "TEST START") == 0) {
		state->test_requested = true;
		state->test_offset_ms = 0;
		start_test(state, 0);
		snprintf(reply, reply_size,
			 state->test_pid > 0 ? "OK\n" : "ERR test sound failed\n");
		return;
	}

	if (strcmp(command, "TEST STOP") == 0) {
		stop_test(state);
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "TEST STATUS") == 0) {
		snprintf(reply, reply_size, "%s\n",
			 state->test_pid > 0 ? "playing" : "stopped");
		return;
	}

	if (strcmp(command, "RESET DEFAULTS") == 0) {
		snprintf(reply, reply_size,
			 reset_audio_defaults(state, dbus_conn) == 0 ?
				 "OK\n" : "ERR audio reset incomplete\n");
		return;
	}

	if (sscanf(command, "SFX PLAY %31s", arg1) == 1) {
		start_system_sound(state, arg1);
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "SYSTEM VOLUME GET") == 0) {
		snprintf(reply, reply_size, "%d\n", state->volume_system);
		return;
	}

	if (sscanf(command, "SYSTEM VOLUME SET %31s", arg1) == 1) {
		int volume = parse_volume(arg1);

		if (volume < 0 ||
		    persist_stream_volume(state, dbus_conn, false, volume) != 0)
			snprintf(reply, reply_size,
				 "ERR system volume update failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (sscanf(command, "SYSTEM VOLUME ADJUST %31s", arg1) == 1) {
		int delta = atoi(arg1);
		int volume = state->volume_system + delta;

		if (volume < 0)
			volume = 0;
		if (volume > 100)
			volume = 100;

		if (persist_stream_volume(state, dbus_conn, false, volume) != 0)
			snprintf(reply, reply_size,
				 "ERR system volume update failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "APPLICATIONS VOLUME GET") == 0) {
		snprintf(reply, reply_size, "%d\n", state->volume_applications);
		return;
	}

	if (sscanf(command, "APPLICATIONS VOLUME SET %31s", arg1) == 1) {
		int volume = parse_volume(arg1);

		if (volume < 0 ||
		    persist_applications_volume(state, dbus_conn, volume) != 0)
			snprintf(reply, reply_size,
				 "ERR applications volume update failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (sscanf(command, "APPLICATIONS VOLUME ADJUST %31s", arg1) == 1) {
		int delta = atoi(arg1);
		int volume = state->volume_applications + delta;

		if (volume < 0)
			volume = 0;
		if (volume > 100)
			volume = 100;

		if (persist_applications_volume(state, dbus_conn, volume) != 0)
			snprintf(reply, reply_size,
				 "ERR applications volume update failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "SYSTEM NAVIGATION GET") == 0) {
		snprintf(reply, reply_size, "%d\n",
			 state->navigation_sounds_enabled ? 1 : 0);
		return;
	}

	if (sscanf(command, "SYSTEM NAVIGATION SET %31s", arg1) == 1) {
		bool enabled = strcmp(arg1, "0") != 0 &&
			       strcasecmp(arg1, "off") != 0 &&
			       strcasecmp(arg1, "false") != 0;
		if (persist_sound_toggle(state, dbus_conn, true, enabled) != 0)
			snprintf(reply, reply_size, "ERR navigation sound toggle failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "SYSTEM POWER GET") == 0) {
		snprintf(reply, reply_size, "%d\n",
			 state->power_sounds_enabled ? 1 : 0);
		return;
	}

	if (sscanf(command, "SYSTEM POWER SET %31s", arg1) == 1) {
		bool enabled = strcmp(arg1, "0") != 0 &&
			       strcasecmp(arg1, "off") != 0 &&
			       strcasecmp(arg1, "false") != 0;
		if (persist_sound_toggle(state, dbus_conn, false, enabled) != 0)
			snprintf(reply, reply_size, "ERR power sound toggle failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "MUSIC VOLUME GET") == 0) {
		snprintf(reply, reply_size, "%d\n", state->volume_home_music);
		return;
	}

	if (sscanf(command, "MUSIC VOLUME SET %31s", arg1) == 1) {
		int volume = parse_volume(arg1);

		if (volume < 0 ||
		    persist_stream_volume(state, dbus_conn, true, volume) != 0)
			snprintf(reply, reply_size,
				 "ERR home music volume update failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (sscanf(command, "MUSIC VOLUME ADJUST %31s", arg1) == 1) {
		int delta = atoi(arg1);
		int volume = state->volume_home_music + delta;

		if (volume < 0)
			volume = 0;
		if (volume > 100)
			volume = 100;

		if (persist_stream_volume(state, dbus_conn, true, volume) != 0)
			snprintf(reply, reply_size,
				 "ERR home music volume update failed\n");
		else
			snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "MUSIC START") == 0) {
		state->home_session_active = true;
		state->music_requested = true;
		ensure_ui_anchor(state);

		if (state->volume_home_music > 0 &&
		    state->music_pid <= 0)
			(void)start_music_new(state);

		emit_state_changed(dbus_conn, state);
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "MUSIC STOP") == 0) {
		state->home_session_active = false;
		state->music_requested = false;
		stop_music(state);
		ensure_ui_anchor(state);
		emit_state_changed(dbus_conn, state);
		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (strcmp(command, "MUSIC NEXT") == 0) {
		stop_music_player(state);
		state->music_file[0] = '\0';
		state->music_offset_ms = 0;

		if (state->music_requested &&
		    state->volume_home_music > 0)
			(void)start_music_new(state);

		emit_state_changed(dbus_conn, state);
		snprintf(reply, reply_size, "OK\n");
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
		if (persist_output_mode(state, dbus_conn, arg1) != 0) {
			snprintf(reply, reply_size, "ERR invalid or unpersistable output\n");
			return;
		}

		snprintf(reply, reply_size, "OK\n");
		return;
	}

	if (sscanf(command, "VOLUME ADJUST %31s", arg1) == 1) {
		int delta = atoi(arg1);
		int current = selected_volume(state);
		int rc;

		if (current < 0) {
			snprintf(reply, reply_size,
				 "unsupported %s\n", state->selected);
			return;
		}

		rc = persist_selected_volume(
			state, dbus_conn,
			(current + delta < 0) ? 0 :
			(current + delta > 100) ? 100 :
			current + delta);
		if (rc != 0) {
			snprintf(reply, reply_size, "ERR volume update failed\n");
			return;
		}

		status_reply(state, reply, reply_size);
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
		char value[16];
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

		if (strcmp(state->selected, arg1) == 0) {
			if (persist_selected_volume(state, dbus_conn, volume) != 0) {
				snprintf(reply, reply_size, "ERR volume update failed\n");
				return;
			}
		} else {
			key = volume_key(arg1);
			snprintf(value, sizeof(value), "%d", volume);
			if (update_config(key, value) != 0) {
				snprintf(reply, reply_size,
					 "ERR cannot persist volume: %s\n",
					 strerror(errno));
				return;
			}
			set_volume_memory(state, arg1, volume);
			emit_state_changed(dbus_conn, state);
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
				reroute_owned_streams(state);
			}
		}
	}
}

static void refresh_hdmi_state(struct audio_state *state)
{
	DIR *dir;
	struct dirent *entry;
	bool available = false;
	bool known = false;

	dir = opendir(DRM_CLASS_DIR);
	if (dir == NULL) {
		state->hdmi_known = false;
		state->hdmi_available = false;
		reconcile_selection(state);
		return;
	}

	while ((entry = readdir(dir)) != NULL) {
		char path[512];
		char status[32];
		FILE *fp;
		int n;

		if (strstr(entry->d_name, "-HDMI-A-") == NULL)
			continue;

		n = snprintf(path, sizeof(path), "%s/%s/status",
			     DRM_CLASS_DIR, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(path))
			continue;

		fp = fopen(path, "r");
		if (fp == NULL)
			continue;

		if (fgets(status, sizeof(status), fp) != NULL) {
			known = true;

			if (strncmp(status, "connected", 9) == 0)
				available = true;
		}

		fclose(fp);

		if (available)
			break;
	}

	closedir(dir);

	state->hdmi_known = known;
	state->hdmi_available = available;
	reconcile_selection(state);
}

static int open_drm_uevent_socket(struct audio_state *state)
{
	struct sockaddr_nl addr;
	int fd;
	int rcvbuf = 64 * 1024;

	fd = socket(AF_NETLINK,
		    SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
		    NETLINK_KOBJECT_UEVENT);
	if (fd < 0) {
		refresh_hdmi_state(state);
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = (unsigned int)getpid();
	addr.nl_groups = 1;

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		refresh_hdmi_state(state);
		return -1;
	}

	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
			 &rcvbuf, sizeof(rcvbuf));

	/*
	 * Register the listener before taking the first snapshot. A connector
	 * transition racing startup is therefore either visible in sysfs or
	 * delivered as a subsequent DRM uevent.
	 */
	refresh_hdmi_state(state);

	return fd;
}

static bool drain_drm_uevents(int fd, struct audio_state *state)
{
	bool refresh = false;

	for (;;) {
		char buffer[4096];
		ssize_t n;
		size_t offset = 0;
		bool drm = false;
		bool hotplug = false;
		bool connector = false;

		n = recv(fd, buffer, sizeof(buffer) - 1, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			state->hdmi_known = false;
			state->hdmi_available = false;
			reconcile_selection(state);
			return false;
		}

		if (n == 0)
			continue;

		buffer[n] = '\0';

		while (offset < (size_t)n) {
			const char *field = buffer + offset;
			size_t remaining = (size_t)n - offset;
			size_t len = strnlen(field, remaining);

			if (len == 0 || len == remaining)
				break;

			if (strcmp(field, "SUBSYSTEM=drm") == 0)
				drm = true;
			else if (strcmp(field, "HOTPLUG=1") == 0)
				hotplug = true;
			else if (strcmp(field, "DEVTYPE=drm_connector") == 0)
				connector = true;

			offset += len + 1;
		}

		if (drm && (hotplug || connector))
			refresh = true;
	}

	if (refresh) {
		refresh_hdmi_state(state);
		reroute_owned_streams(state);
	}

	return true;
}


static bool bluetooth_devices_have_connected_audio(DBusMessage *reply)
{
	DBusMessageIter root, array;
	if (!reply || !dbus_message_iter_init(reply, &root) ||
	    dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY)
		return false;
	dbus_message_iter_recurse(&root, &array);
	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
		DBusMessageIter item;
		const char *address = NULL, *name = NULL, *kind = NULL;
		dbus_bool_t paired = FALSE, connected = FALSE, trusted = FALSE;
		dbus_int32_t rssi = 0;
		dbus_message_iter_recurse(&array, &item);
		if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&item, &address);
		if (!dbus_message_iter_next(&item)) break;
		if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&item, &name);
		if (!dbus_message_iter_next(&item)) break;
		if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&item, &kind);
		if (!dbus_message_iter_next(&item)) break;
		if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(&item, &paired);
		if (!dbus_message_iter_next(&item)) break;
		if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(&item, &connected);
		if (!dbus_message_iter_next(&item)) break;
		if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(&item, &trusted);
		if (dbus_message_iter_next(&item) && dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_INT32)
			dbus_message_iter_get_basic(&item, &rssi);
		(void)address; (void)name; (void)paired; (void)trusted; (void)rssi;
		if (connected && kind && strcasecmp(kind, "audio") == 0)
			return true;
		dbus_message_iter_next(&array);
	}
	return false;
}

static void refresh_bluetooth_state(DBusConnection *conn, struct audio_state *state)
{
	DBusMessage *msg = NULL, *reply = NULL;
	DBusError error = DBUS_ERROR_INIT;
	bool old = state->bluetooth_available;
	bool available = false;
	if (conn) {
		msg = dbus_message_new_method_call(BLUETOOTH_SERVICE, BLUETOOTH_PATH,
					   BLUETOOTH_INTERFACE, "GetDevices");
		if (msg) {
			reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &error);
			dbus_message_unref(msg);
			if (reply) {
				available = bluetooth_devices_have_connected_audio(reply);
				dbus_message_unref(reply);
			}
		}
	}
	if (dbus_error_is_set(&error)) dbus_error_free(&error);
	state->bluetooth_available = available;
	reconcile_selection(state);
	if (old != available) reroute_owned_streams(state);
}

static bool dbus_message_is_bluetooth_event(DBusMessage *message)
{
	const char *interface, *member;
	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL) return false;
	interface = dbus_message_get_interface(message);
	member = dbus_message_get_member(message);
	if (!interface || !member) return false;
	if (strcmp(interface, BLUETOOTH_INTERFACE) == 0 &&
	    (strcmp(member, "DevicesSnapshotChanged") == 0 ||
	     strcmp(member, "StateChanged") == 0))
		return true;
	if (strcmp(interface, DBUS_INTERFACE_DBUS) == 0 &&
	    strcmp(member, "NameOwnerChanged") == 0) {
		DBusMessageIter iter;
		const char *name = NULL;
		if (dbus_message_iter_init(message, &iter) &&
		    dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&iter, &name);
			return name && strcmp(name, BLUETOOTH_SERVICE) == 0;
		}
	}
	return false;
}

/* PipeWire registry watch: BlueZ reports a device connected before
 * WirePlumber creates its sink, so the route apply at connect time can find
 * no sink. A new Audio/Sink global is the event that makes it applicable. */
struct sink_watch {
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	bool sink_added;
	bool broken;
};

static void sink_watch_global(void *data, uint32_t id, uint32_t permissions,
			      const char *type, uint32_t version,
			      const struct spa_dict *props)
{
	struct sink_watch *watch = data;
	const char *media_class;

	(void)id; (void)permissions; (void)version;
	if (props == NULL || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;
	media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (media_class != NULL && strcmp(media_class, "Audio/Sink") == 0)
		watch->sink_added = true;
}

static const struct pw_registry_events sink_watch_registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = sink_watch_global,
};

static void sink_watch_error(void *data, uint32_t id, int seq, int res,
			     const char *message)
{
	struct sink_watch *watch = data;

	(void)seq;
	if (id == PW_ID_CORE && res == -EPIPE) {
		fprintf(stderr, "nuubos-audiod: PipeWire connection lost: %s\n",
			message);
		watch->broken = true;
	}
}

static const struct pw_core_events sink_watch_core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = sink_watch_error,
};

static void sink_watch_close(struct sink_watch *watch)
{
	if (watch->registry != NULL) {
		spa_hook_remove(&watch->registry_listener);
		pw_proxy_destroy((struct pw_proxy *)watch->registry);
		watch->registry = NULL;
	}
	if (watch->core != NULL) {
		spa_hook_remove(&watch->core_listener);
		pw_core_disconnect(watch->core);
		watch->core = NULL;
	}
	if (watch->context != NULL) {
		pw_context_destroy(watch->context);
		watch->context = NULL;
	}
	if (watch->loop != NULL) {
		pw_loop_leave(watch->loop);
		pw_loop_destroy(watch->loop);
		watch->loop = NULL;
	}
}

/* Returns the fd to poll, or -1 when PipeWire is unavailable (route
 * applies then only happen on audiod's own events, as before). */
static int sink_watch_open(struct sink_watch *watch)
{
	memset(watch, 0, sizeof(*watch));
	pw_init(NULL, NULL);
	(void)setenv("PIPEWIRE_RUNTIME_DIR", PIPEWIRE_RUNTIME, 1);

	watch->loop = pw_loop_new(NULL);
	if (watch->loop == NULL)
		return -1;
	pw_loop_enter(watch->loop);
	watch->context = pw_context_new(watch->loop, NULL, 0);
	if (watch->context != NULL)
		watch->core = pw_context_connect(watch->context, NULL, 0);
	if (watch->core == NULL) {
		fprintf(stderr, "nuubos-audiod: PipeWire registry watch unavailable\n");
		sink_watch_close(watch);
		return -1;
	}
	pw_core_add_listener(watch->core, &watch->core_listener,
			     &sink_watch_core_events, watch);
	watch->registry = pw_core_get_registry(watch->core,
					       PW_VERSION_REGISTRY, 0);
	if (watch->registry == NULL) {
		sink_watch_close(watch);
		return -1;
	}
	pw_registry_add_listener(watch->registry, &watch->registry_listener,
				 &sink_watch_registry_events, watch);
	return pw_loop_get_fd(watch->loop);
}

static void drop_subscriber(struct audio_state *state, size_t index)
{
	close(state->subscribers[index]);
	state->subscribers[index] =
		state->subscribers[--state->subscriber_count];
}

/* Typed event for subscribers. Only volume changes made by the output
 * device itself are pushed: Product clients that change the volume through
 * audiod already get the result in the command reply. */
static void broadcast_device_volume(struct audio_state *state)
{
	char line[96];
	int len = snprintf(line, sizeof(line),
			   "volume origin=device output=%s volume=%d\n",
			   state->selected, selected_volume(state));
	size_t i = 0;

	while (i < state->subscriber_count) {
		if (send(state->subscribers[i], line, (size_t)len,
			 MSG_NOSIGNAL) != len)
			drop_subscriber(state, i);
		else
			i++;
	}
}

/* AVRCP absolute volume: headset buttons change the transport Volume
 * (0..127) and PipeWire follows it on the sink. Adopt it as the Bluetooth
 * master instead of fighting it. Ignored until audiod has applied the
 * Bluetooth route (the stored master wins on connect) and while one of our
 * own volume writes is still pending. */
static bool handle_bluez_transport_volume(DBusConnection *conn,
					  DBusMessage *message,
					  struct audio_state *state)
{
	DBusMessageIter iter, changed;
	const char *interface = NULL;

	if (!dbus_message_is_signal(message, DBUS_INTERFACE_PROPERTIES,
				    "PropertiesChanged"))
		return false;
	if (!dbus_message_iter_init(message, &iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(&iter, &interface);
	if (strcmp(interface, BLUEZ_TRANSPORT_INTERFACE) != 0)
		return false;
	if (!dbus_message_iter_next(&iter) ||
	    dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return true;

	dbus_message_iter_recurse(&iter, &changed);
	while (dbus_message_iter_get_arg_type(&changed) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, variant;
		const char *key = NULL;
		dbus_uint16_t raw;
		int volume;

		dbus_message_iter_recurse(&changed, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &variant);
		dbus_message_iter_next(&changed);

		if (strcmp(key, "Volume") != 0 ||
		    dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_UINT16)
			continue;
		dbus_message_iter_get_basic(&variant, &raw);
		if (raw > 127)
			raw = 127;
		volume = ((int)raw * 100 + 63) / 127;

		if (strcmp(state->selected, "bluetooth") != 0 ||
		    strcmp(state->applied_route, "bluetooth") != 0 ||
		    state->pipewire_volume_dirty ||
		    volume == state->volume_bluetooth)
			continue;

		if (set_selected_volume(state, conn, volume, false) == 0)
			broadcast_device_volume(state);
	}
	return true;
}

static void drain_dbus(DBusConnection *conn, struct audio_state *state)
{
	DBusMessage *message;
	bool refresh = false;
	if (!conn) return;
	dbus_connection_read_write(conn, 0);
	while ((message = dbus_connection_pop_message(conn)) != NULL) {
		if (handle_audio_method(conn, message, state) ||
		    handle_bluez_transport_volume(conn, message, state))
			;
		else if (dbus_message_is_bluetooth_event(message))
			refresh = true;
		dbus_message_unref(message);
	}
	if (refresh) {
		refresh_bluetooth_state(conn, state);
		emit_state_changed(conn, state);
	}
}

static DBusConnection *open_system_bus(struct audio_state *state)
{
	DBusConnection *conn;
	DBusError error = DBUS_ERROR_INIT;
	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (!conn) {
		if (dbus_error_is_set(&error)) dbus_error_free(&error);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);
	{
		int request = dbus_bus_request_name(conn, AUDIO_SERVICE_NAME,
			DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
		if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) goto fail;
	}
	dbus_bus_add_match(conn,
		"type='signal',sender='org.nuubOS.Bluetooth',"
		"interface='org.nuubOS.Bluetooth1',member='DevicesSnapshotChanged',"
		"path='/org/nuubOS/Bluetooth'", &error);
	if (dbus_error_is_set(&error)) goto fail;
	dbus_bus_add_match(conn,
		"type='signal',sender='org.nuubOS.Bluetooth',"
		"interface='org.nuubOS.Bluetooth1',member='StateChanged',"
		"path='/org/nuubOS/Bluetooth'", &error);
	if (dbus_error_is_set(&error)) goto fail;
	dbus_bus_add_match(conn,
		"type='signal',sender='org.freedesktop.DBus',"
		"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
		"arg0='org.nuubOS.Bluetooth'", &error);
	if (dbus_error_is_set(&error)) goto fail;
	dbus_bus_add_match(conn,
		"type='signal',sender='org.bluez',"
		"interface='org.freedesktop.DBus.Properties',"
		"member='PropertiesChanged',"
		"arg0='" BLUEZ_TRANSPORT_INTERFACE "'", &error);
	if (dbus_error_is_set(&error)) goto fail;
	dbus_connection_flush(conn);
	refresh_bluetooth_state(conn, state);
	return conn;
fail:
	if (dbus_error_is_set(&error)) dbus_error_free(&error);
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
	struct sink_watch sink_watch;
	int pw_fd;
	int drm_uevent_fd;
	int jack_fd;
	int server;
	int sigchld_fd;

	if (argc == 2 && strcmp(argv[1], "--supported") == 0)
		return 0;

	if (argc != 1) {
		fprintf(stderr, "Usage: %s [--supported]\n", argv[0]);
		return 2;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	/* Control clients use short timeouts. If one disconnects before a reply,
	 * never let the resulting EPIPE terminate the Product Audio service. */
	signal(SIGPIPE, SIG_IGN);
	sigchld_fd = open_sigchld_pipe();
	if (sigchld_fd < 0)
		fprintf(stderr, "nuubos-audiod: SIGCHLD pipe failed: %s\n",
			strerror(errno));
	srand((unsigned int)(time(NULL) ^ getpid()));

	load_config(&state);
	if (run_applications_volume_helper(&state, state.volume_applications) != 0)
		fprintf(stderr, "nuubos-audiod: failed to apply Applications volume policy\n");
	/* Hardware DAC stays at unity. Product master volume is handled in
	 * PipeWire so 1% changes do not click/pop the analog codec. */
	if (set_codec_dac_volume(100) < 0)
		fprintf(stderr, "nuubos-audiod: failed to set Codec DAC unity\n");
	jack_fd = open_headphone_jack(&state);
	drm_uevent_fd = open_drm_uevent_socket(&state);

	dbus_conn = open_system_bus(&state);
	pw_fd = sink_watch_open(&sink_watch);
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

		if (drm_uevent_fd >= 0)
			close(drm_uevent_fd);

		if (dbus_conn != NULL)
			dbus_connection_unref(dbus_conn);

		return 1;
	}

	emit_state_changed(dbus_conn, &state);
	start_system_sound(&state, "boot");

	while (!stop_requested) {
		struct pollfd fds[6 + MAX_SUBSCRIBERS];
		int pw_index = -1;
		int subscriber_index;
		nfds_t nfds = 1;
		int jack_index = -1;
		int drm_index = -1;
		int dbus_index = -1;
		int sigchld_index = -1;
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

		if (drm_uevent_fd >= 0) {
			drm_index = (int)nfds;
			fds[nfds].fd = drm_uevent_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (dbus_fd >= 0) {
			dbus_index = (int)nfds;
			fds[nfds].fd = dbus_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (sigchld_fd >= 0) {
			sigchld_index = (int)nfds;
			fds[nfds].fd = sigchld_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		if (pw_fd >= 0) {
			pw_index = (int)nfds;
			fds[nfds].fd = pw_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		/* Subscribers never send after SUBSCRIBE: watch them only to
		 * notice a closed peer. */
		subscriber_index = (int)nfds;
		for (size_t i = 0; i < state.subscriber_count; i++) {
			fds[nfds].fd = state.subscribers[i];
			fds[nfds].events = POLLIN;
			nfds++;
		}

		rc = poll(fds, nfds, sigchld_fd >= 0 ?
			  main_loop_timeout_ms(&state) : 250);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (sigchld_index >= 0 &&
		    (fds[sigchld_index].revents & POLLIN))
			drain_sigchld_pipe(sigchld_fd);

		service_owned_streams(&state);
		if (pipewire_volume_apply_is_due(&state) &&
		    flush_pipewire_volume(&state) != 0)
			fprintf(stderr,
				"nuubos-audiod: deferred PipeWire volume apply failed\n");
		if (volume_persist_is_due(&state) &&
		    flush_volume_config(&state) != 0)
			fprintf(stderr,
				"nuubos-audiod: deferred volume persist failed\n");

		if (rc == 0)
			continue;

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
				reconcile_selection(&state);
				reroute_owned_streams(&state);
			}
			emit_state_changed(dbus_conn, &state);
		}

		if (drm_index >= 0 &&
		    fds[drm_index].revents != 0) {
			bool alive = true;

			if (fds[drm_index].revents & POLLIN)
				alive = drain_drm_uevents(drm_uevent_fd,
							    &state);

			if (!alive ||
			    (fds[drm_index].revents &
			     (POLLERR | POLLHUP | POLLNVAL))) {
				close(drm_uevent_fd);
				drm_uevent_fd = -1;
				state.hdmi_known = false;
				state.hdmi_available = false;
				reconcile_selection(&state);
				reroute_owned_streams(&state);
			}
			emit_state_changed(dbus_conn, &state);
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

		for (size_t i = state.subscriber_count; i-- > 0;) {
			if (fds[subscriber_index + (int)i].revents != 0)
				drop_subscriber(&state, i);
		}

		if (pw_index >= 0 && fds[pw_index].revents != 0) {
			(void)pw_loop_iterate(sink_watch.loop, 0);
			if (sink_watch.sink_added) {
				sink_watch.sink_added = false;
				if (strcmp(state.applied_route, state.selected) != 0) {
					reconcile_selection(&state);
					if (strcmp(state.applied_route,
						   state.selected) == 0) {
						reroute_owned_streams(&state);
						emit_state_changed(dbus_conn, &state);
					}
				}
			}
			if (sink_watch.broken) {
				sink_watch_close(&sink_watch);
				pw_fd = -1;
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

				if (strcmp(command, "SUBSCRIBE") == 0) {
					if (state.subscriber_count < MAX_SUBSCRIBERS &&
					    write_reply(client, "OK\n") == 0 &&
					    fcntl(client, F_SETFD, FD_CLOEXEC) == 0 &&
					    fcntl(client, F_SETFL, O_NONBLOCK) == 0) {
						state.subscribers[state.subscriber_count++] = client;
						continue;
					}
					(void)write_reply(client, "ERR subscribers\n");
					close(client);
					continue;
				}

				handle_command(&state, dbus_conn,
					       command,
					       reply, sizeof(reply));
				(void)write_reply(client, reply);
			}

			close(client);
		}
	}

	stop_test(&state);
	state.home_session_active = false;
	state.music_requested = false;
	stop_music(&state);
	stop_system_sound(&state);
	stop_child(&state.ui_anchor_pid);
	if (state.volume_config_dirty)
		(void)flush_volume_config(&state);

	if (jack_fd >= 0)
		close(jack_fd);

	if (drm_uevent_fd >= 0)
		close(drm_uevent_fd);

	if (dbus_conn != NULL)
		dbus_connection_unref(dbus_conn);

	while (state.subscriber_count > 0)
		drop_subscriber(&state, state.subscriber_count - 1);
	sink_watch_close(&sink_watch);
	close(server);
	unlink(SOCKET_PATH);

	return 0;
}
