/* SPDX-License-Identifier: MIT */
/*
 * nuubos-webd — Web Mode service (EPIC-030).
 *
 * Owns the one browser session: Cog (WPE WebKit, Wayland, GLES, WebKit's
 * bubblewrap sandbox for web content and networking) fullscreen, with the
 * active user's profile. For the session's lifetime it holds a
 * "WEB ON" connection to nuubos-inputd, which then turns the controllers
 * into a keyboard and pointer for the browser (and sends the zoom HOTKEYs
 * back here); a crashed service never leaves controllers grabbed. Cog is
 * controlled with cogctl on the system bus (open, previous, next, reload,
 * zoom-*, quit) and reports the page it shows on stdout ("@nuubos uri=",
 * nuubOS patch), which feeds the per-user history.
 *
 * Protocol (/run/nuubos/webd.sock, tab separated):
 *   STATUS | SUBSCRIBE  state=idle|running|stopping uri= title= zoom=
 *                       bookmark=<url>\t<title> ... history=<url>\t<title> ...
 *                       end=1
 *   OPEN <address>      URL, host name or search words; starts the session
 *                       or loads it in the running one
 *   BACK | FORWARD | RELOAD | ZOOM_IN | ZOOM_OUT | ZOOM_RESET | QUIT
 *   BOOKMARK_ADD [<url> [<title>]]   (no url = the page shown)
 *   BOOKMARK_REMOVE <url> | HISTORY_CLEAR
 *   CLEAR_DATA          cookies, site data, cache and history (idle only)
 *   PRE_POWER <action>  sleep keeps the session; restart/poweroff end it
 * Replies OK / ERR <reason>.
 *
 * Per user: /userdata/users/<id>/appdata/web/{bookmarks.tsv,history.tsv,
 * data/} and the cache in /userdata/cache/web/<id>. Idle: poll() without
 * timeout; a timer exists only while a quit is pending.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef COG_BIN
#define COG_BIN "/usr/bin/cog"
#endif
#ifndef COGCTL_BIN
#define COGCTL_BIN "/usr/bin/cogctl"
#endif
#ifndef USERDATA_USERS
#define USERDATA_USERS "/userdata/users"
#endif
#ifndef WEB_CACHE_ROOT
#define WEB_CACHE_ROOT "/userdata/cache/web"
#endif
#ifndef STATE_USERS
#define STATE_USERS "/state/users"
#endif
#define SOCKET_PATH RUN_ROOT "/webd.sock"
#define PIDFILE RUN_ROOT "/webd.pid"
#define INPUT_SOCKET RUN_ROOT "/inputd.sock"
#define USER_DIR RUN_ROOT "/user"
#define ACTIVE_USER_FILE USER_DIR "/active"
#define COG_LOG RUN_ROOT "/cog.log"
#define SEARCH_URL "https://duckduckgo.com/html/?q="

#define MAX_CLIENTS 16
#define MAX_LINE 4096
#define MAX_ENTRIES 64
#define HISTORY_SHOWN 12
#define QUIT_TIMEOUT_MS 3000
#define WEB_MEM_LIMIT_MIB "450"

enum state { IDLE, RUNNING, STOPPING };

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

struct entry {
	char url[1024];
	char title[256];
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];

static struct {
	enum state state;
	pid_t pid;
	int out;        /* Cog stdout: "@nuubos" reports */
	int input;      /* inputd WEB ON connection */
	char in[8192];
	size_t in_used;
	char inbuf[512];
	size_t inbuf_used;
	char user[64];
	char uri[1024];
	char title[256];
	int zoom;
	bool quit_requested;
	bool term_sent;
	long long deadline_ms;
	int power_client;
} s = { .pid = -1, .out = -1, .input = -1, .zoom = 100, .power_client = -1 };

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("webd: ", stderr);
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

