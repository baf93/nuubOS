/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-tsd — the Tailscale capability of the console.
 *
 * Owner of Tailscale (user request 2026-10-21, Reddit: "a Tailscale client
 * option would be good"): it downloads Tailscale's daemon on request
 * (nuubos-tailscale-get, never part of the image), runs it only while the
 * user keeps Tailscale on, applies the nuubOS policy through its local
 * API, follows its event bus and publishes one state snapshot to its
 * clients. Device-wide, like the other remote services: the console joins
 * one tailnet, whoever is playing.
 *
 *   /userdata/tailscale/app/tailscaled + VERSION   the downloaded build
 *   /state/tailscale/                              tailscaled's state (node key)
 *   /state/config/tailscale.conf                   ENABLED=0|1
 *
 * tailscaled is reached only through its root-only unix socket
 * (/run/nuubos/tailscale/tailscaled.sock); it runs with --no-logs-no-support
 * (no log upload to Tailscale), nice 10. Policy at every start: the host
 * name "nuubOS <model>", no MagicDNS rewrite of /etc/resolv.conf (CorpDNS
 * off), no netfilter rules (the console has no iptables), no subnet routes
 * accepted, WantRunning on. Once signed in, the console's remote services
 * (SSH, file sharing, Web administration, when on) answer on its tailnet
 * address too, and its games can reach PCs (Moonlight) through the tailnet.
 *
 * Socket /run/nuubos/tsd.sock (0660), one command per line:
 *   STATUS | SUBSCRIBE    snapshot (below); SUBSCRIBE then gets a new one
 *                         after every change
 *   INSTALL               download (or update) and turn Tailscale on
 *   CHECK                 look for a newer build (latest=)
 *   CANCEL                stop a download
 *   REMOVE                sign out, delete the build and the node state
 *   ENABLE | DISABLE      run tailscaled or not
 *   LOGIN                 start an interactive sign-in (auth_url= follows)
 *   LOGOUT                sign out of the tailnet
 * Replies: OK | ERR <reason> (busy, not-installed, not-running, tailscale,
 * storage, unknown).
 *
 * Snapshot (key=value, ends with end=1):
 *   installed= version= latest= job=install|check|remove| progress=0..100|-1
 *   error=network|space|archive|checksum|write|
 *   enabled= running= ready= failed=
 *   state=NoState|NeedsLogin|NeedsMachineAuth|Stopped|Starting|Running
 *   auth_url= name= ip= ip6= tailnet=
 *
 * Events: inotify on the tailscaled socket directory (ready), SIGCHLD, the
 * download helper's output, client sockets, and a thread that keeps the
 * local API's event bus (watch-ipn-bus) open while tailscaled runs: any
 * notification rebuilds the snapshot from /localapi/v0/status. A tailscaled
 * that dies on its own is not restarted (failed=1 until ENABLE). Idle:
 * poll() without timeout.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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
#include <unistd.h>

#include <cjson/cJSON.h>
#include <curl/curl.h>

#ifndef RUN_ROOT
#define RUN_ROOT "/run/nuubos"
#endif
#ifndef APP_ROOT
#define APP_ROOT "/userdata/tailscale"
#endif
#ifndef STATE_DIR
#define STATE_DIR "/state/tailscale"
#endif
#ifndef CONF_PATH
#define CONF_PATH "/state/config/tailscale.conf"
#endif
#ifndef GET_BIN
#define GET_BIN "/usr/lib/nuubos/nuubos-tailscale-get"
#endif
#ifndef MODEL_FILE
#define MODEL_FILE "/proc/device-tree/model"
#endif
#define PIDFILE RUN_ROOT "/tsd.pid"
#define SOCKET_PATH RUN_ROOT "/tsd.sock"
#define TS_DIR RUN_ROOT "/tailscale"
#define TS_NAME "tailscaled.sock"
#define TS_SOCKET TS_DIR "/" TS_NAME
#define TS_LOG RUN_ROOT "/tailscaled.log"
#define APP_BIN APP_ROOT "/app/tailscaled"
#define APP_VERSION APP_ROOT "/app/VERSION"

#define MAX_CLIENTS 12

struct client {
	int fd;
	bool sub;
	size_t used;
	char buf[256];
};

static volatile sig_atomic_t stop_requested;
static int wake_pipe[2] = { -1, -1 };
static struct client clients[MAX_CLIENTS];

/* Main thread only. */
static pid_t child = -1;
static bool failed, ready, enabled;
static char *snapshot;
/* The download helper: its pid, stdout, what it does, its result. */
static pid_t job = -1;
static int job_fd = -1;
static char job_kind[16], job_error[16], latest[32], job_line[256];
static size_t job_used;
static int progress = -1;
/* INSTALL turns Tailscale on once the build is in place. */
static bool enable_after_install;

/* Shared with the event thread. */
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;
static unsigned ev_gen;
static bool ev_active;

static void wake(void)
{
	int saved = errno;
	char b = 1;

	(void)!write(wake_pipe[1], &b, 1);
	errno = saved;
}

static void on_signal(int sig)
{
	if (sig != SIGCHLD)
		stop_requested = 1;
	wake();
}

/* ---------------------------------------------------------------- */
/* Files                                                            */
/* ---------------------------------------------------------------- */

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

static void mkdir_parents(const char *dir, mode_t mode)
{
	char p[PATH_MAX];

	snprintf(p, sizeof(p), "%s", dir);
	for (char *q = p + 1; *q; q++)
		if (*q == '/') {
			*q = '\0';
			mkdir(p, 0755);
			*q = '/';
		}
	mkdir(p, mode);
}

static bool conf_enabled(void)
{
	char line[64];
	bool on = false;
	FILE *fp = fopen(CONF_PATH, "r");

	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp))
		if (!strncmp(line, "ENABLED=", 8))
			on = line[8] == '1';
	fclose(fp);
	return on;
}

