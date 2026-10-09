/* SPDX-License-Identifier: MIT */
/*
 * nuubos-jobd — Background Jobs (EPIC-007).
 *
 * Long-running product work (support bundle, metadata scraping, backups...)
 * runs as a job: a worker process started by this service, never by a UI.
 * Job types are registered by the owning feature in
 * /usr/share/nuubos/jobs/<type>.job; a client can only start a registered
 * type with plain arguments, never an arbitrary command:
 *
 *   COMMAND=/usr/bin/nuubos-diag bundle      program and fixed arguments
 *   SCOPE=device|user                        user: runs for the active user
 *   CANCEL=1                                 the worker may be cancelled
 *   RETRY=1                                  a failed job may be retried
 *   ARGS=0..4                                client arguments accepted
 *   NOTIFY=1                                 Live Notification (default 1)
 *
 * Workers run at low CPU and idle I/O priority with NUUBOS_JOB_ID and, for
 * user jobs, NUUBOS_JOB_USER in the environment. They report on stdout:
 *   @progress <0..100|-1> [step]   determinate / indeterminate progress
 *   @result <text>                 a value for the client (e.g. a path)
 *   @error <reason>                failure reason (raw, the UI localizes)
 * Exit status 0 = succeeded, anything else = failed; SIGTERM = cancel.
 *
 * Every job is one Live Notification (id job.<n>, event "job") updated in
 * place, completed by its final state. At most MAX_RUNNING workers run;
 * the rest wait in submission order. Finished jobs stay listed (bounded)
 * so a client can read the result or ask for a retry; nothing persists
 * across a restart (every current job type is ephemeral or restartable).
 * There is no automatic retry. Idle: poll() without timeout.
 *
 * Protocol (/run/nuubos/jobd.sock, tab separated):
 *   SUBMIT <type> [arg...]   -> OK <id> | ERR unknown|args|busy|no-user
 *   CANCEL <id> | RETRY <id> -> OK | ERR ...
 *   STATUS | SUBSCRIBE       -> job=<id> <type> <state> <progress> <user>
 *                               <step> <result> <reason> ... end=1
 */

#include <ctype.h>
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <nuubos/notify.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef JOBS_DIR
#define JOBS_DIR "/usr/share/nuubos/jobs"
#endif
#define SOCKET_PATH RUN_ROOT "/jobd.sock"
#define PIDFILE RUN_ROOT "/jobd.pid"
#define ACTIVE_USER_FILE RUN_ROOT "/user/active"

#define MAX_CLIENTS 16
#define MAX_JOBS 32
#define MAX_RUNNING 2
#define MAX_ARGS 4
#define MAX_LINE 1024
#define KEEP_FINISHED 16

enum job_state { JOB_QUEUED, JOB_RUNNING, JOB_SUCCEEDED, JOB_FAILED, JOB_CANCELLED };

struct job_type {
	char name[48];
	char command[512];
	bool user_scope;
	bool cancel;
	bool retry;
	bool notify;
	int max_args;
};

struct job {
	bool used;
	int id;
	struct job_type type;
	enum job_state state;
	int progress;
	char step[64];
	char result[256];
	char reason[64];
	char user[64];
	char args[MAX_ARGS][256];
	int nargs;
	pid_t pid;
	int out_fd;
	char out_buf[MAX_LINE];
	size_t out_used;
	bool cancel_requested;
	long long seq;
};

struct client {
	int fd;
	bool subscribed;
	char buf[MAX_LINE];
	size_t used;
};

static volatile sig_atomic_t stop_requested;
static int sigchld_pipe[2] = { -1, -1 };
static struct job jobs[MAX_JOBS];
static struct client clients[MAX_CLIENTS];
static int next_id = 1;
static long long next_seq = 1;