/* Values for the line protocol and the TSV files: no tabs or breaks. */
static void flatten(char *p)
{
	for (; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r')
			*p = ' ';
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

static void web_dir(const char *user, char *out, size_t size)
{
	snprintf(out, size, USERDATA_USERS "/%s/appdata/web", user);
}

/* ------------------------------------------------------------------ */
/* Bookmarks and history (per user, TSV url\ttitle, newest first)      */
/* ------------------------------------------------------------------ */

static size_t load_entries(const char *user, const char *name, struct entry *out, size_t max)
{
	char path[PATH_MAX], dir[PATH_MAX], line[1400];
	size_t n = 0;
	FILE *fp;

	if (!user[0])
		return 0;
	web_dir(user, dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (n < max && fgets(line, sizeof(line), fp)) {
		char *tab;

		line[strcspn(line, "\r\n")] = '\0';
		tab = strchr(line, '\t');
		if (tab)
			*tab++ = '\0';
		if (!line[0])
			continue;
		copy_text(out[n].url, sizeof(out[n].url), line);
		copy_text(out[n].title, sizeof(out[n].title), tab);
		n++;
	}
	fclose(fp);
	return n;
}

static bool save_entries(const char *user, const char *name, const struct entry *e, size_t n)
{
	char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
	FILE *fp;

	web_dir(user, dir, sizeof(dir));
	mkdir_p(dir);
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, name);
	fp = fopen(tmp, "w");
	if (!fp)
		return false;
	for (size_t i = 0; i < n; i++)
		fprintf(fp, "%s\t%s\n", e[i].url, e[i].title);
	if (fclose(fp) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

/* Moves/insert url at the top (title kept unless a new one is given). */
static bool entries_put(const char *user, const char *name, const char *url, const char *title, size_t max)
{
	static struct entry e[MAX_ENTRIES + 1];
	size_t n = load_entries(user, name, e + 1, MAX_ENTRIES);
	char keep[256] = "";

	for (size_t i = 1; i <= n; i++)
		if (!strcmp(e[i].url, url)) {
			copy_text(keep, sizeof(keep), e[i].title);
			memmove(&e[i], &e[i + 1], (n - i) * sizeof(e[0]));
			n--;
			break;
		}
	copy_text(e[0].url, sizeof(e[0].url), url);
	copy_text(e[0].title, sizeof(e[0].title), title && title[0] ? title : keep);
	flatten(e[0].url);
	flatten(e[0].title);
	n++;
	if (n > max)
		n = max;
	return save_entries(user, name, e, n);
}

static bool entries_remove(const char *user, const char *name, const char *url)
{
	static struct entry e[MAX_ENTRIES];
	size_t n = load_entries(user, name, e, MAX_ENTRIES);

	for (size_t i = 0; i < n; i++)
		if (!strcmp(e[i].url, url)) {
			memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof(e[0]));
			return save_entries(user, name, e, n - 1);
		}
	return false;
}

/* ------------------------------------------------------------------ */
/* Clients                                                             */
/* ------------------------------------------------------------------ */

static const char *state_name(void)
{
	switch (s.state) {
	case RUNNING: return "running";
	case STOPPING: return "stopping";
	default: return "idle";
	}
}

static char *build_status(size_t *len)
{
	static struct entry e[MAX_ENTRIES];
	static char out[MAX_ENTRIES * 1400 + 4096];
	char user[64];
	size_t n, used;
	int w;

	if (s.state != IDLE)
		copy_text(user, sizeof(user), s.user);
	else if (!read_user(user, sizeof(user)))
		user[0] = '\0';
	w = snprintf(out, sizeof(out), "state=%s\nuri=%s\ntitle=%s\nzoom=%d\n", state_name(),
		     s.state == IDLE ? "" : s.uri, s.state == IDLE ? "" : s.title, s.zoom);
	used = w < 0 ? 0 : (size_t)w;
	n = load_entries(user, "bookmarks.tsv", e, MAX_ENTRIES);
	for (size_t i = 0; i < n && used < sizeof(out) - 1500; i++)
		used += (size_t)snprintf(out + used, sizeof(out) - used, "bookmark=%s\t%s\n", e[i].url, e[i].title);
	n = load_entries(user, "history.tsv", e, HISTORY_SHOWN);
	for (size_t i = 0; i < n && used < sizeof(out) - 1500; i++)
		used += (size_t)snprintf(out + used, sizeof(out) - used, "history=%s\t%s\n", e[i].url, e[i].title);
	used += (size_t)snprintf(out + used, sizeof(out) - used, "end=1\n");
	*len = used;
	return out;
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
	size_t len;
	char *buf = build_status(&len);

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed &&
		    send(clients[i].fd, buf, len, MSG_NOSIGNAL) < 0)
			close_client(&clients[i]);
}

static void notify_failed(const char *reason)
{
	struct nuubos_notify n;

	nuubos_notify_begin(&n, "POST", "web", "web.failed");
	nuubos_notify_str(&n, "reason", reason);
	(void)nuubos_notify_send(&n);
}

/* ------------------------------------------------------------------ */
/* Cog                                                                 */
/* ------------------------------------------------------------------ */

/* One cogctl command on the system bus; waits for it (short). */
static bool cogctl(const char *verb, const char *arg)
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return false;
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
		}
		execl(COGCTL_BIN, "cogctl", "--system", verb, arg, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* inputd: controllers become the browser's keyboard and pointer for the
 * lifetime of this connection. */
static void input_connect(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", INPUT_SOCKET);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    write(fd, "WEB ON\n", 7) != 7) {
		log_msg("inputd unavailable: %s", strerror(errno));
		close(fd);
		return;
	}
	(void)fcntl(fd, F_SETFL, O_NONBLOCK);
	s.input = fd;
	s.inbuf_used = 0;
}

static void input_disconnect(void)
{
	if (s.input >= 0)
		close(s.input);
	s.input = -1;
}

static void user_language(const char *user, char *out, size_t size)
{
	char path[PATH_MAX], line[128];
	FILE *fp;

	copy_text(out, size, "en");
	snprintf(path, sizeof(path), STATE_USERS "/%s/localization.conf", user);
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "LANGUAGE=", 9) && line[9] &&
		    strspn(line + 9, "abcdefghijklmnopqrstuvwxyz_ABCDEFGHIJKLMNOPQRSTUVWXYZ") == strlen(line + 9))
			copy_text(out, size, line + 9);
	}
	fclose(fp);
}

