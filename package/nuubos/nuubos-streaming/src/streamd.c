/* SPDX-License-Identifier: MIT */
/*
 * nuubos-streamd — PC Game Streaming Service (EPIC-025).
 *
 * Owns PC streaming for the active user: the hosts (NVIDIA GameStream /
 * Sunshine), their pairing, the applications they advertise, the stream
 * settings and the running stream. Moonlight (moonlight-embedded with the
 * nuubOS Wayland/V4L2 request platform) is the streaming backend; it runs
 * as a child process driven through its -control channel (machine readable
 * "@event" lines on stdout, commands on stdin), so this service never links
 * GPL code.
 *
 * Per user (EPIC-003): /userdata/users/<id>/appdata/moonlight holds the
 * client identity (keys/, created by Moonlight on first use), hosts.conf
 * and settings.conf. Another user never sees or uses these credentials.
 *
 * A stream is a session like a game: the nuubOS game gamepads are switched
 * on for its lifetime (inputd connection), the Quick Menu opens over it and
 * Sleep/Restart/Power Off end it first (PRE_POWER). Host discovery is a
 * one-shot mDNS query (no avahi daemon) while a client asks for it.
 *
 * Protocol (/run/nuubos/streamd.sock, one command per line, tab separated
 * arguments): STATUS and SUBSCRIBE return the snapshot (SUBSCRIBE pushes a
 * new one on every change); DISCOVER, ADD <address>, REMOVE <host>,
 * REFRESH [<host>], PAIR <host>, PAIR_CANCEL, APPS <host>,
 * LAUNCH <host> <app>, QUIT [close], SET <key> <value> reply OK or
 * ERR <reason> at once and report progress in the snapshot; STATS replies
 * "OK key=value..." with the running stream's statistics; PRE_POWER
 * <sleep|restart|poweroff> replies once no stream runs. Outcomes the user
 * must see are typed notifications (EPIC-006).
 *
 * Idle: poll() without timeout. Timers exist only while a child process,
 * a discovery or a stream exit is pending.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#define RUN_ROOT "/run/nuubos"
#define SOCKET_PATH RUN_ROOT "/streamd.sock"
#define PIDFILE RUN_ROOT "/streamd.pid"
#define MOONLIGHT_LOG RUN_ROOT "/moonlight.log"
#define INPUT_SOCKET RUN_ROOT "/inputd.sock"
#define ACTIVE_DIR RUN_ROOT "/user"
#define ACTIVE_USER_FILE ACTIVE_DIR "/active"
#define WAYLAND_RUNTIME RUN_ROOT "/wayland-runtime"
#define PIPEWIRE_RUNTIME RUN_ROOT "/pipewire"
#define USERS_DIR "/userdata/users"

#define MOONLIGHT_BIN "/usr/bin/moonlight"
/* The nuubOS game gamepad (nuubos-inputd): positional face buttons, digital
 * triggers, D-pad on hat 0. The GUID is Moonlight's evdev GUID for a
 * virtual device without vendor id: bus 0x06 + the first 11 name bytes. */
#define GAMEPAD_NAME "nuubOS Gamepad"
#define GAMEPAD_MAPPING \
	"060000006e7575624f532047616d6500,nuubOS Gamepad," \
	"a:b0,b:b1,y:b2,x:b3,leftshoulder:b4,rightshoulder:b5," \
	"lefttrigger:b6,righttrigger:b7,back:b8,start:b9," \
	"leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a2,righty:a3," \
	"dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux,"

#define MAX_CLIENTS 16
#define MAX_LINE 1024
#define MAX_HOSTS 16
#define MAX_APPS 128
#define MAX_JOBS 6
#define DEFAULT_PORT 47989

#define PROBE_TIMEOUT_MS 8000
#define APPS_TIMEOUT_MS 15000
#define PAIR_TIMEOUT_MS 120000
#define QUITAPP_TIMEOUT_MS 10000
#define UNPAIR_TIMEOUT_MS 8000
#define DISCOVER_MS 3000
#define STREAM_QUIT_TIMEOUT_MS 5000
#define KILL_TIMEOUT_MS 3000
#define STATS_TIMEOUT_MS 1500

/* Settings (per user). Defaults live only here. */
#define DEFAULT_RESOLUTION "auto"
#define DEFAULT_FPS 60
#define DEFAULT_CODEC "h264"
#define DEFAULT_BITRATE 0 /* 0 = Moonlight's choice for the resolution/fps */
/* The VPU decodes 1080p60 H.264/HEVC in ~6 ms; above that is not offered. */
#define MAX_STREAM_WIDTH 1920
#define MAX_STREAM_HEIGHT 1080

enum online { ONLINE_UNKNOWN = 0, ONLINE_YES, ONLINE_NO };

struct host {
	char id[17];
	char name[64];
	char address[64];
	int port;
	bool paired;
	bool saved;      /* in hosts.conf (added by hand or paired) */
	enum online online;
	bool probing;
};

struct app {
	int id;
	char name[96];
};

enum job_kind {
	JOB_NONE = 0,
	JOB_PROBE,
	JOB_PAIR,
	JOB_APPS,
	JOB_UNPAIR,
	JOB_QUITAPP,
	JOB_STREAM,
};

struct job {
	enum job_kind kind;
	pid_t pid;
	int out_fd;
	int in_fd;
	char host[17];
	char buf[MAX_LINE];
	size_t used;
	long long deadline_ms;
	bool term_sent;
	bool kill_sent;
	char error[32];
	bool ok;
};

struct client {
	int fd;
	bool subscribed;
	bool wants_stats;
	long long stats_deadline_ms;
	char buf[MAX_LINE];
	size_t used;
};

enum stream_state { STREAM_IDLE = 0, STREAM_STARTING, STREAM_RUNNING, STREAM_EXITING };

struct stream {
	enum stream_state state;
	int job;            /* index in jobs[] */
	int input_fd;       /* inputd connection holding game gamepads on */
	char input_buf[128];
	size_t input_used;
	char host[17];
	char app[96];
	bool close_app;     /* quit the host application after the stream */
	bool user_quit;
	bool started;
	int terminated_error;
	char last_stats[384];
	int power_client;
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];
static struct job jobs[MAX_JOBS];
static struct stream stream;

static char user[64];
static char user_dir[PATH_MAX];
static struct host hosts[MAX_HOSTS];
static int host_count;
static struct app apps[MAX_APPS];
static int app_count;
static char apps_host[17];
static const char *apps_state = "none";
static char pairing_host[17];
static char pairing_pin[8];

static char setting_resolution[16] = DEFAULT_RESOLUTION;
static int setting_fps = DEFAULT_FPS;
static char setting_codec[8] = DEFAULT_CODEC;
static int setting_bitrate = DEFAULT_BITRATE;