static int conf_save(bool on)
{
	char dir[PATH_MAX], tmp[PATH_MAX];
	FILE *fp;

	snprintf(dir, sizeof(dir), "%s", CONF_PATH);
	*strrchr(dir, '/') = '\0';
	mkdir_parents(dir, 0755);
	snprintf(tmp, sizeof(tmp), "%s.tmp", CONF_PATH);
	fp = fopen(tmp, "w");
	if (!fp)
		return -1;
	fprintf(fp, "ENABLED=%d\n", on);
	if (fflush(fp) || fsync(fileno(fp))) {
		fclose(fp);
		unlink(tmp);
		return -1;
	}
	fclose(fp);
	return rename(tmp, CONF_PATH) ? -1 : 0;
}

static bool installed(void)
{
	return access(APP_BIN, X_OK) == 0;
}

/* "nuubOS RG CubeXX" (Tailscale makes it "nuubos-rg-cubexx"). */
static void host_name(char *out, size_t size)
{
	char model[96] = "";
	FILE *fp = fopen(MODEL_FILE, "r");

	if (fp) {
		size_t n = fread(model, 1, sizeof(model) - 1, fp);

		model[n] = '\0';
		fclose(fp);
	}
	model[strcspn(model, "\n")] = '\0';
	for (size_t n = strlen(model); n && isspace((unsigned char)model[n - 1]);)
		model[--n] = '\0';
	snprintf(out, size, "nuubOS %s", strncmp(model, "Anbernic ", 9) ? model : model + 9);
}

/* ---------------------------------------------------------------- */
/* Local API                                                        */
/* ---------------------------------------------------------------- */

struct buffer {
	char *data;
	size_t len;
};

static size_t collect(char *ptr, size_t size, size_t n, void *userdata)
{
	struct buffer *b = userdata;
	size_t len = size * n;
	char *p = realloc(b->data, b->len + len + 1);

	if (!p)
		return 0;
	b->data = p;
	memcpy(b->data + b->len, ptr, len);
	b->len += len;
	b->data[b->len] = '\0';
	return len;
}

static CURL *api_request(const char *method, const char *path, struct curl_slist **h)
{
	char url[256];
	CURL *curl = curl_easy_init();

	if (!curl)
		return NULL;
	snprintf(url, sizeof(url), "http://local-tailscaled.sock%s", path);
	/* The local API refuses requests without it (browser CSRF guard). */
	*h = curl_slist_append(NULL, "Sec-Tailscale: localapi");
	*h = curl_slist_append(*h, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, TS_SOCKET);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *h);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	return curl;
}