static const char *start_cog(const char *url)
{
	char user[64], dir[PATH_MAX], data[PATH_MAX], cache[PATH_MAX], lang[32];
	int out[2];
	pid_t pid;

	if (!read_user(user, sizeof(user)))
		return "no-user";
	web_dir(user, dir, sizeof(dir));
	snprintf(data, sizeof(data), "%s/data", dir);
	snprintf(cache, sizeof(cache), WEB_CACHE_ROOT "/%s", user);
	mkdir_p(data);
	mkdir_p(cache);
	user_language(user, lang, sizeof(lang));
	if (pipe2(out, O_CLOEXEC) != 0)
		return "spawn";
	pid = fork();
	if (pid < 0) {
		close(out[0]);
		close(out[1]);
		return "spawn";
	}
	if (pid == 0) {
		sigset_t none;
		int log;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		dup2(out[1], STDOUT_FILENO);
		log = open(COG_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (log >= 0)
			dup2(log, STDERR_FILENO);
		clearenv();
		setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
		setenv("HOME", dir, 1);
		setenv("XDG_DATA_HOME", data, 1);
		setenv("XDG_CACHE_HOME", cache, 1);
		setenv("XDG_RUNTIME_DIR", RUN_ROOT "/wayland-runtime", 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		setenv("PIPEWIRE_RUNTIME_DIR", RUN_ROOT "/pipewire", 1);
		setenv("LANGUAGE", lang, 1);
		setenv("COG_PLATFORM_WL_VIEW_FULLSCREEN", "1", 1);
		/* No session bus: GApplication runs non-unique, control is on
		 * the system bus. */
		setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", 1);
		execl(COG_BIN, "cog", "--platform=wl",
		      "--webprocess-failure=error-page",
		      "--cookie-jar=sqlite",
		      "--cookie-store=no-third-party",
		      "--web-mem-limit=" WEB_MEM_LIMIT_MIB,
		      "--enable-spatial-navigation=true",
		      "--enable-developer-extras=false",
		      "--enable-write-console-messages-to-stdout=false",
		      "--", url, (char *)NULL);
		_exit(127);
	}
	close(out[1]);
	s.pid = pid;
	s.out = out[0];
	(void)fcntl(s.out, F_SETFL, O_NONBLOCK);
	s.in_used = 0;
	s.state = RUNNING;
	s.quit_requested = false;
	s.term_sent = false;
	s.deadline_ms = -1;
	s.zoom = 100;
	copy_text(s.user, sizeof(s.user), user);
	copy_text(s.uri, sizeof(s.uri), url);
	s.title[0] = '\0';
	input_connect();
	log_msg("cog started pid=%d", (int)pid);
	return NULL;
}

static void handle_report(char *line)
{
	char *value;

	if (strncmp(line, "@nuubos ", 8) != 0) {
		log_msg("cog: %s", line);
		return;
	}
	line += 8;
	value = strchr(line, '=');
	if (!value)
		return;
	*value++ = '\0';
	flatten(value);
	if (!strcmp(line, "uri")) {
		copy_text(s.uri, sizeof(s.uri), value);
		s.title[0] = '\0';
		/* Real pages only: no about:, data: or error pages. */
		if (!strncmp(value, "http://", 7) || !strncmp(value, "https://", 8))
			(void)entries_put(s.user, "history.tsv", value, NULL, MAX_ENTRIES);
	} else if (!strcmp(line, "title")) {
		copy_text(s.title, sizeof(s.title), value);
		if (value[0] && (!strncmp(s.uri, "http://", 7) || !strncmp(s.uri, "https://", 8)))
			(void)entries_put(s.user, "history.tsv", s.uri, value, MAX_ENTRIES);
	} else if (!strcmp(line, "zoom")) {
		s.zoom = atoi(value);
	} else {
		return;
	}
	publish();
}

static void process_out(void)
{
	ssize_t n = read(s.out, s.in + s.in_used, sizeof(s.in) - s.in_used - 1);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close(s.out);
		s.out = -1;
		return;
	}
	s.in_used += (size_t)n;
	s.in[s.in_used] = '\0';
	while ((nl = strchr(s.in, '\n'))) {
		*nl = '\0';
		handle_report(s.in);
		memmove(s.in, nl + 1, s.in_used - (size_t)(nl + 1 - s.in) + 1);
		s.in_used -= (size_t)(nl + 1 - s.in);
	}
	if (s.in_used >= sizeof(s.in) - 1)
		s.in_used = 0;
}

/* HOTKEY lines from inputd (zoom controls). */
static void process_input(void)
{
	ssize_t n = read(s.input, s.inbuf + s.inbuf_used, sizeof(s.inbuf) - s.inbuf_used - 1);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		log_msg("inputd connection closed");
		input_disconnect();
		return;
	}
	s.inbuf_used += (size_t)n;
	s.inbuf[s.inbuf_used] = '\0';
	while ((nl = strchr(s.inbuf, '\n'))) {
		*nl = '\0';
		if (s.state == RUNNING) {
			if (!strcmp(s.inbuf, "HOTKEY zoom_in"))
				(void)cogctl("zoom-in", NULL);
			else if (!strcmp(s.inbuf, "HOTKEY zoom_out"))
				(void)cogctl("zoom-out", NULL);
			else if (!strcmp(s.inbuf, "HOTKEY zoom_reset"))
				(void)cogctl("zoom-reset", NULL);
		}
		memmove(s.inbuf, nl + 1, s.inbuf_used - (size_t)(nl + 1 - s.inbuf) + 1);
		s.inbuf_used -= (size_t)(nl + 1 - s.inbuf);
	}
	if (s.inbuf_used >= sizeof(s.inbuf) - 1)
		s.inbuf_used = 0;
}