static int mdns_fd = -1;
static long long mdns_deadline_ms;

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

	while (len > 0 && isspace((unsigned char)text[len - 1]))
		text[--len] = '\0';
}

/* Snapshot fields are tab separated: never let a host value break them. */
static void sanitize(char *text)
{
	for (char *p = text; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r' || *p == '|')
			*p = ' ';
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

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static uint64_t fnv1a(const char *text)
{
	uint64_t h = 1469598103934665603ULL;

	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

/* Host addresses are IPv4/IPv6 literals or DNS names. */
static bool valid_address(const char *address)
{
	size_t len = strlen(address);

	if (len == 0 || len >= sizeof(((struct host *)0)->address))
		return false;
	for (const char *p = address; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != ':')
			return false;
	return address[0] != '-';
}

/* ------------------------------------------------------------------ */
/* Notifications (EPIC-006)                                            */
/* ------------------------------------------------------------------ */

static void notify_event(const char *event, const char *key, const char *value)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "stream", event);
	if (key)
		nuubos_notify_str(&n, key, value);
	(void)nuubos_notify_send(&n);
}

static void notify_stream_failed(const char *reason, const char *name)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "stream", "stream.failed");
	nuubos_notify_str(&n, "reason", reason);
	if (name && name[0])
		nuubos_notify_str(&n, "name", name);
	(void)nuubos_notify_send(&n);
}

/* ------------------------------------------------------------------ */
/* Per-user state                                                      */
/* ------------------------------------------------------------------ */

static struct host *find_host(const char *id)
{
	for (int i = 0; i < host_count; i++)
		if (!strcmp(hosts[i].id, id))
			return &hosts[i];
	return NULL;
}

static struct host *find_host_by_address(const char *address)
{
	for (int i = 0; i < host_count; i++)
		if (!strcmp(hosts[i].address, address))
			return &hosts[i];
	return NULL;
}

static int write_file_atomic(const char *path, const char *data)
{
	char tmp[PATH_MAX];
	FILE *fp;
	int rc = 0;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	if (fputs(data, fp) == EOF || fflush(fp) != 0 || fsync(fileno(fp)) != 0)
		rc = -1;
	if (fclose(fp) != 0)
		rc = -1;
	if (rc == 0 && rename(tmp, path) != 0)
		rc = -1;
	if (rc != 0)
		unlink(tmp);
	return rc;
}

static void save_hosts(void)
{
	char path[PATH_MAX];
	char data[MAX_HOSTS * 192];
	size_t used = 0;

	if (!user[0])
		return;
	data[0] = '\0';
	for (int i = 0; i < host_count; i++) {
		if (!hosts[i].saved)
			continue;
		used += (size_t)snprintf(data + used, sizeof(data) - used, "%s|%s|%s|%d|%d\n",
					 hosts[i].id, hosts[i].name, hosts[i].address,
					 hosts[i].port, hosts[i].paired ? 1 : 0);
		if (used >= sizeof(data))
			break;
	}
	snprintf(path, sizeof(path), "%s/hosts.conf", user_dir);
	if (mkdir_p(user_dir) != 0 || write_file_atomic(path, data) != 0)
		log_msg("streamd: could not write %s", path);
}

static void save_settings(void)
{
	char path[PATH_MAX];
	char data[256];

	if (!user[0])
		return;
	snprintf(data, sizeof(data), "RESOLUTION=%s\nFPS=%d\nCODEC=%s\nBITRATE=%d\n",
		 setting_resolution, setting_fps, setting_codec, setting_bitrate);
	snprintf(path, sizeof(path), "%s/settings.conf", user_dir);
	if (mkdir_p(user_dir) != 0 || write_file_atomic(path, data) != 0)
		log_msg("streamd: could not write %s", path);
}

static bool valid_resolution(const char *v)
{
	return !strcmp(v, "auto") || !strcmp(v, "720p") || !strcmp(v, "1080p");
}

static bool valid_codec(const char *v)
{
	return !strcmp(v, "h264") || !strcmp(v, "hevc");
}