/* One call: the parsed JSON (or NULL), the HTTP status in *status (-1 when
 * tailscaled did not answer). */
static cJSON *api(const char *method, const char *path, const char *body, long *status)
{
	struct buffer b = { 0 };
	struct curl_slist *h = NULL;
	CURL *curl = api_request(method, path, &h);
	cJSON *json = NULL;

	*status = -1;
	if (!curl)
		return NULL;
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	if (curl_easy_perform(curl) == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
	curl_slist_free_all(h);
	curl_easy_cleanup(curl);
	if (b.data)
		json = cJSON_Parse(b.data);
	if (*status >= 400 && b.data)
		fprintf(stderr, "tsd: %s %s: %ld %.200s\n", method, path, *status, b.data);
	free(b.data);
	return json;
}

static bool api_ok(const char *method, const char *path, cJSON *body)
{
	char *text = body ? cJSON_PrintUnformatted(body) : NULL;
	long status;

	cJSON_Delete(api(method, path, text, &status));
	free(text);
	return status >= 200 && status < 300;
}

static const char *jstr(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

	return cJSON_IsString(v) ? v->valuestring : "";
}

/* The nuubOS policy (MaskedPrefs: each field with its "...Set"). */
static bool apply_policy(void)
{
	cJSON *p = cJSON_CreateObject();
	char name[128];
	bool ok;

	host_name(name, sizeof(name));
	cJSON_AddBoolToObject(p, "WantRunning", true);
	cJSON_AddBoolToObject(p, "WantRunningSet", true);
	/* /etc/resolv.conf stays the network's. */
	cJSON_AddBoolToObject(p, "CorpDNS", false);
	cJSON_AddBoolToObject(p, "CorpDNSSet", true);
	/* No iptables on the console. */
	cJSON_AddNumberToObject(p, "NetfilterMode", 0);
	cJSON_AddBoolToObject(p, "NetfilterModeSet", true);
	cJSON_AddBoolToObject(p, "RouteAll", false);
	cJSON_AddBoolToObject(p, "RouteAllSet", true);
	cJSON_AddStringToObject(p, "Hostname", name);
	cJSON_AddBoolToObject(p, "HostnameSet", true);
	ok = api_ok("PATCH", "/localapi/v0/prefs", p);
	cJSON_Delete(p);
	return ok;
}

/* ---------------------------------------------------------------- */
/* Event bus (own thread)                                           */
/* ---------------------------------------------------------------- */

/* Every notification (state, sign-in URL, network map...) only wakes the
 * main thread: it reads the status itself. */
static size_t bus_data(char *ptr, size_t size, size_t n, void *userdata)
{
	unsigned gen = *(unsigned *)userdata;
	bool current;

	(void)ptr;
	pthread_mutex_lock(&ev_lock);
	current = ev_active && ev_gen == gen;
	pthread_mutex_unlock(&ev_lock);
	if (!current)
		return 0;
	wake();
	return size * n;
}

static void *event_thread(void *arg)
{
	(void)arg;
	for (;;) {
		unsigned gen;

		pthread_mutex_lock(&ev_lock);
		while (!ev_active)
			pthread_cond_wait(&ev_cond, &ev_lock);
		gen = ev_gen;
		pthread_mutex_unlock(&ev_lock);

		for (int failures = 0; failures < 5;) {
			struct curl_slist *h = NULL;
			/* mask 2 = NotifyInitialState: the current state first. */
			CURL *curl = api_request("GET", "/localapi/v0/watch-ipn-bus?mask=2", &h);
			bool current;

			if (curl) {
				curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bus_data);
				curl_easy_setopt(curl, CURLOPT_WRITEDATA, &gen);
				/* Held open for as long as tailscaled runs. */
				curl_easy_perform(curl);
				curl_slist_free_all(h);
				curl_easy_cleanup(curl);
			}
			pthread_mutex_lock(&ev_lock);
			current = ev_active && ev_gen == gen;
			pthread_mutex_unlock(&ev_lock);
			if (!current)
				break;
			/* tailscaled stopping: the main loop clears ev_active. */
			failures++;
			sleep(1);
		}
		pthread_mutex_lock(&ev_lock);
		if (ev_gen == gen)
			ev_active = false;
		pthread_mutex_unlock(&ev_lock);
	}
	return NULL;
}