static void quit_session(void)
{
	if (s.state != RUNNING)
		return;
	s.state = STOPPING;
	s.quit_requested = true;
	if (!cogctl("quit", NULL)) {
		kill(s.pid, SIGTERM);
		s.term_sent = true;
	}
	s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
	publish();
}

static void child_exited(int status)
{
	bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	log_msg("cog exited status=%d", status);
	if (!s.quit_requested && !clean)
		notify_failed(WIFEXITED(status) && WEXITSTATUS(status) == 127 ? "start" : "crash");
	if (s.out >= 0) {
		/* Last reports, then the pipe. */
		process_out();
		if (s.out >= 0)
			close(s.out);
	}
	s.out = -1;
	input_disconnect();
	s.pid = -1;
	s.state = IDLE;
	s.deadline_ms = -1;
	s.uri[0] = s.title[0] = '\0';
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
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

static bool has_scheme(const char *a)
{
	return !strncmp(a, "http://", 7) || !strncmp(a, "https://", 8) || !strncmp(a, "about:", 6);
}

/* Host names become https URLs; anything with spaces or without a dot
 * is a search. */
static void normalize_address(const char *in, char *out, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	char a[1024];
	size_t len, o;

	copy_text(a, sizeof(a), in);
	flatten(a);
	while (a[0] == ' ')
		memmove(a, a + 1, strlen(a));
	len = strlen(a);
	while (len && a[len - 1] == ' ')
		a[--len] = '\0';
	if (has_scheme(a)) {
		copy_text(out, size, a);
		return;
	}
	if (!strchr(a, ' ') && (strchr(a, '.') || !strncmp(a, "localhost", 9))) {
		snprintf(out, size, "https://%s", a);
		return;
	}
	o = (size_t)snprintf(out, size, "%s", SEARCH_URL);
	for (const unsigned char *p = (const unsigned char *)a; *p && o + 4 < size; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		    *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			out[o++] = (char)*p;
		} else if (*p == ' ') {
			out[o++] = '+';
		} else {
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 15];
		}
	}
	out[o] = '\0';
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	(void)remove(path);
	return 0;
}