static void load_user_state(void)
{
	char path[PATH_MAX];
	char line[256];
	FILE *fp;

	host_count = 0;
	app_count = 0;
	apps_host[0] = '\0';
	apps_state = "none";
	copy_text(setting_resolution, sizeof(setting_resolution), DEFAULT_RESOLUTION);
	setting_fps = DEFAULT_FPS;
	copy_text(setting_codec, sizeof(setting_codec), DEFAULT_CODEC);
	setting_bitrate = DEFAULT_BITRATE;
	if (!user[0])
		return;

	snprintf(path, sizeof(path), "%s/hosts.conf", user_dir);
	fp = fopen(path, "r");
	while (fp && fgets(line, sizeof(line), fp) && host_count < MAX_HOSTS) {
		char *f[5];
		char *save = NULL;
		int n = 0;
		struct host *h = &hosts[host_count];

		trim(line);
		for (char *t = strtok_r(line, "|", &save); t && n < 5; t = strtok_r(NULL, "|", &save))
			f[n++] = t;
		if (n != 5 || !valid_address(f[2]))
			continue;
		memset(h, 0, sizeof(*h));
		copy_text(h->id, sizeof(h->id), f[0]);
		copy_text(h->name, sizeof(h->name), f[1]);
		copy_text(h->address, sizeof(h->address), f[2]);
		h->port = atoi(f[3]) > 0 ? atoi(f[3]) : DEFAULT_PORT;
		h->paired = atoi(f[4]) != 0;
		h->saved = true;
		host_count++;
	}
	if (fp)
		fclose(fp);

	snprintf(path, sizeof(path), "%s/settings.conf", user_dir);
	fp = fopen(path, "r");
	while (fp && fgets(line, sizeof(line), fp)) {
		trim(line);
		if (!strncmp(line, "RESOLUTION=", 11) && valid_resolution(line + 11))
			copy_text(setting_resolution, sizeof(setting_resolution), line + 11);
		else if (!strncmp(line, "FPS=", 4) && (atoi(line + 4) == 30 || atoi(line + 4) == 60))
			setting_fps = atoi(line + 4);
		else if (!strncmp(line, "CODEC=", 6) && valid_codec(line + 6))
			copy_text(setting_codec, sizeof(setting_codec), line + 6);
		else if (!strncmp(line, "BITRATE=", 8) && atoi(line + 8) >= 0)
			setting_bitrate = atoi(line + 8);
	}
	if (fp)
		fclose(fp);
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

/* Returns true when the active user changed. */
static bool refresh_active_user(void)
{
	char now[64];

	if (!read_active_user(now, sizeof(now)))
		now[0] = '\0';
	if (!strcmp(now, user))
		return false;
	copy_text(user, sizeof(user), now);
	if (user[0])
		snprintf(user_dir, sizeof(user_dir), USERS_DIR "/%s/appdata/moonlight", user);
	else
		user_dir[0] = '\0';
	load_user_state();
	log_msg("streamd: active user %s, %d hosts", user[0] ? user : "(none)", host_count);
	return true;
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

static const char *stream_state_name(void)
{
	switch (stream.state) {
	case STREAM_STARTING:
		return "starting";
	case STREAM_RUNNING:
		return "streaming";
	case STREAM_EXITING:
		return "exiting";
	default:
		return "idle";
	}
}

static char *build_status(size_t *len_out)
{
	size_t cap = 4096 + (size_t)(host_count * 192) + (size_t)(app_count * 128);
	char *out = malloc(cap);
	size_t used = 0;

#define APPEND(...) \
	do { \
		int _n = snprintf(out + used, cap - used, __VA_ARGS__); \
		if (_n > 0) \
			used += (size_t)_n < cap - used ? (size_t)_n : cap - used - 1; \
	} while (0)

	if (!out)
		return NULL;
	out[0] = '\0';
	APPEND("state=%s\n", stream_state_name());
	APPEND("stream_host=%s\n", stream.state == STREAM_IDLE ? "" : stream.host);
	APPEND("stream_app=%s\n", stream.state == STREAM_IDLE ? "" : stream.app);
	APPEND("user=%d\n", user[0] ? 1 : 0);
	APPEND("resolution=%s\n", setting_resolution);
	APPEND("fps=%d\n", setting_fps);
	APPEND("codec=%s\n", setting_codec);
	APPEND("bitrate=%d\n", setting_bitrate);
	APPEND("discovering=%d\n", mdns_fd >= 0 ? 1 : 0);
	APPEND("pairing_host=%s\n", pairing_host);
	APPEND("pairing_pin=%s\n", pairing_pin);
	APPEND("apps_host=%s\n", apps_host);
	APPEND("apps_state=%s\n", apps_state);
	for (int i = 0; i < host_count; i++)
		APPEND("host=%s\t%s\t%s\t%d\t%s\t%d\t%d\n", hosts[i].id, hosts[i].name,
		       hosts[i].address, hosts[i].paired ? 1 : 0,
		       hosts[i].online == ONLINE_YES ? "online" :
		       hosts[i].online == ONLINE_NO ? "offline" : "unknown",
		       hosts[i].saved ? 1 : 0, hosts[i].probing ? 1 : 0);
	for (int i = 0; i < app_count; i++)
		APPEND("app=%d\t%s\n", apps[i].id, apps[i].name);
	APPEND("end=1\n");
#undef APPEND
	*len_out = used;
	return out;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->wants_stats = false;
	c->used = 0;
	if (stream.power_client >= 0 && &clients[stream.power_client] == c)
		stream.power_client = -1;
}

static void reply(struct client *c, const char *text)
{
	if (c->fd >= 0 && write_all(c->fd, text, strlen(text)) != 0)
		close_client(c);
}

static void send_status(struct client *c)
{
	size_t len;
	char *status = build_status(&len);

	if (!status) {
		close_client(c);
		return;
	}
	if (write_all(c->fd, status, len) != 0)
		close_client(c);
	free(status);
}

static void notify_subscribers(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed)
			send_status(&clients[i]);
}

/* ------------------------------------------------------------------ */
/* Moonlight child processes                                           */
/* ------------------------------------------------------------------ */

static int free_job(void)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind == JOB_NONE)
			return i;
	return -1;
}

static bool job_running(enum job_kind kind, const char *host)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind == kind && (!host || !strcmp(jobs[i].host, host)))
			return true;
	return false;
}

/*
 * Stream resolution: "auto" follows the screen in use (HDMI when connected,
 * else the internal panel), so the VPU output is composited 1:1; larger
 * screens are capped at 1080p (decoder budget and Wi-Fi bandwidth).
 */
static void stream_size(int *width, int *height)
{
	glob_t g;
	int best_w = 0, best_h = 0;
	bool hdmi = false;

	*width = 1280;
	*height = 720;
	if (!strcmp(setting_resolution, "720p"))
		return;
	if (!strcmp(setting_resolution, "1080p")) {
		*width = 1920;
		*height = 1080;
		return;
	}
	if (glob("/sys/class/drm/card*-*/status", 0, NULL, &g) != 0)
		return;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		char path[PATH_MAX];
		char line[64];
		bool is_hdmi = strstr(g.gl_pathv[i], "HDMI") != NULL;
		FILE *fp = fopen(g.gl_pathv[i], "r");
		int w = 0, h = 0;

		if (!fp)
			continue;
		if (!fgets(line, sizeof(line), fp))
			line[0] = '\0';
		fclose(fp);
		trim(line);
		if (strcmp(line, "connected") != 0 || (hdmi && !is_hdmi))
			continue;
		copy_text(path, sizeof(path), g.gl_pathv[i]);
		strcpy(strrchr(path, '/'), "/modes");
		fp = fopen(path, "r");
		if (!fp)
			continue;
		if (fgets(line, sizeof(line), fp) && sscanf(line, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
			if (is_hdmi && !hdmi) {
				hdmi = true;
				best_w = 0;
			}
			if (best_w == 0) {
				best_w = w;
				best_h = h;
			}
		}
		fclose(fp);
	}
	globfree(&g);
	if (best_w <= 0)
		return;
	if (best_w > MAX_STREAM_WIDTH || best_h > MAX_STREAM_HEIGHT) {
		/* Keep the aspect ratio inside 1920x1080. */
		double scale = (double)MAX_STREAM_WIDTH / best_w;

		if ((double)MAX_STREAM_HEIGHT / best_h < scale)
			scale = (double)MAX_STREAM_HEIGHT / best_h;
		best_w = (int)(best_w * scale) & ~7;
		best_h = (int)(best_h * scale) & ~7;
	}
	*width = best_w;
	*height = best_h;
}

