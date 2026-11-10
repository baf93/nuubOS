/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-termd — Terminal application service (RC4, user decision
 * 2026-10-27; EPIC-001 US-UI-001: never needed for ordinary flows).
 *
 * Owns the one terminal session: foot (Wayland, fullscreen under labwc)
 * running root's login shell, the same environment as an SSH login
 * (/bin/sh -l, HOME=/root, /etc/profile). For the session's lifetime it
 * holds a "TERMINAL ON" connection to nuubos-inputd, which then turns the
 * controllers into terminal keys (arrows, Enter, Esc, Tab...) and sends the
 * keyboard HOTKEY back here; a crashed service never leaves controllers
 * grabbed. The Quick Menu shows its on-screen keyboard over the terminal
 * while keyboard=1 and types through the Wayland input method. With a
 * physical keyboard attached (Bluetooth or USB) the on-screen keyboard
 * starts hidden; plugging one in hides it, removing the last one shows it.
 *
 * Protocol (/run/nuubos/termd.sock, tab separated):
 *   STATUS | SUBSCRIBE  state=idle|running|stopping keyboard=0|1
 *                       physical_keyboard=0|1 end=1
 *   OPEN                starts the session (OK when it already runs)
 *   KEYBOARD 0|1|toggle on-screen keyboard shown or hidden
 *   QUIT
 *   PRE_POWER <action>  sleep keeps the session; restart/poweroff end it
 * Replies OK / ERR <reason>.
 *
 * Idle: poll() without timeout; /dev/input is watched only while a session
 * runs, and a timer exists only while a quit is pending.
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
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef FOOT_BIN
#define FOOT_BIN "/usr/bin/foot"
#endif
#ifndef FOOT_CONFIG
#define FOOT_CONFIG "/etc/nuubos/terminal/foot.ini"
#endif
#ifndef SYS_INPUT
#define SYS_INPUT "/sys/class/input"
#endif
#ifndef DEV_INPUT
#define DEV_INPUT "/dev/input"
#endif
#define SOCKET_PATH RUN_ROOT "/termd.sock"
#define PIDFILE RUN_ROOT "/termd.pid"
#define INPUT_SOCKET RUN_ROOT "/inputd.sock"
#define FOOT_LOG RUN_ROOT "/foot.log"

#define MAX_CLIENTS 16
#define MAX_LINE 1024
#define QUIT_TIMEOUT_MS 3000

enum state { IDLE, RUNNING, STOPPING };

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];

static struct {
	enum state state;
	pid_t pid;
	int input;      /* inputd TERMINAL ON connection */
	char inbuf[512];
	size_t inbuf_used;
	int watch;      /* inotify on /dev/input while running */
	bool keyboard;  /* on-screen keyboard shown */
	bool physical;  /* a real keyboard is attached */
	bool term_sent;
	long long deadline_ms;
	int power_client;
} s = { .pid = -1, .input = -1, .watch = -1, .deadline_ms = -1, .power_client = -1 };

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("termd: ", stderr);
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

/* ------------------------------------------------------------------ */
/* Physical keyboards                                                  */
/* ------------------------------------------------------------------ */

/* Bit n of a sysfs capability bitmap ("hex words, most significant
 * first", each a long). */
static bool cap_bit(const char *bitmap, int n)
{
	unsigned long words[16];
	int count = 0;
	const char *p = bitmap;
	int bits = (int)(sizeof(unsigned long) * 8);

	while (*p && count < 16) {
		char *end;
		unsigned long w = strtoul(p, &end, 16);

		if (end == p)
			break;
		words[count++] = w;
		p = end;
	}
	if (n / bits >= count)
		return false;
	return (words[count - 1 - n / bits] >> (n % bits)) & 1UL;
}

/* A real keyboard: letters and the space bar, not one of nuubOS's own
 * uinput devices (their sysfs node is under /sys/devices/virtual). */
