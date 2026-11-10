/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-castd — Screen Casting Service (EPIC-029).
 *
 * Owns wireless casting of the console screen and sound to a TV or a
 * computer: receiver discovery, the running casting session and the
 * per-user casting preferences. The work runs in child processes
 * (nuubos-cast-session): a one-shot discovery while a client asks for it,
 * one casting session at a time. Idle = poll() without timeout.
 *
 * Protocols (the receiver list shows each one separately, with its delay):
 *   cast  Google Cast: Cast Streaming mirroring, low delay (Default Media
 *         Receiver fallback, a few seconds)
 *   mice  Miracast over Infrastructure (MS-MICE), low delay; needs Cisco's
 *         OpenH264, downloaded on the user's request (H264_INSTALL)
 *   dlna  DLNA/UPnP renderers, a few seconds of delay
 *
 * Protocol (/run/nuubos/castd.sock, one command per line, tab separated):
 * STATUS and SUBSCRIBE return the snapshot (SUBSCRIBE pushes a new one on
 * every change); DISCOVER, START <receiver>, STOP, SET <audio|latency>
 * <value>, H264_INSTALL, H264_CANCEL, H264_REMOVE reply OK or ERR <reason>
 * at once; PRE_POWER <sleep|restart|poweroff> replies once no session
 * runs. Snapshot lines:
 *   state=idle|connecting|casting|stopping
 *   receiver= receiver_name= protocol= delay=low|high fallback=0|1 waiting_tv=0|1
 *   discovering=0|1 user=0|1 audio=0|1 latency=low|normal|smooth
 *   h264=absent|installed h264_job=|installing|removing h264_progress= h264_error=
 *   rx=<id>\t<name>\t<protocol>\t<delay>\t<model>\t<ready>
 *   end=1
 * Outcomes the user must see are typed notifications (cast.*).
 *
 * Sound: the session creates the PipeWire sink "nuubos_cast"; nuubos-audiod
 * routes the console's sound to it while it exists (audio=1).
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#define RUN_ROOT "/run/nuubos"
#define SOCKET_PATH RUN_ROOT "/castd.sock"
#define PIDFILE RUN_ROOT "/castd.pid"
#define SESSION_LOG RUN_ROOT "/cast-session.log"
#define ACTIVE_DIR RUN_ROOT "/user"
#define ACTIVE_USER_FILE ACTIVE_DIR "/active"
#define USERS_DIR "/userdata/users"
#define STATE_USERS "/state/users"
#define MODEL_FILE "/proc/device-tree/model"
#define SESSION_BIN "/usr/lib/nuubos/nuubos-cast-session"
#define H264_GET "/usr/lib/nuubos/nuubos-openh264-get"
#define H264_DIR "/userdata/cast/openh264"
#define H264_LIB H264_DIR "/libopenh264.so"
#define WAYLAND_DISPLAY_PATH RUN_ROOT "/wayland-runtime/wayland-0"
#define PIPEWIRE_RUNTIME RUN_ROOT "/pipewire"

#define MAX_CLIENTS 16
#define MAX_RX 32
#define DISCOVER_SECONDS 4
#define STOP_TIMEOUT_MS 4000
#define KILL_TIMEOUT_MS 2000

enum session_state { S_IDLE, S_CONNECTING, S_CASTING, S_STOPPING };

struct receiver {
	char id[17];
	char protocol[8];
	char address[64];
	int port;
	char name[128];
	char model[96];
	char location[384];
	bool seen; /* in the last discovery */
};

struct child {
	pid_t pid;
	int out_fd;
	int in_fd;
	char buf[2048];
	size_t used;
	long long deadline_ms; /* kill after (stop requested), 0 = none */
	bool term_sent;
};

struct client {
	int fd;
	bool subscribed;
	bool waits_power;
	char buf[512];
	size_t used;
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];
static struct receiver rx[MAX_RX];
static int rx_count;

static char user[64];
static bool setting_audio = true;
static char setting_latency[8] = "normal";

static struct child discovery = { .pid = -1, .out_fd = -1, .in_fd = -1 };
static struct child session = { .pid = -1, .out_fd = -1, .in_fd = -1 };
static struct child h264 = { .pid = -1, .out_fd = -1, .in_fd = -1 };