static void log_msg(const char *fmt, ...)
{
	va_list ap;
	time_t now = time(NULL);
	struct tm tm;
	char stamp[32];

	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(stderr, "%s jobd: ", stamp);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void on_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void on_sigchld(int sig)
{
	int saved = errno;
	char byte = 1;

	(void)sig;
	(void)!write(sigchld_pipe[1], &byte, 1);
	errno = saved;
}

static void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

static void trim(char *s)
{
	size_t len = strlen(s);
	size_t start = 0;

	while (len > 0 && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
	while (s[start] && isspace((unsigned char)s[start]))
		start++;
	if (start)
		memmove(s, s + start, len - start + 1);
}

/* Values sent to clients and notifications: no tabs or line breaks. */
static void sanitize(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\r' || *s == '\n')
			*s = ' ';
}

static bool valid_name(const char *s)
{
	if (!*s || strlen(s) >= 48)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
			return false;
	return true;
}

/* Client arguments are data: printable, no leading '-', bounded. */
static bool valid_arg(const char *s)
{
	if (!*s || *s == '-' || strlen(s) >= 256)
		return false;
	for (; *s; s++)
		if ((unsigned char)*s < 0x20)
			return false;
	return true;
}

static bool load_type(const char *name, struct job_type *t)
{
	char path[PATH_MAX];
	char line[640];
	FILE *fp;

	if (!valid_name(name))
		return false;
	memset(t, 0, sizeof(*t));
	copy_text(t->name, sizeof(t->name), name);
	t->notify = true;
	snprintf(path, sizeof(path), JOBS_DIR "/%s.job", name);
	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		char *eq;

		trim(line);
		if (line[0] == '#' || !(eq = strchr(line, '=')))
			continue;
		*eq++ = '\0';
		if (!strcmp(line, "COMMAND"))
			copy_text(t->command, sizeof(t->command), eq);
		else if (!strcmp(line, "SCOPE"))
			t->user_scope = !strcmp(eq, "user");
		else if (!strcmp(line, "CANCEL"))
			t->cancel = !strcmp(eq, "1");
		else if (!strcmp(line, "RETRY"))
			t->retry = !strcmp(eq, "1");
		else if (!strcmp(line, "NOTIFY"))
			t->notify = strcmp(eq, "0") != 0;
		else if (!strcmp(line, "ARGS"))
			t->max_args = atoi(eq);
	}
	fclose(fp);
	if (t->max_args < 0 || t->max_args > MAX_ARGS)
		t->max_args = 0;
	return t->command[0] == '/';
}

static bool read_active_user(char *out, size_t size)
{
	FILE *fp = fopen(ACTIVE_USER_FILE, "r");

	out[0] = '\0';
	if (!fp)
		return false;
	if (fgets(out, (int)size, fp))
		trim(out);
	fclose(fp);
	for (const char *p = out; *p; p++)
		if (!isxdigit((unsigned char)*p) && *p != '-')
			out[0] = '\0';
	return out[0] != '\0';
}

static const char *state_name(enum job_state s)
{
	switch (s) {
	case JOB_QUEUED: return "queued";
	case JOB_RUNNING: return "running";
	case JOB_SUCCEEDED: return "succeeded";
	case JOB_FAILED: return "failed";
	default: return "cancelled";
	}
}

static bool finished(const struct job *j)
{
	return j->state >= JOB_SUCCEEDED;
}

/* ------------------------------------------------------------------ */
/* Notifications and snapshots                                         */
/* ------------------------------------------------------------------ */

static void notify_job(const struct job *j)
{
	struct nuubos_notify n;
	char id[32];

	if (!j->type.notify)
		return;
	snprintf(id, sizeof(id), "job.%d", j->id);
	nuubos_notify_begin(&n, "POST", id, "job");
	nuubos_notify_str(&n, "type", j->type.name);
	nuubos_notify_str(&n, "state", state_name(j->state));
	if (j->state == JOB_RUNNING || j->state == JOB_QUEUED) {
		nuubos_notify_int(&n, "progress", j->state == JOB_QUEUED ? -1 : j->progress);
		/* The Quick Menu offers Stop for it (CANCEL <n>). */
		if (j->type.cancel)
			nuubos_notify_int(&n, "cancel", 1);
	}
	if (j->step[0])
		nuubos_notify_str(&n, "step", j->step);
	if (j->reason[0])
		nuubos_notify_str(&n, "reason", j->reason);
	(void)nuubos_notify_send(&n);
}

static size_t build_status(char *out, size_t size)
{
	size_t len = 0;

	for (int i = 0; i < MAX_JOBS; i++) {
		const struct job *j = &jobs[i];
		int n;

		if (!j->used)
			continue;
		n = snprintf(out + len, size - len, "job=%d\t%s\t%s\t%d\t%s\t%s\t%s\t%s\n",
			     j->id, j->type.name, state_name(j->state), j->progress,
			     j->user, j->step, j->result, j->reason);
		if (n < 0 || (size_t)n >= size - len)
			break;
		len += (size_t)n;
	}
	if (len + 7 < size) {
		memcpy(out + len, "end=1\n", 6);
		len += 6;
	}
	return len;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->used = 0;
}

static void send_text(struct client *c, const char *text, size_t len)
{
	size_t off = 0;

	while (c->fd >= 0 && off < len) {
		ssize_t n = send(c->fd, text + off, len - off, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			close_client(c);
			return;
		}
		off += (size_t)n;
	}
}

static void reply(struct client *c, const char *text)
{
	send_text(c, text, strlen(text));
}

static void publish(void)
{
	static char buf[MAX_JOBS * 768 + 16];
	size_t len = build_status(buf, sizeof(buf));

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed)
			send_text(&clients[i], buf, len);
}