static void remove_tree(const char *path)
{
	(void)nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static bool running(struct client *c)
{
	if (s.state == RUNNING)
		return true;
	reply(c, "ERR no-session\n");
	return false;
}

static void handle(struct client *c, char *line)
{
	char *arg = strchr(line, '\t'), *arg2 = NULL;
	char text[1200];
	char user[64];
	const char *err;

	line[strcspn(line, "\r")] = '\0';
	if (arg) {
		*arg++ = '\0';
		if ((arg2 = strchr(arg, '\t')))
			*arg2++ = '\0';
	}
	if (!strcmp(line, "STATUS") || !strcmp(line, "SUBSCRIBE")) {
		size_t len;
		char *status = build_status(&len);

		c->subscribed = c->subscribed || line[1] == 'U';
		if (send(c->fd, status, len, MSG_NOSIGNAL) < 0)
			close_client(c);
	} else if (!strcmp(line, "OPEN") && arg && arg[0]) {
		char url[1200];

		normalize_address(arg, url, sizeof(url));
		if (s.state == STOPPING) {
			reply(c, "ERR busy\n");
			return;
		}
		if (s.state == RUNNING) {
			reply(c, cogctl("open", url) ? "OK\n" : "ERR control\n");
			return;
		}
		err = start_cog(url);
		snprintf(text, sizeof(text), err ? "ERR %s\n" : "OK\n", err);
		reply(c, text);
		publish();
	} else if (!strcmp(line, "BACK") || !strcmp(line, "FORWARD") || !strcmp(line, "RELOAD") ||
		   !strcmp(line, "ZOOM_IN") || !strcmp(line, "ZOOM_OUT") || !strcmp(line, "ZOOM_RESET")) {
		const char *verb = !strcmp(line, "BACK") ? "previous" : !strcmp(line, "FORWARD") ? "next"
				 : !strcmp(line, "RELOAD") ? "reload" : !strcmp(line, "ZOOM_IN") ? "zoom-in"
				 : !strcmp(line, "ZOOM_OUT") ? "zoom-out" : "zoom-reset";

		if (!running(c))
			return;
		reply(c, cogctl(verb, NULL) ? "OK\n" : "ERR control\n");
	} else if (!strcmp(line, "QUIT")) {
		quit_session();
		reply(c, "OK\n");
	} else if (!strcmp(line, "BOOKMARK_ADD")) {
		const char *url = arg && arg[0] ? arg : s.state == RUNNING ? s.uri : "";
		const char *title = arg && arg[0] ? arg2 : s.title;
		char normalized[1200];

		if (!read_user(user, sizeof(user))) {
			reply(c, "ERR no-user\n");
			return;
		}
		if (!url[0]) {
			reply(c, "ERR address\n");
			return;
		}
		normalize_address(url, normalized, sizeof(normalized));
		reply(c, entries_put(user, "bookmarks.tsv", normalized, title, MAX_ENTRIES) ? "OK\n" : "ERR storage\n");
		publish();
	} else if (!strcmp(line, "BOOKMARK_REMOVE") && arg) {
		if (!read_user(user, sizeof(user))) {
			reply(c, "ERR no-user\n");
			return;
		}
		reply(c, entries_remove(user, "bookmarks.tsv", arg) ? "OK\n" : "ERR missing\n");
		publish();
	} else if (!strcmp(line, "HISTORY_CLEAR") || !strcmp(line, "CLEAR_DATA")) {
		char dir[PATH_MAX], path[PATH_MAX];

		if (!read_user(user, sizeof(user))) {
			reply(c, "ERR no-user\n");
			return;
		}
		if (line[0] == 'C' && s.state != IDLE) {
			reply(c, "ERR busy\n");
			return;
		}
		web_dir(user, dir, sizeof(dir));
		snprintf(path, sizeof(path), "%s/history.tsv", dir);
		unlink(path);
		if (line[0] == 'C') {
			snprintf(path, sizeof(path), "%s/data", dir);
			remove_tree(path);
			snprintf(path, sizeof(path), WEB_CACHE_ROOT "/%s", user);
			remove_tree(path);
		}
		reply(c, "OK\n");
		publish();
	} else if (!strcmp(line, "PRE_POWER")) {
		/* Sleep keeps the page; the network comes back after resume. */
		if (s.state == IDLE || (arg && !strcmp(arg, "sleep"))) {
			reply(c, "OK\n");
			return;
		}
		s.power_client = (int)(c - clients);
		quit_session();
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
	int server, inotify_fd;
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
		perror("nuubos-webd");
		return 1;
	}
	chmod(SOCKET_PATH, 0666);
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}
	/* The active user's bookmarks follow a user switch. */
	inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inotify_fd >= 0)
		(void)inotify_add_watch(inotify_fd, USER_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE);

	while (!stop_requested) {
		struct pollfd pfd[5 + MAX_CLIENTS];
		int who[5 + MAX_CLIENTS];
		nfds_t count = 0;
		int timeout = -1;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		if (s.out >= 0) {
			pfd[count] = (struct pollfd){ s.out, POLLIN, 0 };
			who[count++] = -3;
		}
		if (s.input >= 0) {
			pfd[count] = (struct pollfd){ s.input, POLLIN, 0 };
			who[count++] = -4;
		}
		if (inotify_fd >= 0) {
			pfd[count] = (struct pollfd){ inotify_fd, POLLIN, 0 };
			who[count++] = -5;
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
				if (s.out >= 0)
					process_out();
			} else if (who[i] == -4) {
				if (s.input >= 0)
					process_input();
			} else if (who[i] == -5) {
				char events[4096];

				while (read(inotify_fd, events, sizeof(events)) > 0)
					;
				if (s.state == IDLE)
					publish();
			} else if (clients[who[i]].fd >= 0) {
				process_client(&clients[who[i]]);
			}
		}
	}
	if (s.pid > 0)
		kill(s.pid, SIGTERM);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