static void stop_events(void)
{
	pthread_mutex_lock(&ev_lock);
	ev_active = false;
	ev_gen++;
	pthread_mutex_unlock(&ev_lock);
}

/* ---------------------------------------------------------------- */
/* tailscaled                                                       */
/* ---------------------------------------------------------------- */

static void on_ready(void)
{
	ready = true;
	if (!apply_policy())
		fprintf(stderr, "tsd: policy not fully applied\n");
	pthread_mutex_lock(&ev_lock);
	ev_active = true;
	ev_gen++;
	pthread_cond_signal(&ev_cond);
	pthread_mutex_unlock(&ev_lock);
	fprintf(stderr, "tsd: tailscaled ready\n");
}

static void on_alarm(int sig)
{
	(void)sig;
}

/* tailscaled closes its state on SIGTERM: wait for it, at most 10 s. */
static void stop_child(void)
{
	struct sigaction sa, old;

	if (child <= 0)
		return;
	stop_events();
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
	ready = false;
	unlink(TS_SOCKET);
}

static void start_child(void)
{
	pid_t pid;

	mkdir(TS_DIR, 0700);
	mkdir_parents(STATE_DIR, 0700);
	unlink(TS_SOCKET);
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		int log = open(TS_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		char *argv[] = {
			(char *)APP_BIN, (char *)"--statedir=" STATE_DIR, (char *)"--socket=" TS_SOCKET,
			(char *)"--port=41641", (char *)"--tun=tailscale0", (char *)"--no-logs-no-support", NULL,
		};
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		if (log >= 0) {
			dup2(log, STDOUT_FILENO);
			dup2(log, STDERR_FILENO);
		}
		clearenv();
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		setenv("HOME", STATE_DIR, 1);
		/* Background work must not slow a game down. */
		if (nice(10) < 0)
			perror("nice");
		execv(APP_BIN, argv);
		_exit(127);
	}
	child = pid;
	ready = false;
	failed = false;
	fprintf(stderr, "tsd: tailscaled started (pid %d)\n", (int)pid);
}

static void reconcile(void)
{
	bool want;

	enabled = conf_enabled();
	want = enabled && installed() && !(job > 0 && strcmp(job_kind, "check"));
	if (child > 0 && !want)
		stop_child();
	if (want && child <= 0 && !failed)
		start_child();
	/* The socket may already be there (an inotify event coalesced). */
	if (child > 0 && !ready && access(TS_SOCKET, F_OK) == 0)
		on_ready();
}

/* ---------------------------------------------------------------- */
/* Download helper                                                  */
/* ---------------------------------------------------------------- */