/* Starts "moonlight <action> <address> ... -control" for the active user. */
static int spawn_job(enum job_kind kind, const struct host *h, char *const extra[], bool with_stdin)
{
	char keydir[PATH_MAX];
	char port[16];
	char *argv[48];
	int argc = 0;
	int out_pipe[2] = { -1, -1 };
	int in_pipe[2] = { -1, -1 };
	int slot = free_job();
	pid_t pid;
	const char *action;
	long long timeout;

	if (slot < 0 || !user[0])
		return -1;
	switch (kind) {
	case JOB_PROBE: action = "status"; timeout = PROBE_TIMEOUT_MS; break;
	case JOB_PAIR: action = "pair"; timeout = PAIR_TIMEOUT_MS; break;
	case JOB_APPS: action = "list"; timeout = APPS_TIMEOUT_MS; break;
	case JOB_UNPAIR: action = "unpair"; timeout = UNPAIR_TIMEOUT_MS; break;
	case JOB_QUITAPP: action = "quit"; timeout = QUITAPP_TIMEOUT_MS; break;
	case JOB_STREAM: action = "stream"; timeout = -1; break;
	default: return -1;
	}
	snprintf(keydir, sizeof(keydir), "%s/keys", user_dir);
	if (mkdir_p(keydir) != 0)
		return -1;
	snprintf(port, sizeof(port), "%d", h->port);

	argv[argc++] = (char *)MOONLIGHT_BIN;
	argv[argc++] = (char *)action;
	argv[argc++] = (char *)h->address;
	argv[argc++] = (char *)"-port";
	argv[argc++] = port;
	argv[argc++] = (char *)"-keydir";
	argv[argc++] = keydir;
	argv[argc++] = (char *)"-control";
	for (int i = 0; extra && extra[i] && argc < 46; i++)
		argv[argc++] = extra[i];
	argv[argc] = NULL;

	if (pipe2(out_pipe, O_CLOEXEC) != 0)
		return -1;
	if (with_stdin && pipe2(in_pipe, O_CLOEXEC) != 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		if (with_stdin) {
			close(in_pipe[0]);
			close(in_pipe[1]);
		}
		return -1;
	}
	if (pid == 0) {
		sigset_t none;
		int log_fd;
		int null_fd;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		if (with_stdin) {
			dup2(in_pipe[0], STDIN_FILENO);
		} else {
			null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
			if (null_fd >= 0)
				dup2(null_fd, STDIN_FILENO);
		}
		dup2(out_pipe[1], STDOUT_FILENO);
		log_fd = open(MOONLIGHT_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
		if (log_fd >= 0)
			dup2(log_fd, STDERR_FILENO);
		if (chdir(user_dir) != 0)
			_exit(126);
		clearenv();
		setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
		setenv("HOME", user_dir, 1);
		setenv("XDG_RUNTIME_DIR", WAYLAND_RUNTIME, 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		setenv("PIPEWIRE_RUNTIME_DIR", PIPEWIRE_RUNTIME, 1);
		setenv("SDL_GAMECONTROLLERCONFIG", GAMEPAD_MAPPING, 1);
		setenv("LANG", "C.UTF-8", 1);
		execv(MOONLIGHT_BIN, argv);
		_exit(127);
	}
	close(out_pipe[1]);
	if (with_stdin)
		close(in_pipe[0]);
	memset(&jobs[slot], 0, sizeof(jobs[slot]));
	jobs[slot].kind = kind;
	jobs[slot].pid = pid;
	jobs[slot].out_fd = out_pipe[0];
	jobs[slot].in_fd = with_stdin ? in_pipe[1] : -1;
	copy_text(jobs[slot].host, sizeof(jobs[slot].host), h->id);
	jobs[slot].deadline_ms = timeout > 0 ? monotonic_ms() + timeout : -1;
	(void)fcntl(jobs[slot].out_fd, F_SETFL, O_NONBLOCK);
	if (jobs[slot].in_fd >= 0)
		(void)fcntl(jobs[slot].in_fd, F_SETFL, O_NONBLOCK);
	log_msg("streamd: %s %s pid=%d", action, h->address, (int)pid);
	return slot;
}

static void job_command(int slot, const char *command)
{
	char line[64];
	int n;

	if (slot < 0 || jobs[slot].in_fd < 0)
		return;
	n = snprintf(line, sizeof(line), "%s\n", command);
	if (write(jobs[slot].in_fd, line, (size_t)n) != n)
		log_msg("streamd: command %s to moonlight failed: %s", command, strerror(errno));
}

static void probe_host(struct host *h)
{
	if (h->probing || job_running(JOB_PROBE, h->id))
		return;
	if (spawn_job(JOB_PROBE, h, NULL, false) >= 0)
		h->probing = true;
}

/* "@key a=1 b=two words" → value of one key (up to the next " key="). */
static bool field(const char *line, const char *key, char *out, size_t size)
{
	size_t klen = strlen(key);
	const char *p = line;

	while ((p = strstr(p, key)) != NULL) {
		if ((p == line || p[-1] == ' ') && p[klen] == '=') {
			const char *v = p + klen + 1;
			const char *end = v;

			/* name= values may hold spaces: they always come last. */
			if (!strcmp(key, "name"))
				end = v + strlen(v);
			else
				while (*end && *end != ' ')
					end++;
			snprintf(out, size, "%.*s", (int)(end - v), v);
			return true;
		}
		p += klen;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Stream session                                                      */
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

/* Game gamepads stay on for as long as this connection is open: if streamd
 * dies, inputd gives the controllers back to the nuubUI by itself. */
static int input_gamepads_on(void)
{
	char answer[64];
	size_t used = 0;
	int fd = connect_socket(INPUT_SOCKET, 1000);

	if (fd < 0)
		return -1;
	if (write_all(fd, "GAMEPADS ON\n", 12) != 0) {
		close(fd);
		return -1;
	}
	while (used + 1 < sizeof(answer)) {
		char c;
		ssize_t n = read(fd, &c, 1);

		if (n <= 0 || c == '\n')
			break;
		answer[used++] = c;
	}
	answer[used] = '\0';
	if (strcmp(answer, "OK") != 0) {
		close(fd);
		return -1;
	}
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

static void reset_stream(void)
{
	if (stream.input_fd >= 0)
		close(stream.input_fd);
	memset(&stream, 0, sizeof(stream));
	stream.state = STREAM_IDLE;
	stream.job = -1;
	stream.input_fd = -1;
	stream.power_client = -1;
}

static const char *launch_stream(const char *host_id, const char *app_name)
{
	struct host *h = find_host(host_id);
	char width[16], height[16], fps[16], bitrate[16];
	char *extra[24];
	int n = 0;
	int w, hh;
	int slot;

	if (stream.state != STREAM_IDLE)
		return "busy";
	if (!user[0])
		return "no-user";
	if (!h)
		return "host";
	if (!h->paired)
		return "unpaired";
	if (!app_name[0] || strlen(app_name) >= sizeof(stream.app))
		return "app";

	stream_size(&w, &hh);
	snprintf(width, sizeof(width), "%d", w);
	snprintf(height, sizeof(height), "%d", hh);
	snprintf(fps, sizeof(fps), "%d", setting_fps);
	extra[n++] = (char *)"-app";
	extra[n++] = (char *)app_name;
	extra[n++] = (char *)"-platform";
	extra[n++] = (char *)"wayland";
	extra[n++] = (char *)"-audio";
	extra[n++] = (char *)"default";
	extra[n++] = (char *)"-gamepad";
	extra[n++] = (char *)GAMEPAD_NAME;
	extra[n++] = (char *)"-noquitcombo";
	extra[n++] = (char *)"-nomouseemulation";
	extra[n++] = (char *)"-width";
	extra[n++] = width;
	extra[n++] = (char *)"-height";
	extra[n++] = height;
	extra[n++] = (char *)"-fps";
	extra[n++] = fps;
	extra[n++] = (char *)"-codec";
	extra[n++] = (char *)(strcmp(setting_codec, "hevc") ? "h264" : "h265");
	if (setting_bitrate > 0) {
		snprintf(bitrate, sizeof(bitrate), "%d", setting_bitrate);
		extra[n++] = (char *)"-bitrate";
		extra[n++] = bitrate;
	}
	extra[n] = NULL;

	stream.input_fd = input_gamepads_on();
	if (stream.input_fd < 0)
		log_msg("streamd: inputd game gamepads unavailable");

	slot = spawn_job(JOB_STREAM, h, extra, true);
	if (slot < 0) {
		if (stream.input_fd >= 0)
			close(stream.input_fd);
		stream.input_fd = -1;
		return "spawn";
	}
	stream.state = STREAM_STARTING;
	stream.job = slot;
	stream.power_client = -1;
	stream.terminated_error = 0;
	copy_text(stream.host, sizeof(stream.host), h->id);
	copy_text(stream.app, sizeof(stream.app), app_name);
	log_msg("streamd: stream %s app=%s %dx%d@%d %s user=%s", h->address, app_name,
		w, hh, setting_fps, setting_codec, user);
	return NULL;
}

static void request_stream_quit(bool close_app)
{
	if (stream.state != STREAM_STARTING && stream.state != STREAM_RUNNING)
		return;
	stream.close_app |= close_app;
	stream.user_quit = true;
	stream.state = STREAM_EXITING;
	jobs[stream.job].deadline_ms = monotonic_ms() + STREAM_QUIT_TIMEOUT_MS;
	job_command(stream.job, "QUIT");
	notify_subscribers();
}

static void finish_power_wait(void)
{
	struct client *c;

	if (stream.power_client < 0 || stream.state != STREAM_IDLE)
		return;
	c = &clients[stream.power_client];
	stream.power_client = -1;
	reply(c, "OK\n");
}

static void answer_stats(const char *line)
{
	char text[512];

	copy_text(stream.last_stats, sizeof(stream.last_stats), line + 7);
	snprintf(text, sizeof(text), "OK %s\n", stream.last_stats);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd < 0 || !clients[i].wants_stats)
			continue;
		clients[i].wants_stats = false;
		reply(&clients[i], text);
	}
}

static void handle_hotkey(const char *action)
{
	/* Emulation chords (save state, slots...) mean nothing on a PC
	 * stream; they are consumed by inputd and ignored here. */
	log_msg("streamd: hotkey %s ignored", action);
}

static void process_input_link(void)
{
	char buf[128];
	ssize_t n = read(stream.input_fd, buf, sizeof(buf));

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		/* inputd restarted: take the gamepads again. */
		close(stream.input_fd);
		stream.input_used = 0;
		stream.input_fd = input_gamepads_on();
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			stream.input_buf[stream.input_used] = '\0';
			if (!strncmp(stream.input_buf, "HOTKEY ", 7))
				handle_hotkey(stream.input_buf + 7);
			stream.input_used = 0;
		} else if (stream.input_used + 1 < sizeof(stream.input_buf)) {
			stream.input_buf[stream.input_used++] = buf[i];
		} else {
			stream.input_used = 0;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Job output and completion                                           */
/* ------------------------------------------------------------------ */

static void handle_job_line(int slot, char *line)
{
	struct job *j = &jobs[slot];
	struct host *h = find_host(j->host);
	char value[128];

	if (line[0] != '@')
		return;
	if (!strncmp(line, "@error ", 7)) {
		size_t len = strcspn(line + 7, " ");

		if (len >= sizeof(j->error))
			len = sizeof(j->error) - 1;
		memmove(j->error, line + 7, len);
		j->error[len] = '\0';
		return;
	}
	switch (j->kind) {
	case JOB_PROBE:
		if (!strncmp(line, "@host ", 6) && h) {
			j->ok = true;
			if (field(line, "paired", value, sizeof(value)))
				h->paired = atoi(value) != 0;
			if (field(line, "name", value, sizeof(value)) && value[0]) {
				sanitize(value);
				copy_text(h->name, sizeof(h->name), value);
			}
		}
		break;
	case JOB_PAIR:
		if (!strncmp(line, "@paired", 7))
			j->ok = true;
		break;
	case JOB_APPS:
		if (!strncmp(line, "@app ", 5) && app_count < MAX_APPS) {
			char name[96];

			if (field(line, "id", value, sizeof(value)) &&
			    field(line, "name", name, sizeof(name))) {
				sanitize(name);
				apps[app_count].id = atoi(value);
				copy_text(apps[app_count].name, sizeof(apps[app_count].name), name);
				app_count++;
			}
		}
		break;
	case JOB_STREAM:
		if (!strncmp(line, "@started", 8)) {
			stream.started = true;
			if (stream.state == STREAM_STARTING) {
				stream.state = STREAM_RUNNING;
				notify_subscribers();
			}
		} else if (!strncmp(line, "@terminated ", 12)) {
			if (field(line, "error", value, sizeof(value)))
				stream.terminated_error = atoi(value);
		} else if (!strncmp(line, "@failed ", 8)) {
			copy_text(j->error, sizeof(j->error), "connection");
		} else if (!strncmp(line, "@stats ", 7)) {
			answer_stats(line);
		}
		break;
	default:
		break;
	}
}

/* Returns the bytes read; closes the pipe at end of file. */
static ssize_t read_job(int slot)
{
	struct job *j = &jobs[slot];
	char buf[1024];
	ssize_t n = read(j->out_fd, buf, sizeof(buf));

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return -1;
		close(j->out_fd);
		j->out_fd = -1;
		return 0;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			j->buf[j->used] = '\0';
			handle_job_line(slot, j->buf);
			j->used = 0;
		} else if (j->used + 1 < sizeof(j->buf)) {
			j->buf[j->used++] = buf[i];
		} else {
			j->used = 0;
		}
	}
	return n;
}

static void finish_job(int slot, int status)
{
	struct job done;
	struct job *j = &done;
	struct host *h;
	bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	/* Everything the process wrote before it exited, then free the slot
	 * (a follow-up job may need it). */
	while (jobs[slot].out_fd >= 0 && read_job(slot) > 0)
		;
	if (jobs[slot].out_fd >= 0)
		close(jobs[slot].out_fd);
	if (jobs[slot].in_fd >= 0)
		close(jobs[slot].in_fd);
	done = jobs[slot];
	memset(&jobs[slot], 0, sizeof(jobs[slot]));
	jobs[slot].kind = JOB_NONE;
	jobs[slot].out_fd = -1;
	jobs[slot].in_fd = -1;
	h = find_host(j->host);

	switch (j->kind) {
	case JOB_PROBE:
		if (h) {
			h->probing = false;
			h->online = j->ok ? ONLINE_YES : ONLINE_NO;
			if (h->saved)
				save_hosts();
		}
		break;
	case JOB_PAIR:
		pairing_host[0] = '\0';
		pairing_pin[0] = '\0';
		if (h && j->ok && exited_ok) {
			h->paired = true;
			h->saved = true;
			h->online = ONLINE_YES;
			save_hosts();
			notify_event("stream.paired", "name", h->name);
		} else if (h && !j->term_sent) {
			notify_event("stream.pair.failed", "name", h->name);
		}
		break;
	case JOB_APPS:
		if (!strcmp(apps_host, j->host)) {
			apps_state = exited_ok ? "ready" : "error";
			if (h && !exited_ok && !strcmp(j->error, "unpaired")) {
				h->paired = false;
				save_hosts();
			}
			if (h)
				h->online = exited_ok ? ONLINE_YES : h->online;
		}
		break;
	case JOB_STREAM: {
		bool close_app = stream.close_app;
		char host_id[17];

		log_msg("streamd: moonlight exited status=%d error=%s terminated=%d", status,
			j->error[0] ? j->error : "-", stream.terminated_error);
		if (!stream.user_quit) {
			if (!stream.started) {
				const char *reason = j->error[0] ? j->error : "start";

				if (!strcmp(reason, "unreachable") || !strcmp(reason, "host"))
					reason = "unreachable";
				else if (!strcmp(reason, "unpaired")) {
					if (h) {
						h->paired = false;
						save_hosts();
					}
				} else if (strcmp(reason, "app") != 0)
					reason = "start";
				notify_stream_failed(reason, h ? h->name : "");
			} else if (stream.terminated_error != 0) {
				notify_stream_failed("connection", h ? h->name : "");
			}
		}
		copy_text(host_id, sizeof(host_id), stream.host);
		reset_stream();
		/* The host application keeps running after a plain quit, so the
		 * next stream resumes it; "close" also ends it on the host. */
		if (close_app && (h = find_host(host_id)) != NULL)
			(void)spawn_job(JOB_QUITAPP, h, NULL, false);
		finish_power_wait();
		break;
	}
	default:
		break;
	}
	notify_subscribers();
}

static void reap_children(void)
{
	char drain[32];
	int status;
	pid_t pid;

	while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
		;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (int i = 0; i < MAX_JOBS; i++) {
			if (jobs[i].kind != JOB_NONE && jobs[i].pid == pid) {
				finish_job(i, status);
				break;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* mDNS discovery (_nvstream._tcp, GameStream and Sunshine hosts)      */
/* ------------------------------------------------------------------ */

static const unsigned char *dns_skip_name(const unsigned char *p, const unsigned char *end)
{
	while (p < end) {
		if (*p == 0)
			return p + 1;
		if ((*p & 0xc0) == 0xc0)
			return p + 2 <= end ? p + 2 : NULL;
		p += *p + 1;
	}
	return NULL;
}

/* First label of a (possibly compressed) name. */
static void dns_first_label(const unsigned char *msg, size_t len, const unsigned char *p,
			    char *out, size_t size)
{
	int hops = 0;

	out[0] = '\0';
	while (p >= msg && p < msg + len && hops++ < 8) {
		if ((*p & 0xc0) == 0xc0) {
			if (p + 1 >= msg + len)
				return;
			p = msg + (((p[0] & 0x3f) << 8) | p[1]);
			continue;
		}
		if (*p == 0 || p + 1 + *p > msg + len)
			return;
		snprintf(out, size, "%.*s", (int)*p, (const char *)p + 1);
		return;
	}
}

static void mdns_start(void)
{
	static const unsigned char query[] = {
		0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
		9, '_', 'n', 'v', 's', 't', 'r', 'e', 'a', 'm',
		4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
		0, 12, 0, 1, /* PTR, IN (legacy unicast: answers come back here) */
	};
	struct sockaddr_in to = { 0 };

	if (mdns_fd >= 0) {
		mdns_deadline_ms = monotonic_ms() + DISCOVER_MS;
		return;
	}
	mdns_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (mdns_fd < 0)
		return;
	to.sin_family = AF_INET;
	to.sin_port = htons(5353);
	to.sin_addr.s_addr = inet_addr("224.0.0.251");
	if (sendto(mdns_fd, query, sizeof(query), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
		log_msg("streamd: mDNS query failed: %s", strerror(errno));
		close(mdns_fd);
		mdns_fd = -1;
		return;
	}
	mdns_deadline_ms = monotonic_ms() + DISCOVER_MS;
}

static void mdns_stop(void)
{
	if (mdns_fd >= 0)
		close(mdns_fd);
	mdns_fd = -1;
}

static void mdns_read(void)
{
	unsigned char msg[1500];
	struct sockaddr_in from;
	socklen_t from_len = sizeof(from);
	ssize_t len;
	bool changed = false;

	while ((len = recvfrom(mdns_fd, msg, sizeof(msg), 0, (struct sockaddr *)&from, &from_len)) > 12) {
		const unsigned char *end = msg + len;
		const unsigned char *p = msg + 12;
		int qd = (msg[4] << 8) | msg[5];
		int records = ((msg[6] << 8) | msg[7]) + ((msg[8] << 8) | msg[9]) + ((msg[10] << 8) | msg[11]);
		char name[64] = "";
		char address[64];
		int port = 0;
		struct host *h;

		for (int i = 0; i < qd && p; i++) {
			p = dns_skip_name(p, end);
			if (p)
				p += 4;
		}
		for (int i = 0; i < records && p && p + 10 <= end; i++) {
			const unsigned char *owner = p;
			int type, rdlen;

			p = dns_skip_name(p, end);
			if (!p || p + 10 > end)
				break;
			type = (p[0] << 8) | p[1];
			rdlen = (p[8] << 8) | p[9];
			p += 10;
			if (p + rdlen > end)
				break;
			if (type == 12 && !name[0])        /* PTR: the instance */
				dns_first_label(msg, (size_t)len, p, name, sizeof(name));
			else if (type == 33 && rdlen >= 6) { /* SRV: port */
				port = (p[4] << 8) | p[5];
				if (!name[0])
					dns_first_label(msg, (size_t)len, owner, name, sizeof(name));
			}
			p += rdlen;
		}
		if (!name[0])
			continue;
		inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));
		sanitize(name);
		h = find_host_by_address(address);
		if (!h && host_count < MAX_HOSTS) {
			h = &hosts[host_count++];
			memset(h, 0, sizeof(*h));
			snprintf(h->id, sizeof(h->id), "%016llx", (unsigned long long)fnv1a(address));
			copy_text(h->address, sizeof(h->address), address);
			h->port = DEFAULT_PORT;
			log_msg("streamd: discovered %s at %s", name, address);
		}
		if (!h)
			continue;
		if (!h->name[0] || !h->saved)
			copy_text(h->name, sizeof(h->name), name);
		/* Moonlight's HTTP port is the advertised one (GameStream 47989). */
		if (port > 0)
			h->port = port;
		h->online = ONLINE_YES;
		changed = true;
		probe_host(h);
		from_len = sizeof(from);
	}
	if (changed)
		notify_subscribers();
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static struct host *add_host(const char *address)
{
	struct host *h = find_host_by_address(address);

	if (h) {
		if (!h->saved) {
			h->saved = true;
			save_hosts();
		}
		return h;
	}
	if (host_count >= MAX_HOSTS)
		return NULL;
	h = &hosts[host_count++];
	memset(h, 0, sizeof(*h));
	snprintf(h->id, sizeof(h->id), "%016llx", (unsigned long long)fnv1a(address));
	copy_text(h->name, sizeof(h->name), address);
	copy_text(h->address, sizeof(h->address), address);
	h->port = DEFAULT_PORT;
	h->saved = true;
	save_hosts();
	return h;
}

static void remove_host(struct host *h)
{
	int index = (int)(h - hosts);

	if (h->paired)
		(void)spawn_job(JOB_UNPAIR, h, NULL, false);
	if (!strcmp(apps_host, h->id)) {
		apps_host[0] = '\0';
		app_count = 0;
		apps_state = "none";
	}
	memmove(&hosts[index], &hosts[index + 1], (size_t)(host_count - index - 1) * sizeof(hosts[0]));
	host_count--;
	save_hosts();
}

static void start_pairing(struct host *h)
{
	unsigned int r = 0;
	char pin_arg[8];
	char *extra[] = { (char *)"-pin", pin_arg, NULL };

	if (getrandom(&r, sizeof(r), 0) != sizeof(r))
		r = (unsigned int)monotonic_ms();
	snprintf(pin_arg, sizeof(pin_arg), "%04u", r % 10000);
	if (spawn_job(JOB_PAIR, h, extra, false) < 0)
		return;
	copy_text(pairing_host, sizeof(pairing_host), h->id);
	copy_text(pairing_pin, sizeof(pairing_pin), pin_arg);
}

static void cancel_pairing(void)
{
	for (int i = 0; i < MAX_JOBS; i++) {
		if (jobs[i].kind != JOB_PAIR || jobs[i].term_sent)
			continue;
		jobs[i].term_sent = true;
		jobs[i].deadline_ms = monotonic_ms() + KILL_TIMEOUT_MS;
		kill(-jobs[i].pid, SIGTERM);
	}
}

static void handle_command(struct client *c, char *line)
{
	char *args[3] = { NULL, NULL, NULL };
	char *p;
	int argc = 0;
	struct host *h;

	trim(line);
	p = strchr(line, '\t');
	if (p) {
		*p++ = '\0';
		while (p && argc < 3) {
			args[argc++] = p;
			p = strchr(p, '\t');
			if (p)
				*p++ = '\0';
		}
	}

	if (!strcmp(line, "STATUS")) {
		send_status(c);
		return;
	}
	if (!strcmp(line, "SUBSCRIBE")) {
		c->subscribed = true;
		send_status(c);
		return;
	}
	if (!strcmp(line, "PRE_POWER") && argc == 1) {
		if (strcmp(args[0], "sleep") && strcmp(args[0], "restart") && strcmp(args[0], "poweroff")) {
			reply(c, "ERR action\n");
			return;
		}
		pairing_host[0] = '\0';
		cancel_pairing();
		if (stream.state == STREAM_IDLE) {
			reply(c, "OK\n");
			return;
		}
		if (stream.power_client >= 0) {
			reply(c, "ERR busy\n");
			return;
		}
		/* A stream cannot survive suspend (Wi-Fi goes down): it ends,
		 * the host application keeps running for the next stream. */
		stream.power_client = (int)(c - clients);
		request_stream_quit(false);
		return;
	}
	if (!strcmp(line, "STATS")) {
		if (stream.state != STREAM_RUNNING) {
			reply(c, "ERR no-stream\n");
			return;
		}
		c->wants_stats = true;
		c->stats_deadline_ms = monotonic_ms() + STATS_TIMEOUT_MS;
		job_command(stream.job, "STATS");
		return;
	}
	if (!strcmp(line, "QUIT")) {
		if (stream.state != STREAM_STARTING && stream.state != STREAM_RUNNING) {
			reply(c, "ERR no-stream\n");
			return;
		}
		request_stream_quit(argc == 1 && !strcmp(args[0], "close"));
		reply(c, "OK\n");
		return;
	}

	/* Everything else works on the active user's data. */
	if (!user[0]) {
		reply(c, "ERR no-user\n");
		return;
	}
	if (!strcmp(line, "DISCOVER")) {
		mdns_start();
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "ADD") && argc == 1) {
		if (!valid_address(args[0])) {
			reply(c, "ERR address\n");
			return;
		}
		h = add_host(args[0]);
		if (!h) {
			reply(c, "ERR full\n");
			return;
		}
		probe_host(h);
		{
			char text[64];

			snprintf(text, sizeof(text), "OK %s\n", h->id);
			reply(c, text);
		}
		notify_subscribers();
	} else if (!strcmp(line, "REMOVE") && argc == 1) {
		h = find_host(args[0]);
		if (!h) {
			reply(c, "ERR host\n");
			return;
		}
		if (stream.state != STREAM_IDLE && !strcmp(stream.host, h->id)) {
			reply(c, "ERR busy\n");
			return;
		}
		remove_host(h);
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "REFRESH")) {
		for (int i = 0; i < host_count; i++)
			if (argc == 0 || !strcmp(hosts[i].id, args[0]))
				probe_host(&hosts[i]);
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "PAIR") && argc == 1) {
		h = find_host(args[0]);
		if (!h) {
			reply(c, "ERR host\n");
			return;
		}
		if (job_running(JOB_PAIR, NULL) || stream.state != STREAM_IDLE) {
			reply(c, "ERR busy\n");
			return;
		}
		start_pairing(h);
		reply(c, pairing_host[0] ? "OK\n" : "ERR spawn\n");
		notify_subscribers();
	} else if (!strcmp(line, "PAIR_CANCEL")) {
		cancel_pairing();
		pairing_host[0] = '\0';
		pairing_pin[0] = '\0';
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "APPS") && argc == 1) {
		h = find_host(args[0]);
		if (!h) {
			reply(c, "ERR host\n");
			return;
		}
		if (!h->paired) {
			reply(c, "ERR unpaired\n");
			return;
		}
		if (strcmp(apps_host, h->id) != 0 || strcmp(apps_state, "loading") != 0) {
			copy_text(apps_host, sizeof(apps_host), h->id);
			app_count = 0;
			apps_state = spawn_job(JOB_APPS, h, NULL, false) >= 0 ? "loading" : "error";
		}
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "LAUNCH") && argc == 2) {
		const char *error = launch_stream(args[0], args[1]);

		if (error) {
			char text[64];

			log_msg("streamd: launch refused: %s", error);
			snprintf(text, sizeof(text), "ERR %s\n", error);
			reply(c, text);
			return;
		}
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "SET") && argc == 2) {
		if (!strcmp(args[0], "resolution") && valid_resolution(args[1]))
			copy_text(setting_resolution, sizeof(setting_resolution), args[1]);
		else if (!strcmp(args[0], "fps") && (atoi(args[1]) == 30 || atoi(args[1]) == 60))
			setting_fps = atoi(args[1]);
		else if (!strcmp(args[0], "codec") && valid_codec(args[1]))
			copy_text(setting_codec, sizeof(setting_codec), args[1]);
		else if (!strcmp(args[0], "bitrate") && atoi(args[1]) >= 0 && atoi(args[1]) <= 150000)
			setting_bitrate = atoi(args[1]);
		else {
			reply(c, "ERR setting\n");
			return;
		}
		save_settings();
		reply(c, "OK\n");
		notify_subscribers();
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
		memset(&clients[i], 0, sizeof(clients[i]));
		clients[i].fd = fd;
		return;
	}
	close(fd);
}