static bool is_keyboard(const char *event)
{
	char path[PATH_MAX], real[PATH_MAX], caps[512] = "", name[128] = "";
	FILE *fp;

	snprintf(path, sizeof(path), SYS_INPUT "/%s/device", event);
	if (!realpath(path, real) || strstr(real, "/devices/virtual/"))
		return false;
	snprintf(path, sizeof(path), SYS_INPUT "/%s/device/name", event);
	if ((fp = fopen(path, "r"))) {
		if (!fgets(name, sizeof(name), fp))
			name[0] = '\0';
		fclose(fp);
	}
	if (!strncmp(name, "nuubOS ", 7))
		return false;
	snprintf(path, sizeof(path), SYS_INPUT "/%s/device/capabilities/key", event);
	if (!(fp = fopen(path, "r")))
		return false;
	if (!fgets(caps, sizeof(caps), fp))
		caps[0] = '\0';
	fclose(fp);
	/* KEY_Q 16, KEY_A 30, KEY_Z 44, KEY_SPACE 57 */
	return cap_bit(caps, 16) && cap_bit(caps, 30) && cap_bit(caps, 44) && cap_bit(caps, 57);
}

static bool keyboard_attached(void)
{
	DIR *dir = opendir(SYS_INPUT);
	struct dirent *e;
	bool found = false;

	if (!dir)
		return false;
	while (!found && (e = readdir(dir)))
		if (!strncmp(e->d_name, "event", 5))
			found = is_keyboard(e->d_name);
	closedir(dir);
	return found;
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

static int build_status(char *out, size_t size)
{
	return snprintf(out, size, "state=%s\nkeyboard=%d\nphysical_keyboard=%d\nend=1\n",
			state_name(), s.state == RUNNING && s.keyboard, s.state != IDLE && s.physical);
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
	char buf[256];
	int len = build_status(buf, sizeof(buf));

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed &&
		    send(clients[i].fd, buf, (size_t)len, MSG_NOSIGNAL) < 0)
			close_client(&clients[i]);
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

/* inputd: controllers become terminal keys for the lifetime of this
 * connection. */
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
	    write(fd, "TERMINAL ON\n", 12) != 12) {
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

static void watch_start(void)
{
	s.watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (s.watch >= 0 && inotify_add_watch(s.watch, DEV_INPUT, IN_CREATE | IN_DELETE | IN_ATTRIB) < 0) {
		close(s.watch);
		s.watch = -1;
	}
}

static void watch_stop(void)
{
	if (s.watch >= 0)
		close(s.watch);
	s.watch = -1;
}

/* A keyboard appeared: hide the on-screen one; the last one went away:
 * show it. */
static void keyboards_changed(void)
{
	bool physical = keyboard_attached();

	if (physical == s.physical)
		return;
	s.physical = physical;
	s.keyboard = !physical;
	log_msg("physical keyboard %s", physical ? "attached" : "removed");
	publish();
}

static const char *start_foot(void)
{
	pid_t pid = fork();

	if (pid < 0)
		return "spawn";
	if (pid == 0) {
		sigset_t none;
		int log, null;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		null = open("/dev/null", O_RDWR);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDOUT_FILENO);
		}
		log = open(FOOT_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (log >= 0)
			dup2(log, STDERR_FILENO);
		if (chdir("/root") != 0)
			(void)!chdir("/");
		/* What dropbear gives an SSH login; the login shell reads
		 * /etc/profile for the rest. */
		clearenv();
		setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
		setenv("HOME", "/root", 1);
		setenv("USER", "root", 1);
		setenv("LOGNAME", "root", 1);
		setenv("SHELL", "/bin/sh", 1);
		setenv("LANG", "C.UTF-8", 1);
		setenv("XDG_RUNTIME_DIR", RUN_ROOT "/wayland-runtime", 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		execl(FOOT_BIN, "foot", "--config=" FOOT_CONFIG, "--login-shell", (char *)NULL);
		_exit(127);
	}
	s.pid = pid;
	s.state = RUNNING;
	s.term_sent = false;
	s.deadline_ms = -1;
	s.physical = keyboard_attached();
	s.keyboard = !s.physical;
	watch_start();
	input_connect();
	log_msg("foot started pid=%d physical_keyboard=%d", (int)pid, s.physical);
	return NULL;
}

static void quit_session(void);

/* HOTKEY lines from inputd (the keyboard button; quit = H + Start). */
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
		if (s.state == RUNNING && !strcmp(s.inbuf, "HOTKEY keyboard")) {
			s.keyboard = !s.keyboard;
			publish();
		} else if (!strcmp(s.inbuf, "HOTKEY quit")) {
			quit_session();
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
	/* foot hangs up the shell and its children. */
	kill(s.pid, SIGTERM);
	s.term_sent = true;
	s.deadline_ms = now_ms() + QUIT_TIMEOUT_MS;
	publish();
}

static void child_exited(int status)
{
	log_msg("foot exited status=%d", status);
	/* Processes the shell left behind in the session go too. */
	kill(-s.pid, SIGHUP);
	input_disconnect();
	watch_stop();
	s.pid = -1;
	s.state = IDLE;
	s.deadline_ms = -1;
	s.keyboard = false;
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
	kill(-s.pid, SIGKILL);
	s.deadline_ms = -1;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void handle(struct client *c, char *line)
{
	char *arg = strchr(line, '\t');
	char text[64];
	const char *err;

	line[strcspn(line, "\r")] = '\0';
	if (arg)
		*arg++ = '\0';
	if (!strcmp(line, "STATUS") || !strcmp(line, "SUBSCRIBE")) {
		char status[256];
		int len = build_status(status, sizeof(status));

		c->subscribed = c->subscribed || line[1] == 'U';
		if (send(c->fd, status, (size_t)len, MSG_NOSIGNAL) < 0)
			close_client(c);
	} else if (!strcmp(line, "OPEN")) {
		if (s.state == STOPPING) {
			reply(c, "ERR busy\n");
			return;
		}
		if (s.state == RUNNING) {
			reply(c, "OK\n");
			return;
		}
		err = start_foot();
		snprintf(text, sizeof(text), err ? "ERR %s\n" : "OK\n", err);
		reply(c, text);
		publish();
	} else if (!strcmp(line, "KEYBOARD") && arg) {
		if (s.state != RUNNING) {
			reply(c, "ERR no-session\n");
			return;
		}
		if (!strcmp(arg, "toggle"))
			s.keyboard = !s.keyboard;
		else if (!strcmp(arg, "0") || !strcmp(arg, "1"))
			s.keyboard = arg[0] == '1';
		else {
			reply(c, "ERR value\n");
			return;
		}
		reply(c, "OK\n");
		publish();
	} else if (!strcmp(line, "QUIT")) {
		quit_session();
		reply(c, "OK\n");
	} else if (!strcmp(line, "PRE_POWER")) {
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
		perror("nuubos-termd");
		return 1;
	}
	chmod(SOCKET_PATH, 0666);
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}

	while (!stop_requested) {
		struct pollfd pfd[4 + MAX_CLIENTS];
		int who[4 + MAX_CLIENTS];
		nfds_t count = 0;
		int timeout = -1;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		if (s.input >= 0) {
			pfd[count] = (struct pollfd){ s.input, POLLIN, 0 };
			who[count++] = -3;
		}
		if (s.watch >= 0) {
			pfd[count] = (struct pollfd){ s.watch, POLLIN, 0 };
			who[count++] = -4;
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
				if (s.input >= 0)
					process_input();
			} else if (who[i] == -4) {
				char events[4096];

				while (s.watch >= 0 && read(s.watch, events, sizeof(events)) > 0)
					;
				/* udev sets the node's permissions after it appears:
				 * IN_ATTRIB comes when sysfs is complete too. */
				if (s.state == RUNNING)
					keyboards_changed();
			} else if (clients[who[i]].fd >= 0) {
				process_client(&clients[who[i]]);
			}
		}
	}
	if (s.pid > 0)
		kill(-s.pid, SIGTERM);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