static bool start_job(const char *kind)
{
	int fds[2];
	pid_t pid;

	if (job > 0 || pipe2(fds, O_CLOEXEC))
		return false;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	if (pid == 0) {
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		dup2(fds[1], STDOUT_FILENO);
		if (nice(10) < 0)
			perror("nice");
		if (!strcmp(kind, "check"))
			execl(GET_BIN, GET_BIN, "check", (char *)NULL);
		else
			execl(GET_BIN, GET_BIN, kind, APP_ROOT, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	job = pid;
	job_fd = fds[0];
	job_used = 0;
	snprintf(job_kind, sizeof(job_kind), "%s", kind);
	job_error[0] = '\0';
	progress = strcmp(kind, "install") ? -1 : 0;
	return true;
}

static void job_event(const char *line)
{
	if (!strncmp(line, "@progress ", 10))
		progress = atoi(line + 10);
	else if (!strncmp(line, "@latest ", 8))
		snprintf(latest, sizeof(latest), "%.31s", line + 8);
	else if (!strncmp(line, "@error ", 7))
		snprintf(job_error, sizeof(job_error), "%.15s", line + 7);
	else if (!strncmp(line, "@done", 5) && !strcmp(job_kind, "install"))
		snprintf(latest, sizeof(latest), "%.31s", line[5] == ' ' ? line + 6 : "");
}

static void read_job(void)
{
	char b[256];
	ssize_t n;

	while ((n = read(job_fd, b, sizeof(b))) > 0)
		for (ssize_t i = 0; i < n; i++) {
			if (b[i] == '\n') {
				job_line[job_used] = '\0';
				job_used = 0;
				job_event(job_line);
			} else if (job_used + 1 < sizeof(job_line)) {
				job_line[job_used++] = b[i];
			}
		}
	if (n == 0) {
		close(job_fd);
		job_fd = -1;
	}
}

/* The helper exited: what it did decides what runs next. */
static void job_done(int status)
{
	bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	fprintf(stderr, "tsd: %s %s%s%s\n", job_kind, ok ? "done" : "failed", job_error[0] ? " " : "", job_error);
	if (job_fd >= 0) {
		read_job();
		if (job_fd >= 0)
			close(job_fd);
		job_fd = -1;
	}
	if (!strcmp(job_kind, "install") && ok && enable_after_install && conf_save(true) == 0)
		failed = false;
	if (!strcmp(job_kind, "remove")) {
		/* The node's identity goes with the application. */
		char cmd[PATH_MAX];

		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", STATE_DIR);
		if (system(cmd) != 0)
			fprintf(stderr, "tsd: %s not removed\n", STATE_DIR);
		latest[0] = '\0';
	}
	enable_after_install = false;
	job = -1;
	job_kind[0] = '\0';
	progress = -1;
}

/* ---------------------------------------------------------------- */
/* Snapshot                                                         */
/* ---------------------------------------------------------------- */

static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static char *build_snapshot(void)
{
	char *text = NULL, version[32] = "";
	size_t len = 0;
	FILE *out = open_memstream(&text, &len);
	bool have = installed();

	if (!out)
		return NULL;
	if (have)
		read_line(APP_VERSION, version, sizeof(version));
	clean(version);
	fprintf(out, "installed=%d\nversion=%s\nlatest=%s\njob=%s\nprogress=%d\nerror=%s\n", have, version, latest,
		job_kind, progress, job_error);
	fprintf(out, "enabled=%d\nrunning=%d\nready=%d\nfailed=%d\n", enabled, child > 0, child > 0 && ready, failed);
	if (child > 0 && ready) {
		long status;
		cJSON *st = api("GET", "/localapi/v0/status?peers=false", NULL, &status);
		const cJSON *self = cJSON_GetObjectItemCaseSensitive(st, "Self");
		const cJSON *ips = cJSON_GetObjectItemCaseSensitive(self, "TailscaleIPs"), *a;
		char state[32], url[256], name[256], tailnet[128], ip[64] = "", ip6[64] = "";

		snprintf(state, sizeof(state), "%s", jstr(st, "BackendState"));
		snprintf(url, sizeof(url), "%s", jstr(st, "AuthURL"));
		snprintf(name, sizeof(name), "%s", jstr(self, "DNSName"));
		snprintf(tailnet, sizeof(tailnet), "%s",
			 jstr(cJSON_GetObjectItemCaseSensitive(st, "CurrentTailnet"), "Name"));
		/* "nuubos-rg-cubexx.tail1234.ts.net." */
		if (name[0] && name[strlen(name) - 1] == '.')
			name[strlen(name) - 1] = '\0';
		cJSON_ArrayForEach(a, ips) {
			if (!cJSON_IsString(a))
				continue;
			if (strchr(a->valuestring, ':'))
				snprintf(ip6, sizeof(ip6), "%s", a->valuestring);
			else if (!ip[0])
				snprintf(ip, sizeof(ip), "%s", a->valuestring);
		}
		/* Only https sign-in pages, never anything a client would open blindly. */
		if (strncmp(url, "https://", 8))
			url[0] = '\0';
		clean(state);
		clean(url);
		clean(name);
		clean(tailnet);
		fprintf(out, "state=%s\nauth_url=%s\nname=%s\nip=%s\nip6=%s\ntailnet=%s\n", state, url,
			!strcmp(state, "Running") ? name : "", ip, ip6, tailnet);
		cJSON_Delete(st);
	} else {
		fprintf(out, "state=\nauth_url=\nname=\nip=\nip6=\ntailnet=\n");
	}
	fputs("end=1\n", out);
	fclose(out);
	return text;
}

static int write_all(int fd, const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(fd, s, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		s += w;
		n -= (size_t)w;
	}
	return 0;
}

static void drop_client(struct client *c)
{
	close(c->fd);
	c->fd = -1;
	c->sub = false;
	c->used = 0;
}

static void publish(void)
{
	char *next = build_snapshot();

	if (!next)
		return;
	if (snapshot && !strcmp(snapshot, next)) {
		free(next);
		return;
	}
	free(snapshot);
	snapshot = next;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].sub && write_all(clients[i].fd, snapshot, strlen(snapshot)))
			drop_client(&clients[i]);
}

