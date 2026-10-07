/* SPDX-License-Identifier: MIT */
/*
 * nuubos-syncd — Syncthing session for the active user (EPIC-034).
 *
 * Syncthing runs only for the active user and only when that user enabled
 * it (/userdata/users/<id>/appdata/syncthing/nuubos.conf, ENABLED=1). Each
 * user has an own Syncthing home (identity, devices, folders, database), so
 * switching users never exposes another user's configuration. Management
 * is only through a root-owned unix socket (/run/nuubos/syncthing.sock,
 * REST with a per-user API key); there is no network GUI.
 *
 * Events: inotify on /run/nuubos/user (user switch), SIGHUP (enable or
 * disable from nuubos-syncctl), SIGCHLD. A Syncthing process that dies on
 * its own is not restarted (no restart storm): the state shows "failed"
 * until the next user switch or enable. Idle: poll() without timeout.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef USERDATA_USERS
#define USERDATA_USERS "/userdata/users"
#endif
#ifndef SYNCTHING_BIN
#define SYNCTHING_BIN "/usr/bin/syncthing"
#endif
#define PIDFILE RUN_ROOT "/syncd.pid"
#define STATE_FILE RUN_ROOT "/syncd.state"
#define GUI_SOCKET RUN_ROOT "/syncthing.sock"
#define USER_DIR RUN_ROOT "/user"
#define ACTIVE_FILE USER_DIR "/active"

static volatile sig_atomic_t stop_requested, reload_requested;
static int wake_pipe[2] = { -1, -1 };
static pid_t child = -1;
static char child_user[64];
static bool failed;

static void wake(void)
{
	int saved = errno;
	char b = 1;

	(void)!write(wake_pipe[1], &b, 1);
	errno = saved;
}

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		reload_requested = 1;
	else if (sig != SIGCHLD)
		stop_requested = 1;
	wake();
}

static bool read_line(const char *path, char *out, size_t size)
{
	FILE *fp = fopen(path, "r");

	out[0] = '\0';
	if (!fp)
		return false;
	if (fgets(out, (int)size, fp))
		out[strcspn(out, "\r\n")] = '\0';
	fclose(fp);
	return out[0] != '\0';
}

static bool valid_user(const char *u)
{
	if (strlen(u) != 36)
		return false;
	for (; *u; u++)
		if (!((*u >= '0' && *u <= '9') || (*u >= 'a' && *u <= 'f') || *u == '-'))
			return false;
	return true;
}

/* key=value from the user's nuubos.conf. */
static bool user_value(const char *user, const char *key, char *out, size_t size)
{
	char path[PATH_MAX], line[256];
	size_t klen = strlen(key);
	FILE *fp;
	bool found = false;

	out[0] = '\0';
	snprintf(path, sizeof(path), USERDATA_USERS "/%s/appdata/syncthing/nuubos.conf", user);
	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, key, klen) && line[klen] == '=') {
			snprintf(out, size, "%s", line + klen + 1);
			found = true;
		}
	}
	fclose(fp);
	return found;
}

static void write_state(void)
{
	FILE *fp = fopen(STATE_FILE ".tmp", "w");

	if (!fp)
		return;
	fprintf(fp, "user=%s\nrunning=%d\nfailed=%d\n", child > 0 ? child_user : "", child > 0, failed);
	fclose(fp);
	rename(STATE_FILE ".tmp", STATE_FILE);
}

static void on_alarm(int sig)
{
	(void)sig;
}

/* Syncthing closes its database on SIGTERM: wait for it, at most 10 s. */
static void stop_child(void)
{
	struct sigaction sa, old;

	if (child <= 0)
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm; /* no SA_RESTART: interrupts waitpid */
	sigaction(SIGALRM, &sa, &old);
	kill(child, SIGTERM);
	alarm(10);
	if (waitpid(child, NULL, 0) != child) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
	alarm(0);
	sigaction(SIGALRM, &old, NULL);
	child = -1;
	unlink(GUI_SOCKET);
	child_user[0] = '\0';
}

static void start_child(const char *user)
{
	char home[PATH_MAX], key[128], arg_home[PATH_MAX + 16], arg_key[160];
	pid_t pid;

	if (!user_value(user, "APIKEY", key, sizeof(key)) || strpbrk(key, " \t"))
		return;
	snprintf(home, sizeof(home), USERDATA_USERS "/%s/appdata/syncthing", user);
	snprintf(arg_home, sizeof(arg_home), "--home=%s", home);
	snprintf(arg_key, sizeof(arg_key), "--gui-apikey=%s", key);
	unlink(GUI_SOCKET);
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		int log = open(RUN_ROOT "/syncthing.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		char *argv[] = {
			(char *)SYNCTHING_BIN, (char *)"serve", arg_home, arg_key,
			(char *)"--no-browser", (char *)"--no-restart", (char *)"--no-upgrade",
			(char *)"--gui-address=unix://" GUI_SOCKET, NULL,
		};
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		clearenv();
		setenv("PATH", "/usr/bin:/bin", 1);
		setenv("HOME", home, 1);
		/* No ~/Sync default folder: nuubOS shares the user's folders. */
		setenv("STNODEFAULTFOLDER", "1", 1);
		setenv("STNOUPGRADE", "1", 1);
		/* Background work must not slow a game down. */
		if (nice(10) < 0)
			perror("nice"); /* keeps the default priority */
		execv(SYNCTHING_BIN, argv);
		_exit(127);
	}
	child = pid;
	snprintf(child_user, sizeof(child_user), "%s", user);
	failed = false;
	fprintf(stderr, "syncd: syncthing started for %s (pid %d)\n", user, (int)pid);
}

static void reconcile(void)
{
	static char last_user[64];
	char user[64], enabled[8];
	bool want;

	read_line(ACTIVE_FILE, user, sizeof(user));
	if (strcmp(user, last_user)) {
		/* Another user: a failure of the previous session does not carry over. */
		failed = false;
		snprintf(last_user, sizeof(last_user), "%s", user);
	}
	want = valid_user(user) && user_value(user, "ENABLED", enabled, sizeof(enabled)) && !strcmp(enabled, "1");
	if (child > 0 && (!want || strcmp(child_user, user))) {
		stop_child();
		failed = false;
	}
	if (want && child <= 0 && !failed)
		start_child(user);
	write_state();
}

int main(void)
{
	struct sigaction sa;
	int in;
	FILE *fp;

	if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);
	mkdir(USER_DIR, 0755);
	in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (in < 0 || inotify_add_watch(in, USER_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE) < 0)
		fprintf(stderr, "syncd: inotify unavailable\n");
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}
	reconcile();
	while (!stop_requested) {
		struct pollfd pfd[2] = { { wake_pipe[0], POLLIN, 0 }, { in, POLLIN, 0 } };
		char buf[4096];
		int status;
		pid_t pid;

		if (poll(pfd, in >= 0 ? 2 : 1, -1) < 0 && errno != EINTR)
			break;
		while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
			;
		while (in >= 0 && read(in, buf, sizeof(buf)) > 0)
			;
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
			if (pid == child) {
				fprintf(stderr, "syncd: syncthing exited status=%d\n", status);
				child = -1;
				child_user[0] = '\0';
				failed = true;
			}
		if (reload_requested) {
			reload_requested = 0;
			failed = false;
		}
		reconcile();
	}
	stop_child();
	unlink(PIDFILE);
	unlink(STATE_FILE);
	return 0;
}