static enum session_state sstate = S_IDLE;
static struct receiver current;
static char cur_delay[8];
static bool cur_fallback;
static bool cur_waiting_tv;
static char cur_error[32];
static bool user_stop;
static bool cur_started; /* reached casting */

static char h264_job[16];
static int h264_progress;
static char h264_error[16];
static bool h264_cancel;

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
		/* a wakeup is pending */
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

static uint64_t fnv1a(const char *text)
{
	uint64_t h = 1469598103934665603ULL;

	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

/* ------------------------------------------------------------------ */
/* Notifications (EPIC-006)                                            */
/* ------------------------------------------------------------------ */

static void notify(const char *event, const char *k1, const char *v1, const char *k2, const char *v2)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "cast", event);
	if (k1)
		nuubos_notify_str(&n, k1, v1);
	if (k2)
		nuubos_notify_str(&n, k2, v2);
	(void)nuubos_notify_send(&n);
}

/* ------------------------------------------------------------------ */
/* Per-user settings                                                   */
/* ------------------------------------------------------------------ */

static void settings_path(char *out, size_t size)
{
	snprintf(out, size, USERS_DIR "/%s/appdata/cast/settings.conf", user);
}

static void load_settings(void)
{
	char path[PATH_MAX], line[128];
	FILE *fp;

	setting_audio = true;
	copy_text(setting_latency, sizeof(setting_latency), "normal");
	if (!user[0])
		return;
	settings_path(path, sizeof(path));
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		trim(line);
		if (!strncmp(line, "AUDIO=", 6))
			setting_audio = strcmp(line + 6, "0") != 0;
		else if (!strncmp(line, "LATENCY=", 8) &&
			 (!strcmp(line + 8, "low") || !strcmp(line + 8, "normal") || !strcmp(line + 8, "smooth")))
			copy_text(setting_latency, sizeof(setting_latency), line + 8);
	}
	fclose(fp);
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

static int save_settings(void)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8], dir[PATH_MAX];
	FILE *fp;

	if (!user[0])
		return -1;
	snprintf(dir, sizeof(dir), USERS_DIR "/%s/appdata/cast", user);
	if (mkdir_p(dir) != 0)
		return -1;
	settings_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "AUDIO=%d\nLATENCY=%s\n", setting_audio ? 1 : 0, setting_latency);
	if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
		fclose(fp);
		unlink(tmp);
		return -1;
	}
	fclose(fp);
	return rename(tmp, path);
}

static int latency_ms(void)
{
	/* Cast Streaming playout delay: low suits playing, smooth absorbs a
	 * weak Wi-Fi. */
	if (!strcmp(setting_latency, "low"))
		return 120;
	if (!strcmp(setting_latency, "smooth"))
		return 400;
	return 250;
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
	for (const char *p = out; *p; p++)
		if (!isxdigit((unsigned char)*p) && *p != '-')
			return false;
	return out[0] != '\0';
}

static bool refresh_active_user(void)
{
	char now[64];

	if (!read_active_user(now, sizeof(now)))
		now[0] = '\0';
	if (!strcmp(now, user))
		return false;
	copy_text(user, sizeof(user), now);
	load_settings();
	log_msg("castd: active user %s", user[0] ? user : "(none)");
	return true;
}

/* "<profile name> · <model>", the name the receiver shows. */
static void console_name(char *out, size_t size)
{
	char path[PATH_MAX], line[160], name[96] = "", model[96] = "";
	FILE *fp;
	const char *m;

	snprintf(path, sizeof(path), STATE_USERS "/%s/profile.conf", user);
	fp = fopen(path, "r");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			trim(line);
			if (!strncmp(line, "USER_NAME=", 10)) {
				char *v = line + 10;
				size_t l = strlen(v);

				if (l >= 2 && v[0] == '"' && v[l - 1] == '"') {
					v[l - 1] = '\0';
					v++;
				}
				copy_text(name, sizeof(name), v);
			}
		}
		fclose(fp);
	}
	fp = fopen(MODEL_FILE, "r");
	if (fp) {
		size_t n = fread(model, 1, sizeof(model) - 1, fp);

		model[n] = '\0';
		fclose(fp);
	}
	model[strcspn(model, "\n")] = '\0';
	trim(model);
	m = strncmp(model, "Anbernic ", 9) ? model : model + 9;
	if (name[0] && m[0])
		snprintf(out, size, "%s · %s", name, m);
	else
		snprintf(out, size, "%s", name[0] ? name : m[0] ? m : "nuubOS");
}