static void changed(struct job *j)
{
	notify_job(j);
	publish();
}

/* ------------------------------------------------------------------ */
/* Workers                                                             */
/* ------------------------------------------------------------------ */

static int running_count(void)
{
	int n = 0;

	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].used && jobs[i].state == JOB_RUNNING)
			n++;
	return n;
}

static bool type_running(const char *type)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].used && jobs[i].state == JOB_RUNNING &&
		    !strcmp(jobs[i].type.name, type))
			return true;
	return false;
}

static void start_worker(struct job *j)
{
	char command[512];
	char *argv[24];
	int argc = 0;
	int out[2];
	char *save = NULL;
	pid_t pid;

	copy_text(command, sizeof(command), j->type.command);
	for (char *tok = strtok_r(command, " ", &save); tok && argc < 16;
	     tok = strtok_r(NULL, " ", &save))
		argv[argc++] = tok;
	for (int i = 0; i < j->nargs; i++)
		argv[argc++] = j->args[i];
	argv[argc] = NULL;

	if (pipe2(out, O_CLOEXEC) != 0) {
		j->state = JOB_FAILED;
		copy_text(j->reason, sizeof(j->reason), "spawn");
		changed(j);
		return;
	}
	pid = fork();
	if (pid < 0) {
		close(out[0]);
		close(out[1]);
		j->state = JOB_FAILED;
		copy_text(j->reason, sizeof(j->reason), "spawn");
		changed(j);
		return;
	}
	if (pid == 0) {
		char id[16];
		sigset_t none;
		int devnull;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		setsid();
		dup2(out[1], STDOUT_FILENO);
		devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0)
			dup2(devnull, STDIN_FILENO);
		/* Background work must not slow the foreground game. */
		(void)setpriority(PRIO_PROCESS, 0, 10);
#ifdef SYS_ioprio_set
		(void)syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0,
			      (3 << 13) /* IOPRIO_CLASS_IDLE */);
