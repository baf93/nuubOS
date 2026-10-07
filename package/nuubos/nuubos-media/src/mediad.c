/* SPDX-License-Identifier: MIT */
/*
 * nuubos-mediad — Media playback service (EPIC-027, EPIC-028).
 *
 * Owns the one playback session: mpv fullscreen on Wayland with Cedrus
 * decoding (--hwdec=drm, dmabuf presentation), controlled over a JSON IPC
 * socket pair inherited by mpv (--input-ipc-client=fd://3), so there is no
 * connect retry. nuubUI sends the user's actions here; nothing else talks
 * to mpv.
 *
 * Protocol (/run/nuubos/mediad.sock, tab separated):
 *   STATUS | SUBSCRIBE   state=idle|playing|paused title= kind=video|audio
 *                        index= count= item= last_item= last_position= end=1
 *   PLAY <path|url> [start seconds]     one item (a video, a stream)
 *   PLAYDIR <dir> <file>                the folder's audio files, from <file>
 *   PAUSE | SEEK <+-seconds> | NEXT | PREV | SUB | AUDIO | STOP | PRE_POWER
 * Replies OK / ERR <reason>.
 *
 * Resume: mpv's watch-later folder per user (appdata/media/watch-later)
 * restores the position of local files; STOP reads the position first and
 * publishes it (last_item/last_position) for clients that report progress
 * to a server (Jellyfin). Idle: poll() without timeout; a timer exists only
 * while a stop is waiting for mpv to quit.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <nuubos/notify.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef MPV_BIN
#define MPV_BIN "/usr/bin/mpv"
#endif
#ifndef USERDATA_USERS
#define USERDATA_USERS "/userdata/users"
#endif
#define SOCKET_PATH RUN_ROOT "/mediad.sock"
#define PIDFILE RUN_ROOT "/mediad.pid"
#define ACTIVE_USER_FILE RUN_ROOT "/user/active"
#define PLAYLIST_FILE RUN_ROOT "/media-playlist.m3u"
#define MPV_LOG RUN_ROOT "/mpv.log"

#define MAX_CLIENTS 16
#define MAX_LINE 4096
#define QUIT_TIMEOUT_MS 5000
#define POSITION_TIMEOUT_MS 1000

enum state { IDLE, PLAYING, PAUSED, STOPPING };

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

struct session {
	enum state state;
	pid_t pid;
	int ipc;
	char in[65536];
	size_t in_used;
	char title[512];
	char item[PATH_MAX];
	bool video;
	long index;
	long count;
	bool started;
	long long deadline_ms;
	bool term_sent;
	int power_client;
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];
static struct session s = { .ipc = -1, .pid = -1, .power_client = -1 };
static char last_item[PATH_MAX];
static double last_position = -1;

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("mediad: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void on_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void on_sigchld(int sig)
{
	int saved = errno;
	char b = 1;

	(void)sig;
	(void)!write(sigchld_pipe[1], &b, 1);
	errno = saved;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

/* Values for the line protocol: no tabs or line breaks. */
static void flatten(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static void mkdir_p(const char *path)
{
	char tmp[PATH_MAX];

	copy_text(tmp, sizeof(tmp), path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

static bool read_user(char *out, size_t size)
{
	FILE *fp = fopen(ACTIVE_USER_FILE, "r");

	out[0] = '\0';
	if (!fp)
		return false;
	if (fgets(out, (int)size, fp))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(fp);
	for (char *p = out; *p; p++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-'))
			return false;
	return out[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* Clients                                                             */
/* ------------------------------------------------------------------ */

static const char *state_name(void)
{
	switch (s.state) {
	case PLAYING: return "playing";
	case PAUSED: return "paused";
	case STOPPING: return "stopping";
	default: return "idle";
	}
}

static size_t build_status(char *out, size_t size)
{
	int n = snprintf(out, size,
			 "state=%s\ntitle=%s\nkind=%s\nindex=%ld\ncount=%ld\nitem=%s\n"
			 "last_item=%s\nlast_position=%.0f\nend=1\n",
			 state_name(), s.state == IDLE ? "" : s.title,
			 s.state == IDLE ? "" : (s.video ? "video" : "audio"),
			 s.index, s.count, s.state == IDLE ? "" : s.item, last_item,
			 last_position < 0 ? -1.0 : last_position);

	return n < 0 ? 0 : (size_t)n < size ? (size_t)n : size - 1;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->used = 0;
}

static void reply(struct client *c, const char *text)
{
	if (c->fd >= 0 && send(c->fd, text, strlen(text), MSG_NOSIGNAL) < 0)
		close_client(c);
}

static void publish(void)
{
	char buf[2 * PATH_MAX + 1024];
	size_t len = build_status(buf, sizeof(buf));

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed &&
		    send(clients[i].fd, buf, len, MSG_NOSIGNAL) < 0)
			close_client(&clients[i]);
}

/* ------------------------------------------------------------------ */
/* mpv                                                                 */
/* ------------------------------------------------------------------ */

static void mpv_send(const char *json)
{
	size_t len = strlen(json);

	if (s.ipc < 0)
		return;
	if (write(s.ipc, json, len) != (ssize_t)len || write(s.ipc, "\n", 1) != 1)
		log_msg("ipc write failed: %s", strerror(errno));
}

static void mpv_command(cJSON *args)
{
	cJSON *root = cJSON_CreateObject();
	char *text;

	cJSON_AddItemToObject(root, "command", args);
	text = cJSON_PrintUnformatted(root);
	if (text)
		mpv_send(text);
	free(text);
	cJSON_Delete(root);
}

static void observe(int id, const char *property)
{
	cJSON *a = cJSON_CreateArray();

	cJSON_AddItemToArray(a, cJSON_CreateString("observe_property"));
	cJSON_AddItemToArray(a, cJSON_CreateNumber(id));
	cJSON_AddItemToArray(a, cJSON_CreateString(property));
	mpv_command(a);
}

static void notify_failed(void)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "media", "media.failed");
	(void)nuubos_notify_send(&n);
}

static void handle_mpv_line(const char *line)
{
	cJSON *msg = cJSON_Parse(line);
	const cJSON *ev, *name, *data, *id;
	bool changed = false;

	if (!msg)
		return;
	ev = cJSON_GetObjectItemCaseSensitive(msg, "event");
	id = cJSON_GetObjectItemCaseSensitive(msg, "request_id");
	data = cJSON_GetObjectItemCaseSensitive(msg, "data");
	if (cJSON_IsNumber(id) && id->valueint == 100) {
		/* Position asked by STOP: publish it, then quit. */
		if (cJSON_IsNumber(data)) {
			last_position = data->valuedouble;
			copy_text(last_item, sizeof(last_item), s.item);
		}
		mpv_send("{\"command\":[\"quit-watch-later\"]}");
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
	} else if (cJSON_IsString(ev) && !strcmp(ev->valuestring, "property-change")) {
		name = cJSON_GetObjectItemCaseSensitive(msg, "name");
		if (!cJSON_IsString(name)) {
			cJSON_Delete(msg);
			return;
		}
		if (!strcmp(name->valuestring, "pause") && cJSON_IsBool(data) && s.state != STOPPING) {
			s.state = cJSON_IsTrue(data) ? PAUSED : PLAYING;
			changed = true;
		} else if (!strcmp(name->valuestring, "media-title") && cJSON_IsString(data)) {
			copy_text(s.title, sizeof(s.title), data->valuestring);
			flatten(s.title);
			changed = true;
		} else if (!strcmp(name->valuestring, "path") && cJSON_IsString(data)) {
			copy_text(s.item, sizeof(s.item), data->valuestring);
			flatten(s.item);
			s.started = true;
			changed = true;
		} else if (!strcmp(name->valuestring, "playlist-pos") && cJSON_IsNumber(data)) {
			s.index = (long)data->valuedouble;
			changed = true;
		} else if (!strcmp(name->valuestring, "playlist-count") && cJSON_IsNumber(data)) {
			s.count = (long)data->valuedouble;
			changed = true;
		} else if (!strcmp(name->valuestring, "vid")) {
			/* Album art counts as audio: only a real video track is video. */
			s.video = cJSON_IsNumber(data) || (cJSON_IsString(data) && strcmp(data->valuestring, "no"));
			changed = true;
		}
	}
	cJSON_Delete(msg);
	if (changed)
		publish();
}

static void process_ipc(void)
{
	ssize_t n = read(s.ipc, s.in + s.in_used, sizeof(s.in) - s.in_used - 1);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close(s.ipc);
		s.ipc = -1;
		return;
	}
	s.in_used += (size_t)n;
	s.in[s.in_used] = '\0';
	while ((nl = strchr(s.in, '\n'))) {
		*nl = '\0';
		handle_mpv_line(s.in);
		memmove(s.in, nl + 1, s.in_used - (size_t)(nl + 1 - s.in) + 1);
		s.in_used -= (size_t)(nl + 1 - s.in);
	}
	if (s.in_used >= sizeof(s.in) - 1)
		s.in_used = 0;
}

static bool audio_name(const char *name)
{
	static const char *const exts[] = { ".mp3", ".flac", ".ogg", ".opus", ".m4a", ".aac", ".wav", ".alac", NULL };
	size_t len = strlen(name);

	for (int i = 0; exts[i]; i++) {
		size_t e = strlen(exts[i]);

		if (len > e && !strcasecmp(name + len - e, exts[i]))
			return true;
	}
	return false;
}

static int name_cmp(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* The folder's audio files, sorted, into an m3u; *start = index of file. */
static bool write_playlist(const char *dir, const char *first, long *start)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char **names = NULL;
	size_t n = 0, cap = 0;
	FILE *fp;

	*start = 0;
	if (!d)
		return false;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !audio_name(e->d_name) || strpbrk(e->d_name, "\r\n"))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			names = realloc(names, cap * sizeof(*names));
			if (!names)
				abort();
		}
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof(*names), name_cmp);
	fp = fopen(PLAYLIST_FILE, "w");
	if (!fp) {
		for (size_t i = 0; i < n; i++)
			free(names[i]);
		free(names);
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		fprintf(fp, "%s/%s\n", dir, names[i]);
		if (!strcmp(names[i], first))
			*start = (long)i;
		free(names[i]);
	}
	free(names);
	return fclose(fp) == 0 && n > 0;
}