/* ------------------------------------------------------------------ */
/* Snapshot                                                            */
/* ------------------------------------------------------------------ */

static bool h264_installed(void)
{
	return access(H264_LIB, R_OK) == 0;
}

static const char *protocol_delay(const struct receiver *r)
{
	return !strcmp(r->protocol, "dlna") ? "high" : "low";
}

static char *build_status(size_t *len_out)
{
	size_t cap = 2048 + (size_t)rx_count * 320;
	char *out = malloc(cap);
	size_t used = 0;
	static const char *const states[] = { "idle", "connecting", "casting", "stopping" };

#define APPEND(...) \
	do { \
		int _n = snprintf(out + used, cap - used, __VA_ARGS__); \
		if (_n > 0) \
			used += (size_t)_n < cap - used ? (size_t)_n : cap - used - 1; \
	} while (0)

	if (!out)
		return NULL;
	out[0] = '\0';
	APPEND("state=%s\n", states[sstate]);
	APPEND("receiver=%s\n", sstate == S_IDLE ? "" : current.id);
	APPEND("receiver_name=%s\n", sstate == S_IDLE ? "" : current.name);
	APPEND("protocol=%s\n", sstate == S_IDLE ? "" : current.protocol);
	APPEND("delay=%s\n", sstate == S_IDLE ? "" : cur_delay);
	APPEND("fallback=%d\n", sstate != S_IDLE && cur_fallback ? 1 : 0);
	APPEND("waiting_tv=%d\n", sstate == S_CONNECTING && cur_waiting_tv ? 1 : 0);
	APPEND("discovering=%d\n", discovery.pid > 0 ? 1 : 0);
	APPEND("user=%d\n", user[0] ? 1 : 0);
	APPEND("audio=%d\n", setting_audio ? 1 : 0);
	APPEND("latency=%s\n", setting_latency);
	APPEND("h264=%s\n", h264_installed() ? "installed" : "absent");
	APPEND("h264_job=%s\n", h264_job);
	APPEND("h264_progress=%d\n", h264_progress);
	APPEND("h264_error=%s\n", h264_error);
	for (int i = 0; i < rx_count; i++) {
		bool ready = strcmp(rx[i].protocol, "mice") || h264_installed();

		APPEND("rx=%s\t%s\t%s\t%s\t%s\t%d\n", rx[i].id, rx[i].name, rx[i].protocol, protocol_delay(&rx[i]),
		       rx[i].model, ready ? 1 : 0);
	}
	APPEND("end=1\n");
#undef APPEND
	*len_out = used;
	return out;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
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

	if (!status || write_all(c->fd, status, len) != 0)
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
/* Children                                                            */
/* ------------------------------------------------------------------ */

static void child_env(void)
{
	setenv("WAYLAND_DISPLAY", WAYLAND_DISPLAY_PATH, 1);
	setenv("XDG_RUNTIME_DIR", RUN_ROOT "/wayland-runtime", 1);
	setenv("PIPEWIRE_RUNTIME_DIR", PIPEWIRE_RUNTIME, 1);
}

static int spawn(struct child *c, char *const argv[], bool with_stdin, const char *log_path)
{
	int out[2], in[2] = { -1, -1 };
	pid_t pid;

	if (pipe2(out, O_CLOEXEC) != 0)
		return -1;
	if (with_stdin && pipe2(in, O_CLOEXEC) != 0) {
		close(out[0]);
		close(out[1]);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(out[0]);
		close(out[1]);
		if (in[0] >= 0) {
			close(in[0]);
			close(in[1]);
		}
		return -1;
	}
	if (pid == 0) {
		int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

		setpgid(0, 0);
		dup2(out[1], STDOUT_FILENO);
		if (log_fd >= 0)
			dup2(log_fd, STDERR_FILENO);
		if (with_stdin)
			dup2(in[0], STDIN_FILENO);
		else {
			int null_fd = open("/dev/null", O_RDONLY);

			if (null_fd >= 0)
				dup2(null_fd, STDIN_FILENO);
		}
		signal(SIGPIPE, SIG_DFL);
		child_env();
		execv(argv[0], argv);
		_exit(127);
	}
	close(out[1]);
	if (with_stdin)
		close(in[0]);
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	c->pid = pid;
	c->out_fd = out[0];
	c->in_fd = with_stdin ? in[1] : -1;
	c->used = 0;
	c->deadline_ms = 0;
	c->term_sent = false;
	return 0;
}

static void child_close(struct child *c)
{
	if (c->out_fd >= 0)
		close(c->out_fd);
	if (c->in_fd >= 0)
		close(c->in_fd);
	c->out_fd = c->in_fd = -1;
	c->pid = -1;
}

static void child_command(struct child *c, const char *cmd)
{
	if (c->in_fd >= 0 && write_all(c->in_fd, cmd, strlen(cmd)) != 0) {
		close(c->in_fd);
		c->in_fd = -1;
	}
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

static void start_discovery(void)
{
	char seconds[8];
	char *argv[] = { (char *)SESSION_BIN, (char *)"discover", seconds, NULL };

	if (discovery.pid > 0)
		return;
	snprintf(seconds, sizeof(seconds), "%d", DISCOVER_SECONDS);
	for (int i = 0; i < rx_count; i++)
		rx[i].seen = false;
	if (spawn(&discovery, argv, false, RUN_ROOT "/cast-discover.log") != 0)
		log_msg("castd: cannot start the discovery");
}

static void discovery_line(char *line)
{
	char *f[6] = { 0 };
	int n = 0;
	struct receiver *r = NULL;
	char key[512];

	if (strncmp(line, "@rx ", 4))
		return;
	line += 4;
	for (char *p = line; n < 6; n++) {
		f[n] = p;
		p = strchr(p, '\t');
		if (!p) {
			n++;
			break;
		}
		*p++ = '\0';
	}
	if (n < 6)
		return;
	/* Identity: protocol + address (+ description for DLNA, a device may
	 * host several renderers). */
	snprintf(key, sizeof(key), "%s|%s|%s", f[0], f[1], !strcmp(f[0], "dlna") ? f[5] : f[2]);
	for (int i = 0; i < rx_count; i++)
		if (!strcmp(rx[i].protocol, f[0]) && !strcmp(rx[i].address, f[1]) &&
		    (strcmp(f[0], "dlna") || !strcmp(rx[i].location, f[5])))
			r = &rx[i];
	if (!r) {
		if (rx_count >= MAX_RX)
			return;
		r = &rx[rx_count++];
		memset(r, 0, sizeof(*r));
		snprintf(r->id, sizeof(r->id), "%016llx", (unsigned long long)fnv1a(key));
	}
	copy_text(r->protocol, sizeof(r->protocol), f[0]);
	copy_text(r->address, sizeof(r->address), f[1]);
	r->port = atoi(f[2]);
	copy_text(r->name, sizeof(r->name), f[3]);
	copy_text(r->model, sizeof(r->model), f[4]);
	copy_text(r->location, sizeof(r->location), f[5]);
	r->seen = true;
	notify_subscribers();
}

static void discovery_done(void)
{
	/* Receivers that did not answer this time are gone (switched off);
	 * the one in use stays. */
	int kept = 0;

	for (int i = 0; i < rx_count; i++)
		if (rx[i].seen || (sstate != S_IDLE && !strcmp(rx[i].id, current.id)))
			rx[kept++] = rx[i];
	rx_count = kept;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static const char *start_session(const char *id)
{
	struct receiver *r = NULL;
	char port[8], latency[8], name[128];
	char *argv[16];
	int n = 0;

	if (!user[0])
		return "no-user";
	if (sstate != S_IDLE)
		return "busy";
	for (int i = 0; i < rx_count; i++)
		if (!strcmp(rx[i].id, id))
			r = &rx[i];
	if (!r)
		return "unknown-receiver";
	if (!strcmp(r->protocol, "mice") && !h264_installed())
		return "no-encoder";
	console_name(name, sizeof(name));
	snprintf(port, sizeof(port), "%d", r->port);
	snprintf(latency, sizeof(latency), "%d", latency_ms());
	argv[n++] = (char *)SESSION_BIN;
	argv[n++] = r->protocol;
	if (!strcmp(r->protocol, "cast")) {
		argv[n++] = r->address;
		argv[n++] = port;
	} else if (!strcmp(r->protocol, "dlna")) {
		argv[n++] = r->location;
	} else {
		argv[n++] = r->address;
	}
	argv[n++] = r->name;
	if (setting_audio)
		argv[n++] = (char *)"--audio";
	argv[n++] = (char *)"--latency";
	argv[n++] = latency;
	argv[n++] = (char *)"--console";
	argv[n++] = name;
	if (!strcmp(r->protocol, "mice")) {
		argv[n++] = (char *)"--openh264";
		argv[n++] = (char *)H264_LIB;
	}
	argv[n] = NULL;
	if (spawn(&session, argv, true, SESSION_LOG) != 0)
		return "start";
	current = *r;
	copy_text(cur_delay, sizeof(cur_delay), protocol_delay(r));
	cur_fallback = false;
	cur_waiting_tv = false;
	cur_error[0] = '\0';
	user_stop = false;
	cur_started = false;
	sstate = S_CONNECTING;
	log_msg("castd: casting to %s (%s %s)", r->name, r->protocol, r->address);
	return NULL;
}

static void stop_session(void)
{
	if (sstate == S_IDLE || sstate == S_STOPPING)
		return;
	user_stop = true;
	sstate = S_STOPPING;
	child_command(&session, "STOP\n");
	session.deadline_ms = monotonic_ms() + STOP_TIMEOUT_MS;
}

static void session_line(char *line)
{
	if (!strcmp(line, "@state casting")) {
		cur_started = true;
		if (sstate == S_CONNECTING) {
			sstate = S_CASTING;
			notify("cast.started", "name", current.name, "delay", cur_delay);
		}
	} else if (!strncmp(line, "@info delay=", 12)) {
		copy_text(cur_delay, sizeof(cur_delay), line + 12);
	} else if (!strcmp(line, "@info fallback=1")) {
		cur_fallback = true;
	} else if (!strcmp(line, "@info waiting=tv")) {
		cur_waiting_tv = true;
	} else if (!strncmp(line, "@error ", 7)) {
		copy_text(cur_error, sizeof(cur_error), line + 7);
	} else {
		return;
	}
	notify_subscribers();
}

static void finish_power_waits(void)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].waits_power) {
			clients[i].waits_power = false;
			reply(&clients[i], "OK\n");
		}
}