#endif
		snprintf(id, sizeof(id), "%d", j->id);
		setenv("NUUBOS_JOB_ID", id, 1);
		if (j->user[0])
			setenv("NUUBOS_JOB_USER", j->user, 1);
		execv(argv[0], argv);
		_exit(127);
	}
	close(out[1]);
	j->pid = pid;
	j->out_fd = out[0];
	(void)fcntl(j->out_fd, F_SETFL, O_NONBLOCK);
	j->state = JOB_RUNNING;
	j->progress = -1;
	log_msg("job %d %s started pid=%d", j->id, j->type.name, (int)pid);
	changed(j);
}

/* Start queued jobs in submission order while capacity remains; one
 * running instance per type. */
static void schedule(void)
{
	for (;;) {
		struct job *best = NULL;

		if (running_count() >= MAX_RUNNING)
			return;
		for (int i = 0; i < MAX_JOBS; i++) {
			struct job *j = &jobs[i];

			if (!j->used || j->state != JOB_QUEUED || type_running(j->type.name))
				continue;
			if (!best || j->seq < best->seq)
				best = j;
		}
		if (!best)
			return;
		start_worker(best);
	}
}

static void handle_worker_line(struct job *j, char *line)
{
	sanitize(line);
	if (!strncmp(line, "@progress ", 10)) {
		char *step = NULL;
		long p = strtol(line + 10, &step, 10);

		j->progress = p < -1 ? -1 : p > 100 ? 100 : (int)p;
		if (step) {
			trim(step);
			if (*step)
				copy_text(j->step, sizeof(j->step), step);
		}
		changed(j);
	} else if (!strncmp(line, "@result ", 8)) {
		copy_text(j->result, sizeof(j->result), line + 8);
	} else if (!strncmp(line, "@error ", 7)) {
		copy_text(j->reason, sizeof(j->reason), line + 7);
	}
}

static void process_worker_output(struct job *j)
{
	char buf[512];
	ssize_t n = read(j->out_fd, buf, sizeof(buf));

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close(j->out_fd);
		j->out_fd = -1;
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			j->out_buf[j->out_used] = '\0';
			handle_worker_line(j, j->out_buf);
			j->out_used = 0;
		} else if (j->out_used + 1 < sizeof(j->out_buf)) {
			j->out_buf[j->out_used++] = buf[i];
		}
	}
}

/* Drop the oldest finished jobs beyond KEEP_FINISHED. */
static void prune(void)
{
	for (;;) {
		struct job *oldest = NULL;
		int count = 0;

		for (int i = 0; i < MAX_JOBS; i++)
			if (jobs[i].used && finished(&jobs[i])) {
				count++;
				if (!oldest || jobs[i].seq < oldest->seq)
					oldest = &jobs[i];
			}
		if (count <= KEEP_FINISHED || !oldest)
			return;
		oldest->used = false;
	}
}

static void reap(void)
{
	char drain[32];
	int status;
	pid_t pid;

	while (read(sigchld_pipe[0], drain, sizeof(drain)) > 0)
		;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (int i = 0; i < MAX_JOBS; i++) {
			struct job *j = &jobs[i];

			if (!j->used || j->pid != pid)
				continue;
			if (j->out_fd >= 0) {
				/* Read what the worker wrote before exiting. */
				while (j->out_fd >= 0) {
					char buf[512];
					ssize_t n = read(j->out_fd, buf, sizeof(buf));

					if (n <= 0)
						break;
					for (ssize_t k = 0; k < n; k++) {
						if (buf[k] == '\n') {
							j->out_buf[j->out_used] = '\0';
							handle_worker_line(j, j->out_buf);
							j->out_used = 0;
						} else if (j->out_used + 1 < sizeof(j->out_buf)) {
							j->out_buf[j->out_used++] = buf[k];
						}
					}
				}
				close(j->out_fd);
				j->out_fd = -1;
			}
			j->pid = -1;
			if (j->cancel_requested)
				j->state = JOB_CANCELLED;
			else if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
				j->state = JOB_SUCCEEDED;
			else {
				j->state = JOB_FAILED;
				if (!j->reason[0])
					copy_text(j->reason, sizeof(j->reason),
						  WIFEXITED(status) && WEXITSTATUS(status) == 127
							  ? "spawn" : "error");
			}
			if (j->state == JOB_SUCCEEDED)
				j->progress = 100;
			log_msg("job %d %s %s", j->id, j->type.name, state_name(j->state));
			changed(j);
		}
	}
	prune();
	schedule();
}