/* ------------------------------------------------------------------ */
/* Timers: only while something is pending                             */
/* ------------------------------------------------------------------ */

static int next_timeout_ms(void)
{
	long long now = monotonic_ms();
	long long deadline = -1;

#define CONSIDER(t) do { if ((t) > 0 && (deadline < 0 || (t) < deadline)) deadline = (t); } while (0)
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind != JOB_NONE)
			CONSIDER(jobs[i].deadline_ms);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].wants_stats)
			CONSIDER(clients[i].stats_deadline_ms);
	if (mdns_fd >= 0)
		CONSIDER(mdns_deadline_ms);
#undef CONSIDER
	if (deadline < 0)
		return -1;
	if (deadline <= now)
		return 0;
	return deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
}

static void run_timers(void)
{
	long long now = monotonic_ms();

	for (int i = 0; i < MAX_JOBS; i++) {
		struct job *j = &jobs[i];

		if (j->kind == JOB_NONE || j->deadline_ms <= 0 || now < j->deadline_ms)
			continue;
		if (!j->term_sent) {
			log_msg("streamd: moonlight pid=%d timed out, SIGTERM", (int)j->pid);
			kill(-j->pid, SIGTERM);
			j->term_sent = true;
			j->deadline_ms = now + KILL_TIMEOUT_MS;
		} else if (!j->kill_sent) {
			log_msg("streamd: moonlight pid=%d did not terminate, SIGKILL", (int)j->pid);
			kill(-j->pid, SIGKILL);
			j->kill_sent = true;
			j->deadline_ms = -1;
		}
	}
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd < 0 || !clients[i].wants_stats || now < clients[i].stats_deadline_ms)
			continue;
		clients[i].wants_stats = false;
		reply(&clients[i], "ERR timeout\n");
	}
	if (mdns_fd >= 0 && now >= mdns_deadline_ms) {
		mdns_stop();
		notify_subscribers();
	}
}