static void session_ended(void)
{
	log_msg("castd: session ended (%s)", cur_error[0] ? cur_error : user_stop ? "user" : "done");
	if (!user_stop) {
		/* Ended on the receiver (stopped) or lost (network) once it
		 * ran; otherwise it never started. */
		if (cur_started)
			notify("cast.ended", "name", current.name, "reason",
			       !strcmp(cur_error, "network") ? "network" : "stopped");
		else
			notify("cast.failed", "reason", cur_error[0] ? cur_error : "refused", "name", current.name);
	}
	sstate = S_IDLE;
	memset(&current, 0, sizeof(current));
	finish_power_waits();
}

/* ------------------------------------------------------------------ */
/* OpenH264 (Cisco binary, on the user's request)                      */
/* ------------------------------------------------------------------ */

static const char *start_h264(const char *action)
{
	char *argv[] = { (char *)H264_GET, (char *)action, (char *)H264_DIR, NULL };

	if (h264.pid > 0)
		return "busy";
	if (sstate != S_IDLE && !strcmp(action, "remove"))
		return "busy";
	if (spawn(&h264, argv, false, RUN_ROOT "/cast-openh264.log") != 0)
		return "start";
	copy_text(h264_job, sizeof(h264_job), !strcmp(action, "install") ? "installing" : "removing");
	h264_progress = 0;
	h264_error[0] = '\0';
	h264_cancel = false;
	return NULL;
}