static struct job *find_job(const char *text)
{
	int id = atoi(text);

	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].used && jobs[i].id == id)
			return &jobs[i];
	return NULL;
}

static struct job *free_slot(void)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (!jobs[i].used)
			return &jobs[i];
	prune();
	for (int i = 0; i < MAX_JOBS; i++)
		if (!jobs[i].used)
			return &jobs[i];
	return NULL;
}

static void submit(struct client *c, char *args)
{
	char *field[1 + MAX_ARGS + 1];
	int n = 0;
	struct job_type type;
	struct job *j;
	char text[32];
	char *save = NULL;

	for (char *tok = strtok_r(args, "\t", &save); tok && n < 1 + MAX_ARGS + 1;
	     tok = strtok_r(NULL, "\t", &save))
		field[n++] = tok;
	if (n < 1 || !load_type(field[0], &type)) {
		reply(c, "ERR unknown\n");
		return;
	}
	if (n - 1 > type.max_args) {
		reply(c, "ERR args\n");
		return;
	}
	for (int i = 1; i < n; i++)
		if (!valid_arg(field[i])) {
			reply(c, "ERR args\n");
			return;
		}
	/* The same job with the same arguments already pending: one job,
	 * one notification. */
	for (int i = 0; i < MAX_JOBS; i++) {
		struct job *o = &jobs[i];
		bool same = o->used && !finished(o) && !strcmp(o->type.name, type.name) &&
			    o->nargs == n - 1;

		for (int k = 0; same && k < n - 1; k++)
			same = !strcmp(o->args[k], field[k + 1]);
		if (same) {
			snprintf(text, sizeof(text), "OK %d\n", o->id);
			reply(c, text);
			return;
		}
	}
	j = free_slot();
	if (!j) {
		reply(c, "ERR busy\n");
		return;
	}
	memset(j, 0, sizeof(*j));
	j->used = true;
	j->id = next_id++;
	j->seq = next_seq++;
	j->type = type;
	j->pid = -1;
	j->out_fd = -1;
	j->progress = -1;
	j->state = JOB_QUEUED;
	if (type.user_scope && !read_active_user(j->user, sizeof(j->user))) {
		j->used = false;
		reply(c, "ERR no-user\n");
		return;
	}
	j->nargs = n - 1;
	for (int i = 1; i < n; i++)
		copy_text(j->args[i - 1], sizeof(j->args[i - 1]), field[i]);
	snprintf(text, sizeof(text), "OK %d\n", j->id);
	reply(c, text);
	log_msg("job %d %s queued", j->id, type.name);
	changed(j);
	schedule();
}

static void cancel(struct client *c, const char *arg)
{
	struct job *j = find_job(arg);

	if (!j || finished(j)) {
		reply(c, "ERR job\n");
		return;
	}
	if (!j->type.cancel) {
		reply(c, "ERR not-cancellable\n");
		return;
	}
	j->cancel_requested = true;
	if (j->state == JOB_QUEUED) {
		j->state = JOB_CANCELLED;
		changed(j);
	} else if (j->pid > 0) {
		/* The worker stops at its next safe point and removes partial
		 * output; its exit completes the job. */
		kill(-j->pid, SIGTERM);
	}
	reply(c, "OK\n");
}

static void retry(struct client *c, const char *arg)
{
	struct job *j = find_job(arg);

	if (!j || (j->state != JOB_FAILED && j->state != JOB_CANCELLED)) {
		reply(c, "ERR job\n");
		return;
	}
	if (!j->type.retry) {
		reply(c, "ERR not-retryable\n");
		return;
	}
	/* The same job id and notification, re-queued explicitly. */
	j->state = JOB_QUEUED;
	j->seq = next_seq++;
	j->progress = -1;
	j->cancel_requested = false;
	j->step[0] = j->result[0] = j->reason[0] = '\0';
	reply(c, "OK\n");
	changed(j);
	schedule();
}