/* ---------------------------------------------------------------- */
/* Commands                                                         */
/* ---------------------------------------------------------------- */

static const char *cmd_job(const char *kind)
{
	if (job > 0)
		return "busy";
	if (!strcmp(kind, "install"))
		enable_after_install = true;
	if (!strcmp(kind, "remove")) {
		if (!installed())
			return "not-installed";
		/* Leave the tailnet first (the node disappears from it). */
		if (child > 0 && ready && !api_ok("POST", "/localapi/v0/logout", NULL))
			fprintf(stderr, "tsd: logout before removal failed\n");
		if (conf_save(false))
			return "storage";
		enabled = false;
		failed = false;
		stop_child();
	}
	if (!strcmp(kind, "install") || !strcmp(kind, "remove")) {
		/* The binary is replaced: tailscaled must not run from it. */
		stop_child();
	}
	return start_job(kind) ? NULL : "storage";
}

static const char *cmd_enable(bool on)
{
	if (on && !installed())
		return "not-installed";
	if (conf_save(on))
		return "storage";
	if (on)
		failed = false;
	reconcile();
	return NULL;
}

static const char *cmd_api(const char *what)
{
	if (child <= 0 || !ready)
		return "not-running";
	if (!strcmp(what, "LOGIN")) {
		cJSON *p = cJSON_CreateObject();
		bool ok;

		cJSON_AddBoolToObject(p, "WantRunning", true);
		cJSON_AddBoolToObject(p, "WantRunningSet", true);
		ok = api_ok("PATCH", "/localapi/v0/prefs", p);
		cJSON_Delete(p);
		return ok && api_ok("POST", "/localapi/v0/login-interactive", NULL) ? NULL : "tailscale";
	}
	return api_ok("POST", "/localapi/v0/logout", NULL) ? NULL : "tailscale";
}

static void command(struct client *c, char *line)
{
	const char *err = "unknown";

	line[strcspn(line, "\r")] = '\0';
	if (!strcmp(line, "STATUS") || !strcmp(line, "SUBSCRIBE")) {
		if (!snapshot)
			publish();
		c->sub = c->sub || !strcmp(line, "SUBSCRIBE");
		if (snapshot && write_all(c->fd, snapshot, strlen(snapshot)))
			drop_client(c);
		return;
	}
	if (!strcmp(line, "INSTALL"))
		err = cmd_job("install");
	else if (!strcmp(line, "CHECK"))
		err = cmd_job("check");
	else if (!strcmp(line, "REMOVE"))
		err = cmd_job("remove");
	else if (!strcmp(line, "CANCEL"))
		err = job > 0 && !strcmp(job_kind, "install") && kill(job, SIGTERM) == 0 ? NULL : "unknown";
	else if (!strcmp(line, "ENABLE") || !strcmp(line, "DISABLE"))
		err = cmd_enable(line[0] == 'E');
	else if (!strcmp(line, "LOGIN") || !strcmp(line, "LOGOUT"))
		err = cmd_api(line);
	if (err) {
		char reply[64];

		snprintf(reply, sizeof(reply), "ERR %s\n", err);
		if (write_all(c->fd, reply, strlen(reply)))
			drop_client(c);
		return;
	}
	if (write_all(c->fd, "OK\n", 3))
		drop_client(c);
	reconcile();
	publish();
}

static void service(struct client *c)
{
	char b[256];
	ssize_t n = read(c->fd, b, sizeof(b));

	if (n <= 0) {
		drop_client(c);
		return;
	}
	for (ssize_t i = 0; i < n && c->fd >= 0; i++) {
		if (b[i] == '\n') {
			c->buf[c->used] = '\0';
			c->used = 0;
			command(c, c->buf);
		} else if (c->used + 1 < sizeof(c->buf)) {
			c->buf[c->used++] = b[i];
		} else {
			c->used = 0;
		}
	}
}