static void h264_line(char *line)
{
	if (!strncmp(line, "@progress ", 10))
		h264_progress = atoi(line + 10);
	else if (!strncmp(line, "@error ", 7))
		copy_text(h264_error, sizeof(h264_error), line + 7);
	else
		return;
	notify_subscribers();
}

static void h264_finished(int status)
{
	bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	if (!strcmp(h264_job, "installing") && !h264_cancel) {
		if (ok)
			notify("cast.encoder.installed", NULL, NULL, NULL, NULL);
		else
			notify("cast.encoder.failed", "reason", h264_error[0] ? h264_error : "network", NULL, NULL);
	}
	if (h264_cancel)
		h264_error[0] = '\0';
	h264_job[0] = '\0';
	h264_progress = 0;
}

/* ------------------------------------------------------------------ */
/* Child output and exit                                               */
/* ------------------------------------------------------------------ */

static void read_child(struct child *c, void (*on_line)(char *))
{
	for (;;) {
		ssize_t n = read(c->out_fd, c->buf + c->used, sizeof(c->buf) - 1 - c->used);
		char *start, *nl;

		if (n <= 0)
			return;
		c->used += (size_t)n;
		c->buf[c->used] = '\0';
		start = c->buf;
		while ((nl = strchr(start, '\n')) != NULL) {
			*nl = '\0';
			on_line(start);
			start = nl + 1;
		}
		c->used = strlen(start);
		memmove(c->buf, start, c->used);
		if (c->used >= sizeof(c->buf) - 1)
			c->used = 0;
	}
}