static void handle_command(struct client *c, char *line)
{
	char *arg = strchr(line, '\t');
	static char buf[MAX_JOBS * 768 + 16];

	if (arg)
		*arg++ = '\0';
	trim(line);
	if (!strcmp(line, "STATUS") || !strcmp(line, "SUBSCRIBE")) {
		c->subscribed = c->subscribed || !strcmp(line, "SUBSCRIBE");
		send_text(c, buf, build_status(buf, sizeof(buf)));
	} else if (!strcmp(line, "SUBMIT") && arg) {
		submit(c, arg);
	} else if (!strcmp(line, "CANCEL") && arg) {
		cancel(c, arg);
	} else if (!strcmp(line, "RETRY") && arg) {
		retry(c, arg);
	} else {
		reply(c, "ERR unknown command\n");
	}
}

static void process_client(struct client *c)
{
	ssize_t n = recv(c->fd, c->buf + c->used, sizeof(c->buf) - c->used - 1, 0);
	char *nl;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		close_client(c);
		return;
	}
	c->used += (size_t)n;
	c->buf[c->used] = '\0';
	while (c->fd >= 0 && (nl = strchr(c->buf, '\n'))) {
		size_t len = (size_t)(nl - c->buf) + 1;

		*nl = '\0';
		handle_command(c, c->buf);
		if (c->fd < 0)
			return;
		memmove(c->buf, c->buf + len, c->used - len + 1);
		c->used -= len;
	}
	if (c->used >= sizeof(c->buf) - 1)
		close_client(c);
}

static int make_server(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 16) != 0) {
		close(fd);
		return -1;
	}
	chmod(SOCKET_PATH, 0600);
	return fd;
}

int main(void)
{
	struct sigaction sa;
	int server;
	FILE *fp;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	mkdir(RUN_ROOT, 0755);
	if (pipe2(sigchld_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = on_stop;
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	server = make_server();
	if (server < 0) {
		perror("nuubos-jobd: socket");
		return 1;
	}
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}
	log_msg("started");

	while (!stop_requested) {
		struct pollfd pfd[2 + MAX_JOBS + MAX_CLIENTS];
		int who[2 + MAX_JOBS + MAX_CLIENTS];
		nfds_t count = 0;

		pfd[count] = (struct pollfd){ server, POLLIN, 0 };
		who[count++] = -1;
		pfd[count] = (struct pollfd){ sigchld_pipe[0], POLLIN, 0 };
		who[count++] = -2;
		for (int i = 0; i < MAX_JOBS; i++)
			if (jobs[i].used && jobs[i].out_fd >= 0) {
				pfd[count] = (struct pollfd){ jobs[i].out_fd, POLLIN, 0 };
				who[count++] = 1000 + i;
			}
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0) {
				pfd[count] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
				who[count++] = i;
			}
		if (poll(pfd, count, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (nfds_t i = 0; i < count; i++) {
			if (!pfd[i].revents)
				continue;
			if (who[i] == -1) {
				for (;;) {
					int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC);
					int slot = -1;

					if (fd < 0)
						break;
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
				reap();
			} else if (who[i] >= 1000) {
				struct job *j = &jobs[who[i] - 1000];

				if (j->used && j->out_fd >= 0)
					process_worker_output(j);
			} else if (clients[who[i]].fd >= 0) {
				process_client(&clients[who[i]]);
			}
		}
	}

	/* Service stop: cancel running workers (each removes partial output). */
	for (int i = 0; i < MAX_JOBS; i++)
		if (jobs[i].used && jobs[i].pid > 0)
			kill(-jobs[i].pid, SIGTERM);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