int main(void)
{
	struct sigaction sa;
	int server;
	int inotify_fd;
	FILE *pid_fp;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	for (int i = 0; i < MAX_JOBS; i++) {
		jobs[i].out_fd = -1;
		jobs[i].in_fd = -1;
	}
	reset_stream();

	if (mkdir_p(RUN_ROOT) != 0 || pipe2(sigchld_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		perror("nuubos-streamd: init");
		return 1;
	}
	chmod(RUN_ROOT, 0755);
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

	refresh_active_user();
	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0 &&
	    inotify_add_watch(inotify_fd, ACTIVE_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE) < 0)
		log_msg("streamd: active-user watch unavailable: %s", strerror(errno));

	server = make_server();
	if (server < 0) {
		perror("nuubos-streamd: socket");
		return 1;
	}
	pid_fp = fopen(PIDFILE, "w");
	if (pid_fp) {
		fprintf(pid_fp, "%ld\n", (long)getpid());
		fclose(pid_fp);
	}
	log_msg("streamd: started pid=%ld", (long)getpid());

	while (!stop_requested) {
		struct pollfd pfd[5 + MAX_JOBS + MAX_CLIENTS];
		int who[5 + MAX_JOBS + MAX_CLIENTS];
		nfds_t count = 0;
		int rc;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		if (inotify_fd >= 0) {
			pfd[count] = (struct pollfd){ inotify_fd, POLLIN, 0 };
			who[count++] = -3;
		}
		if (mdns_fd >= 0) {
			pfd[count] = (struct pollfd){ mdns_fd, POLLIN, 0 };
			who[count++] = -4;
		}
		if (stream.input_fd >= 0) {
			pfd[count] = (struct pollfd){ stream.input_fd, POLLIN, 0 };
			who[count++] = -5;
		}
		for (int i = 0; i < MAX_JOBS; i++) {
			if (jobs[i].kind == JOB_NONE || jobs[i].out_fd < 0)
				continue;
			pfd[count] = (struct pollfd){ jobs[i].out_fd, POLLIN, 0 };
			who[count++] = 1000 + i;
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			pfd[count] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
			who[count++] = i;
		}

		rc = poll(pfd, count, next_timeout_ms());
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		run_timers();
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
			case -3: {
				char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

				while (read(inotify_fd, events, sizeof(events)) > 0)
					;
				if (refresh_active_user()) {
					cancel_pairing();
					mdns_stop();
					notify_subscribers();
				}
				break;
			}
			case -4:
				if (mdns_fd >= 0)
					mdns_read();
				break;
			case -5:
				if (stream.input_fd >= 0)
					process_input_link();
				break;
			default:
				if (who[i] >= 1000) {
					int slot = who[i] - 1000;

					if (jobs[slot].kind != JOB_NONE && jobs[slot].out_fd >= 0)
						read_job(slot);
				} else if (clients[who[i]].fd >= 0) {
					process_client(&clients[who[i]]);
				}
				break;
			}
		}
	}

	/* Service stop (shutdown): end a running stream, bounded. */
	if (stream.state != STREAM_IDLE && stream.job >= 0) {
		long long deadline = monotonic_ms() + STREAM_QUIT_TIMEOUT_MS;
		pid_t pid = jobs[stream.job].pid;
		bool exited = false;

		job_command(stream.job, "QUIT");
		while (!exited && monotonic_ms() < deadline) {
			struct pollfd p = { sigchld_pipe[0], POLLIN, 0 };
			char drain[32];

			(void)poll(&p, 1, (int)(deadline - monotonic_ms()));
			while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
				;
			exited = waitpid(pid, NULL, WNOHANG) == pid;
		}
		if (!exited)
			kill(-pid, SIGKILL);
	}
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].kind != JOB_NONE && jobs[i].kind != JOB_STREAM)
			kill(-jobs[i].pid, SIGTERM);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