static void reap_children(void)
{
	char drain[64];
	int status;
	pid_t pid;

	while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
		;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == discovery.pid) {
			if (discovery.out_fd >= 0)
				read_child(&discovery, discovery_line);
			child_close(&discovery);
			discovery_done();
		} else if (pid == session.pid) {
			if (session.out_fd >= 0)
				read_child(&session, session_line);
			child_close(&session);
			session_ended();
		} else if (pid == h264.pid) {
			if (h264.out_fd >= 0)
				read_child(&h264, h264_line);
			child_close(&h264);
			h264_finished(status);
		} else {
			continue;
		}
		notify_subscribers();
	}
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void handle_command(struct client *c, char *line)
{
	char *arg = strchr(line, '\t');
	char *arg2 = NULL;
	const char *err;

	trim(line);
	if (arg) {
		*arg++ = '\0';
		arg2 = strchr(arg, '\t');
		if (arg2)
			*arg2++ = '\0';
	}
	if (!strcmp(line, "STATUS")) {
		send_status(c);
	} else if (!strcmp(line, "SUBSCRIBE")) {
		c->subscribed = true;
		send_status(c);
	} else if (!strcmp(line, "DISCOVER")) {
		start_discovery();
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "START") && arg) {
		err = start_session(arg);
		if (err) {
			char msg[64];

			snprintf(msg, sizeof(msg), "ERR %s\n", err);
			reply(c, msg);
		} else {
			reply(c, "OK\n");
			notify_subscribers();
		}
	} else if (!strcmp(line, "STOP")) {
		stop_session();
		reply(c, "OK\n");
		notify_subscribers();
	} else if (!strcmp(line, "SET") && arg && arg2) {
		if (!strcmp(arg, "audio") && (!strcmp(arg2, "0") || !strcmp(arg2, "1"))) {
			setting_audio = arg2[0] == '1';
		} else if (!strcmp(arg, "latency") &&
			   (!strcmp(arg2, "low") || !strcmp(arg2, "normal") || !strcmp(arg2, "smooth"))) {
			copy_text(setting_latency, sizeof(setting_latency), arg2);
		} else {
			reply(c, "ERR invalid\n");
			return;
		}
		/* Applies from the next session. */
		reply(c, save_settings() == 0 ? "OK\n" : "ERR write\n");
		notify_subscribers();
	} else if (!strcmp(line, "H264_INSTALL") || !strcmp(line, "H264_REMOVE")) {
		err = start_h264(!strcmp(line, "H264_INSTALL") ? "install" : "remove");
		if (err) {
			char msg[64];

			snprintf(msg, sizeof(msg), "ERR %s\n", err);
			reply(c, msg);
		} else {
			reply(c, "OK\n");
			notify_subscribers();
		}
	} else if (!strcmp(line, "H264_CANCEL")) {
		if (h264.pid > 0) {
			h264_cancel = true;
			kill(-h264.pid, SIGTERM);
		}
		reply(c, "OK\n");
	} else if (!strcmp(line, "PRE_POWER") && arg) {
		/* Sleep turns Wi-Fi off; Restart/Power Off end everything:
		 * the receiver is released properly first. */
		if (sstate == S_IDLE) {
			reply(c, "OK\n");
		} else {
			c->waits_power = true;
			stop_session();
			notify_subscribers();
		}
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
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || chmod(SOCKET_PATH, 0660) != 0 ||
	    listen(fd, 8) != 0) {
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

/* Timers exist only while a stopped session is still exiting. */
static int next_timeout_ms(void)
{
	long long now = monotonic_ms();

	if (session.pid <= 0 || !session.deadline_ms)
		return -1;
	return session.deadline_ms <= now ? 0 : (int)(session.deadline_ms - now);
}

static void run_timers(void)
{
	if (session.pid <= 0 || !session.deadline_ms || monotonic_ms() < session.deadline_ms)
		return;
	if (!session.term_sent) {
		log_msg("castd: session did not stop, SIGTERM");
		kill(-session.pid, SIGTERM);
		session.term_sent = true;
		session.deadline_ms = monotonic_ms() + KILL_TIMEOUT_MS;
	} else {
		log_msg("castd: session did not terminate, SIGKILL");
		kill(-session.pid, SIGKILL);
		session.deadline_ms = 0;
	}
}

int main(void)
{
	struct sigaction sa;
	int server, inotify_fd;
	FILE *pid_fp;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	if (mkdir_p(RUN_ROOT) != 0 || pipe2(sigchld_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		perror("nuubos-castd: init");
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

	refresh_active_user();
	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0 &&
	    inotify_add_watch(inotify_fd, ACTIVE_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE) < 0)
		log_msg("castd: active-user watch unavailable: %s", strerror(errno));
	server = make_server();
	if (server < 0) {
		perror("nuubos-castd: socket");
		return 1;
	}
	pid_fp = fopen(PIDFILE, "w");
	if (pid_fp) {
		fprintf(pid_fp, "%ld\n", (long)getpid());
		fclose(pid_fp);
	}
	log_msg("castd: started pid=%ld", (long)getpid());

	while (!stop_requested) {
		struct pollfd pfd[6 + MAX_CLIENTS];
		int who[6 + MAX_CLIENTS];
		nfds_t count = 0;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		if (inotify_fd >= 0) {
			pfd[count] = (struct pollfd){ inotify_fd, POLLIN, 0 };
			who[count++] = -3;
		}
		if (discovery.out_fd >= 0) {
			pfd[count] = (struct pollfd){ discovery.out_fd, POLLIN, 0 };
			who[count++] = -4;
		}
		if (session.out_fd >= 0) {
			pfd[count] = (struct pollfd){ session.out_fd, POLLIN, 0 };
			who[count++] = -5;
		}
		if (h264.out_fd >= 0) {
			pfd[count] = (struct pollfd){ h264.out_fd, POLLIN, 0 };
			who[count++] = -6;
		}
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			pfd[count] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
			who[count++] = i;
		}
		if (poll(pfd, count, next_timeout_ms()) < 0) {
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
					/* A cast belongs to the user who started it. */
					stop_session();
					notify_subscribers();
				}
				break;
			}
			case -4:
				if (discovery.out_fd >= 0)
					read_child(&discovery, discovery_line);
				break;
			case -5:
				if (session.out_fd >= 0)
					read_child(&session, session_line);
				break;
			case -6:
				if (h264.out_fd >= 0)
					read_child(&h264, h264_line);
				break;
			default:
				if (clients[who[i]].fd >= 0)
					process_client(&clients[who[i]]);
				break;
			}
		}
	}

	/* Service stop: release the receiver, bounded. */
	if (session.pid > 0) {
		long long deadline = monotonic_ms() + STOP_TIMEOUT_MS;

		child_command(&session, "STOP\n");
		while (monotonic_ms() < deadline && waitpid(session.pid, NULL, WNOHANG) == 0)
			usleep(50000);
		if (waitpid(session.pid, NULL, WNOHANG) == 0)
			kill(-session.pid, SIGKILL);
	}
	if (discovery.pid > 0)
		kill(-discovery.pid, SIGTERM);
	if (h264.pid > 0)
		kill(-h264.pid, SIGTERM);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