static const char *start_mpv(const char *target, bool playlist, long start_index, double start_seconds)
{
	char user[64], home[PATH_MAX], later[PATH_MAX], arg_later[PATH_MAX + 32];
	char arg_start[64], arg_pos[64];
	int sv[2];
	pid_t pid;

	if (s.state != IDLE)
		return "busy";
	if (!read_user(user, sizeof(user)))
		return "no-user";
	snprintf(home, sizeof(home), USERDATA_USERS "/%s/appdata/media", user);
	snprintf(later, sizeof(later), "%s/watch-later", home);
	mkdir_p(later);
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
		return "spawn";
	snprintf(arg_later, sizeof(arg_later), "--watch-later-dir=%s", later);
	snprintf(arg_start, sizeof(arg_start), "--start=%.0f", start_seconds);
	snprintf(arg_pos, sizeof(arg_pos), "--playlist-start=%ld", start_index);
	pid = fork();
	if (pid < 0) {
		close(sv[0]);
		close(sv[1]);
		return "spawn";
	}
	if (pid == 0) {
		char playlist_arg[PATH_MAX + 16];
		char *argv[32];
		int argc = 0, log;
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		dup2(sv[1], 3);
		log = open(MPV_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		clearenv();
		setenv("PATH", "/usr/bin:/bin", 1);
		setenv("HOME", home, 1);
		setenv("XDG_RUNTIME_DIR", RUN_ROOT "/wayland-runtime", 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		setenv("PIPEWIRE_RUNTIME_DIR", RUN_ROOT "/pipewire", 1);
		argv[argc++] = (char *)MPV_BIN;
		argv[argc++] = (char *)"--input-ipc-client=fd://3";
		argv[argc++] = (char *)"--no-config";
		argv[argc++] = (char *)"--no-terminal";
		argv[argc++] = (char *)"--fullscreen";
		argv[argc++] = (char *)"--force-window=yes";
		argv[argc++] = (char *)"--hwdec=drm";
		argv[argc++] = (char *)"--vo=dmabuf-wayland,gpu";
		argv[argc++] = (char *)"--gpu-context=wayland";
		argv[argc++] = (char *)"--ao=pipewire,alsa";
		argv[argc++] = (char *)"--input-default-bindings=no";
		argv[argc++] = (char *)"--input-vo-keyboard=no";
		argv[argc++] = (char *)"--osd-level=1";
		argv[argc++] = (char *)"--save-position-on-quit=yes";
		argv[argc++] = (char *)"--resume-playback=yes";
		argv[argc++] = arg_later;
		argv[argc++] = (char *)"--sub-auto=fuzzy";
		argv[argc++] = (char *)"--idle=no";
		if (start_seconds > 0)
			argv[argc++] = arg_start;
		if (playlist) {
			snprintf(playlist_arg, sizeof(playlist_arg), "--playlist=%s", target);
			argv[argc++] = arg_pos;
			argv[argc++] = playlist_arg;
		} else {
			argv[argc++] = (char *)"--";
			argv[argc++] = (char *)target;
		}
		argv[argc] = NULL;
		execv(MPV_BIN, argv);
		_exit(127);
	}
	close(sv[1]);
	s.pid = pid;
	s.ipc = sv[0];
	(void)fcntl(s.ipc, F_SETFL, O_NONBLOCK);
	s.state = PLAYING;
	s.in_used = 0;
	s.title[0] = s.item[0] = '\0';
	s.video = false;
	s.index = 0;
	s.count = 1;
	s.started = false;
	s.term_sent = false;
	s.deadline_ms = -1;
	last_item[0] = '\0';
	last_position = -1;
	observe(1, "pause");
	observe(2, "media-title");
	observe(3, "path");
	observe(4, "playlist-pos");
	observe(5, "playlist-count");
	observe(6, "vid");
	log_msg("playing %s (pid %d)", target, (int)pid);
	publish();
	return NULL;
}

static void stop_session(void)
{
	if (s.state == IDLE || s.state == STOPPING)
		return;
	s.state = STOPPING;
	mpv_send("{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":100}");
	/* No answer (nothing playing yet): quit anyway after a short wait. */
	s.deadline_ms = now_ms() + POSITION_TIMEOUT_MS;
	publish();
}

static void child_exited(int status)
{
	bool failed = !s.started || (WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
				     WEXITSTATUS(status) != 4) || WIFSIGNALED(status);

	log_msg("mpv exited status=%d", status);
	if (failed && s.state != STOPPING)
		notify_failed();
	if (s.ipc >= 0)
		close(s.ipc);
	s.ipc = -1;
	s.pid = -1;
	s.state = IDLE;
	s.deadline_ms = -1;
	unlink(PLAYLIST_FILE);
	publish();
	if (s.power_client >= 0) {
		reply(&clients[s.power_client], "OK\n");
		s.power_client = -1;
	}
}

static void timers(void)
{
	if (s.deadline_ms < 0 || now_ms() < s.deadline_ms || s.pid <= 0)
		return;
	if (s.state == STOPPING && !s.term_sent && s.ipc >= 0 && last_position < 0) {
		/* Position never came: ask mpv to quit normally. */
		mpv_send("{\"command\":[\"quit-watch-later\"]}");
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
		s.term_sent = false;
		last_position = 0;
		return;
	}
	if (!s.term_sent) {
		kill(s.pid, SIGTERM);
		s.term_sent = true;
		s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
	} else {
		kill(-s.pid, SIGKILL);
		s.deadline_ms = -1;
	}
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static bool playing(struct client *c)
{
	if (s.state == PLAYING || s.state == PAUSED)
		return true;
	reply(c, "ERR no-session\n");
	return false;
}

static void handle(struct client *c, char *line)
{
	char *arg = strchr(line, '\t'), *arg2 = NULL;
	char status[2 * PATH_MAX + 1024];
	const char *err;

	line[strcspn(line, "\r")] = '\0';
	if (arg) {
		*arg++ = '\0';
		if ((arg2 = strchr(arg, '\t')))
			*arg2++ = '\0';
	}
	if (!strcmp(line, "STATUS") || !strcmp(line, "SUBSCRIBE")) {
		size_t len = build_status(status, sizeof(status));

		c->subscribed = c->subscribed || line[1] == 'U';
		if (send(c->fd, status, len, MSG_NOSIGNAL) < 0)
			close_client(c);
	} else if (!strcmp(line, "PLAY") && arg && arg[0]) {
		bool url = !strncmp(arg, "http://", 7) || !strncmp(arg, "https://", 8);

		if (!url && (arg[0] != '/' || access(arg, R_OK) != 0)) {
			reply(c, "ERR file\n");
			return;
		}
		err = start_mpv(arg, false, 0, arg2 ? atof(arg2) : 0);
		snprintf(status, sizeof(status), err ? "ERR %s\n" : "OK\n", err);
		reply(c, status);
	} else if (!strcmp(line, "PLAYDIR") && arg && arg2) {
		long start;

		if (arg[0] != '/' || strchr(arg2, '/') || !write_playlist(arg, arg2, &start)) {
			reply(c, "ERR file\n");
			return;
		}
		err = start_mpv(PLAYLIST_FILE, true, start, 0);
		snprintf(status, sizeof(status), err ? "ERR %s\n" : "OK\n", err);
		reply(c, status);
	} else if (!strcmp(line, "PAUSE")) {
		if (!playing(c))
			return;
		mpv_send("{\"command\":[\"cycle\",\"pause\"]}");
		reply(c, "OK\n");
	} else if (!strcmp(line, "SEEK") && arg) {
		cJSON *a;

		if (!playing(c))
			return;
		a = cJSON_CreateArray();
		cJSON_AddItemToArray(a, cJSON_CreateString("seek"));
		cJSON_AddItemToArray(a, cJSON_CreateNumber(atof(arg)));
		cJSON_AddItemToArray(a, cJSON_CreateString("relative"));
		mpv_command(a);
		reply(c, "OK\n");
	} else if (!strcmp(line, "NEXT") || !strcmp(line, "PREV")) {
		if (!playing(c))
			return;
		mpv_send(line[0] == 'N' ? "{\"command\":[\"playlist-next\",\"weak\"]}"
					: "{\"command\":[\"playlist-prev\",\"weak\"]}");
		reply(c, "OK\n");
	} else if (!strcmp(line, "SUB") || !strcmp(line, "AUDIO")) {
		if (!playing(c))
			return;
		mpv_send(line[0] == 'S' ? "{\"command\":[\"cycle\",\"sub\"]}"
					: "{\"command\":[\"cycle\",\"audio\"]}");
		reply(c, "OK\n");
	} else if (!strcmp(line, "STOP")) {
		stop_session();
		reply(c, "OK\n");
	} else if (!strcmp(line, "PRE_POWER")) {
		if (s.state == IDLE) {
			reply(c, "OK\n");
			return;
		}
		s.power_client = (int)(c - clients);
		stop_session();
	} else {
		reply(c, "ERR unknown\n");
	}
}

static void process_client(struct client *c)
{
	ssize_t n = recv(c->fd, c->buf + c->used, sizeof(c->buf) - c->used - 1, 0);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		if (s.power_client == (int)(c - clients))
			s.power_client = -1;
		close_client(c);
		return;
	}
	c->used += (size_t)n;
	c->buf[c->used] = '\0';
	while (c->fd >= 0 && (nl = strchr(c->buf, '\n'))) {
		size_t len = (size_t)(nl - c->buf) + 1;

		*nl = '\0';
		handle(c, c->buf);
		if (c->fd < 0)
			return;
		memmove(c->buf, c->buf + len, c->used - len + 1);
		c->used -= len;
	}
	if (c->used >= sizeof(c->buf) - 1)
		close_client(c);
}

int main(void)
{
	struct sigaction sa;
	struct sockaddr_un addr;
	int server;
	FILE *fp;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	if (pipe2(sigchld_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = on_stop;
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(server, 8) != 0) {
		perror("nuubos-mediad");
		return 1;
	}
	chmod(SOCKET_PATH, 0666);
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}

	while (!stop_requested) {
		struct pollfd pfd[3 + MAX_CLIENTS];
		int who[3 + MAX_CLIENTS];
		nfds_t count = 0;
		int timeout = -1;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		if (s.ipc >= 0) {
			pfd[count] = (struct pollfd){ s.ipc, POLLIN, 0 };
			who[count++] = -3;
		}
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0) {
				pfd[count] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
				who[count++] = i;
			}
		if (s.deadline_ms >= 0) {
			long long left = s.deadline_ms - now_ms();

			timeout = left < 0 ? 0 : (int)left;
		}
		if (poll(pfd, count, timeout) < 0 && errno != EINTR)
			break;
		timers();
		for (nfds_t i = 0; i < count; i++) {
			if (!pfd[i].revents)
				continue;
			if (who[i] == -1) {
				int fd;

				while ((fd = accept4(server, NULL, NULL, SOCK_CLOEXEC)) >= 0) {
					int slot = -1;

					for (int k = 0; k < MAX_CLIENTS; k++)
						if (clients[k].fd < 0) {
							slot = k;
							break;
						}
					if (slot < 0) {
						close(fd);
						continue;
					}
					clients[slot].fd = fd;
					clients[slot].subscribed = false;
					clients[slot].used = 0;
				}
			} else if (who[i] == -2) {
				char drain[32];
				int status;
				pid_t pid;

				while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
					;
				while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
					if (pid == s.pid)
						child_exited(status);
			} else if (who[i] == -3) {
				if (s.ipc >= 0)
					process_ipc();
			} else if (clients[who[i]].fd >= 0) {
				process_client(&clients[who[i]]);
			}
		}
	}
	if (s.pid > 0) {
		mpv_send("{\"command\":[\"quit-watch-later\"]}");
		kill(s.pid, SIGTERM);
	}
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