static int listener(void)
{
	struct sockaddr_un a;
	int fd;

	unlink(SOCKET_PATH);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", SOCKET_PATH);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 8)) {
		close(fd);
		return -1;
	}
	chmod(SOCKET_PATH, 0660);
	return fd;
}

static void accept_clients(int l)
{
	for (;;) {
		int fd = accept4(l, NULL, NULL, SOCK_CLOEXEC), i;

		if (fd < 0)
			return;
		for (i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd < 0)
				break;
		if (i == MAX_CLIENTS) {
			close(fd);
			continue;
		}
		clients[i] = (struct client){ .fd = fd };
	}
}

int main(void)
{
	struct sigaction sa;
	pthread_t thread;
	sigset_t block, old;
	int in, l, ts_wd = -1;
	FILE *fp;

	if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;
	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	curl_global_init(CURL_GLOBAL_DEFAULT);
	signal(SIGPIPE, SIG_IGN);
	/* The event thread never takes the signals. */
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGCHLD);
	sigaddset(&block, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &block, &old);
	if (pthread_create(&thread, NULL, event_thread, NULL) != 0)
		return 1;
	pthread_detach(thread);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);
	mkdir(RUN_ROOT, 0755);
	mkdir(TS_DIR, 0700);
	in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (in < 0 || (ts_wd = inotify_add_watch(in, TS_DIR, IN_CREATE)) < 0)
		fprintf(stderr, "tsd: inotify unavailable\n");
	l = listener();
	if (l < 0)
		return 1;
	fp = fopen(PIDFILE, "w");
	if (fp) {
		fprintf(fp, "%ld\n", (long)getpid());
		fclose(fp);
	}
	reconcile();
	publish();
	while (!stop_requested) {
		struct pollfd p[4 + MAX_CLIENTS];
		int map[4 + MAX_CLIENTS], n = 0, status;
		bool changed = false;
		char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
		ssize_t len;
		pid_t pid;

		p[n] = (struct pollfd){ wake_pipe[0], POLLIN, 0 };
		map[n++] = -1;
		p[n] = (struct pollfd){ l, POLLIN, 0 };
		map[n++] = -2;
		if (in >= 0) {
			p[n] = (struct pollfd){ in, POLLIN, 0 };
			map[n++] = -3;
		}
		if (job_fd >= 0) {
			p[n] = (struct pollfd){ job_fd, POLLIN, 0 };
			map[n++] = -4;
		}
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0) {
				p[n] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
				map[n++] = i;
			}
		if (poll(p, (nfds_t)n, -1) < 0 && errno != EINTR)
			break;
		for (int j = 0; j < n; j++) {
			if (!p[j].revents)
				continue;
			if (map[j] == -1) {
				while (read(wake_pipe[0], buf, sizeof(buf)) > 0)
					;
				/* Bus notifications, a signal or a child. */
				changed = true;
			} else if (map[j] == -2) {
				accept_clients(l);
			} else if (map[j] == -3) {
				while ((len = read(in, buf, sizeof(buf))) > 0)
					for (char *q = buf; q < buf + len;) {
						const struct inotify_event *ev = (const struct inotify_event *)q;

						if (ev->wd == ts_wd && ev->len && !strcmp(ev->name, TS_NAME))
							changed = true;
						q += sizeof(*ev) + ev->len;
					}
			} else if (map[j] == -4) {
				read_job();
				changed = true;
			} else if (p[j].revents & (POLLHUP | POLLERR)) {
				drop_client(&clients[map[j]]);
			} else {
				service(&clients[map[j]]);
			}
		}
		while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
			if (pid == child) {
				fprintf(stderr, "tsd: tailscaled exited status=%d\n", status);
				stop_events();
				child = -1;
				ready = false;
				failed = true;
				changed = true;
			} else if (pid == job) {
				job_done(status);
				changed = true;
			}
		}
		if (changed) {
			reconcile();
			publish();
		}
	}
	if (job > 0)
		kill(job, SIGTERM);
	stop_child();
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0)
			close(clients[i].fd);
	close(l);
	unlink(SOCKET_PATH);
	unlink(PIDFILE);
	return 0;
}
