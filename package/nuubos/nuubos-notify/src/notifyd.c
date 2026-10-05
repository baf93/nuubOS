/* SPDX-License-Identifier: MIT */
/*
 * nuubos-notifyd — Notification router (EPIC-006).
 *
 * Owning services post typed notification events (see notify.h); renderers
 * subscribe and present them. The router only validates, de-duplicates and
 * fans out. It keeps no history: the only state it holds is the set of
 * currently running Live Notifications, replayed to a renderer that
 * (re)subscribes so an in-progress operation is not lost when the UI
 * session restarts.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "notify.h"

#ifndef PIDFILE
#define PIDFILE "/run/nuubos/notifyd.pid"
#endif
#define MAX_CLIENTS 16
#define MAX_LIVE 8
#define MAX_RECENT 16
#define MAX_ID 96
/* Identical messages inside this window are one event (e.g. a reconnect
 * storm or two refresh paths observing the same transition). */
#define DUPLICATE_WINDOW_MS 3000LL

struct client {
	int fd;
	bool subscribed;
	char buf[NUUBOS_NOTIFY_MAX_LINE];
	size_t used;
};

struct live_entry {
	char id[MAX_ID];
	char line[NUUBOS_NOTIFY_MAX_LINE];
};

struct recent_entry {
	uint32_t hash;
	long long at_ms;
};

static volatile sig_atomic_t running = 1;
static struct client clients[MAX_CLIENTS];
static struct live_entry live[MAX_LIVE];
static struct recent_entry recent[MAX_RECENT];
static size_t recent_next;

static void on_signal(int signo)
{
	(void)signo;
	running = 0;
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static uint32_t fnv1a(const char *s)
{
	uint32_t h = 2166136261u;

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

static void close_client(struct client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->subscribed = false;
	c->used = 0;
}

static void send_line(struct client *c, const char *line)
{
	size_t len = strlen(line);

	if (c->fd < 0)
		return;
	if (send(c->fd, line, len, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)len &&
	    errno != EAGAIN && errno != EWOULDBLOCK)
		close_client(c);
}

static bool id_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
	       c == '-';
}

static bool valid_token(const char *value, size_t max, bool lower_only)
{
	size_t len = strlen(value);

	if (len == 0 || len >= max)
		return false;
	for (size_t i = 0; i < len; i++) {
		if (!id_char(value[i]))
			return false;
		if (lower_only && value[i] >= 'A' && value[i] <= 'Z')
			return false;
	}
	return true;
}

/*
 * Validate "VERB key=value ..." in place. Values are percent-encoded by the
 * producer, so a raw space only ever separates fields.
 */
static bool parse_message(char *line, const char **verb, char *id, size_t id_size,
			  bool *has_event, bool *has_progress)
{
	char copy[NUUBOS_NOTIFY_MAX_LINE];
	char *save = NULL;
	char *tok;

	*has_event = false;
	*has_progress = false;
	id[0] = '\0';
	snprintf(copy, sizeof(copy), "%s", line);

	tok = strtok_r(copy, " ", &save);
	if (!tok)
		return false;
	if (!strcmp(tok, "POST"))
		*verb = "POST";
	else if (!strcmp(tok, "UPDATE"))
		*verb = "UPDATE";
	else if (!strcmp(tok, "DISMISS"))
		*verb = "DISMISS";
	else
		return false;

	while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
		char *eq = strchr(tok, '=');
		size_t key_len;

		if (!eq || eq == tok)
			return false;
		key_len = (size_t)(eq - tok);
		if (key_len > 24)
			return false;
		for (size_t i = 0; i < key_len; i++)
			if (!((tok[i] >= 'a' && tok[i] <= 'z') || tok[i] == '_'))
				return false;
		if (strchr(eq + 1, '='))
			return false;
		*eq = '\0';
		if (!strcmp(tok, "id")) {
			if (!valid_token(eq + 1, id_size, false))
				return false;
			snprintf(id, id_size, "%s", eq + 1);
		} else if (!strcmp(tok, "event")) {
			if (!valid_token(eq + 1, 64, true))
				return false;
			*has_event = true;
		} else if (!strcmp(tok, "progress")) {
			*has_progress = true;
		}
	}

	if (id[0] == '\0')
		return false;
	if (strcmp(*verb, "DISMISS") != 0 && !*has_event)
		return false;
	return true;
}

static bool duplicate(const char *line)
{
	uint32_t hash = fnv1a(line);
	long long now = monotonic_ms();

	for (size_t i = 0; i < MAX_RECENT; i++)
		if (recent[i].at_ms > 0 && recent[i].hash == hash &&
		    now - recent[i].at_ms < DUPLICATE_WINDOW_MS)
			return true;

	recent[recent_next].hash = hash;
	recent[recent_next].at_ms = now;
	recent_next = (recent_next + 1) % MAX_RECENT;
	return false;
}

static void track_live(const char *verb, const char *id, bool has_progress,
		       const char *line)
{
	int slot = -1;
	int free_slot = -1;

	for (int i = 0; i < MAX_LIVE; i++) {
		if (live[i].id[0] == '\0') {
			if (free_slot < 0)
				free_slot = i;
		} else if (!strcmp(live[i].id, id)) {
			slot = i;
		}
	}

	if (strcmp(verb, "DISMISS") == 0 || !has_progress) {
		/* Completion or removal ends the Live Notification. */
		if (slot >= 0)
			live[slot].id[0] = '\0';
		return;
	}
	if (slot < 0)
		slot = free_slot;
	if (slot < 0)
		return;
	snprintf(live[slot].id, sizeof(live[slot].id), "%s", id);
	/* Replay as POST so a fresh renderer shows it even for an UPDATE. */
	if (!strncmp(line, "UPDATE ", 7))
		snprintf(live[slot].line, sizeof(live[slot].line), "POST %s", line + 7);
	else
		snprintf(live[slot].line, sizeof(live[slot].line), "%s", line);
}

static void handle_line(struct client *c, char *line)
{
	char routed[NUUBOS_NOTIFY_MAX_LINE + 1];
	char id[MAX_ID];
	const char *verb = NULL;
	bool has_event;
	bool has_progress;

	if (!strcmp(line, "SUBSCRIBE")) {
		c->subscribed = true;
		send_line(c, "OK protocol=1\n");
		for (int i = 0; i < MAX_LIVE && c->fd >= 0; i++) {
			if (live[i].id[0] == '\0')
				continue;
			snprintf(routed, sizeof(routed), "%s\n", live[i].line);
			send_line(c, routed);
		}
		return;
	}

	if (!parse_message(line, &verb, id, sizeof(id), &has_event, &has_progress)) {
		fprintf(stderr, "notifyd: rejected \"%.120s\"\n", line);
		send_line(c, "ERR invalid notification\n");
		return;
	}

	if (duplicate(line)) {
		send_line(c, "OK duplicate\n");
		return;
	}

	track_live(verb, id, has_progress, line);
	fprintf(stderr, "notifyd: %s\n", line);

	snprintf(routed, sizeof(routed), "%s\n", line);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && clients[i].subscribed)
			send_line(&clients[i], routed);
	send_line(c, "OK\n");
}

static void service_client(struct client *c)
{
	for (;;) {
		ssize_t n;
		char *nl;

		if (c->used >= sizeof(c->buf) - 1) {
			/* Over-long line: protocol violation. */
			close_client(c);
			return;
		}
		n = recv(c->fd, c->buf + c->used, sizeof(c->buf) - 1 - c->used,
			 MSG_DONTWAIT);
		if (n == 0) {
			close_client(c);
			return;
		}
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				close_client(c);
			return;
		}
		c->used += (size_t)n;
		c->buf[c->used] = '\0';

		while (c->fd >= 0 && (nl = memchr(c->buf, '\n', c->used)) != NULL) {
			size_t line_len = (size_t)(nl - c->buf);
			char line[NUUBOS_NOTIFY_MAX_LINE];

			memcpy(line, c->buf, line_len);
			line[line_len] = '\0';
			if (line_len > 0 && line[line_len - 1] == '\r')
				line[line_len - 1] = '\0';
			memmove(c->buf, nl + 1, c->used - line_len - 1);
			c->used -= line_len + 1;
			if (line[0] != '\0')
				handle_line(c, line);
		}
		if (c->fd < 0)
			return;
	}
}

static int make_listener(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", NUUBOS_NOTIFY_SOCKET);
	unlink(NUUBOS_NOTIFY_SOCKET);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 32) < 0) {
		close(fd);
		return -1;
	}
	chmod(NUUBOS_NOTIFY_SOCKET, 0666);
	return fd;
}

static void accept_clients(int listen_fd)
{
	for (;;) {
		int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		int slot = -1;

		if (fd < 0)
			return;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0) {
				slot = i;
				break;
			}
		}
		if (slot < 0) {
			close(fd);
			continue;
		}
		clients[slot].fd = fd;
		clients[slot].subscribed = false;
		clients[slot].used = 0;
	}
}

int main(void)
{
	struct sigaction sa;
	FILE *pid;
	int listen_fd;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;

	mkdir("/run/nuubos", 0755);
	listen_fd = make_listener();
	if (listen_fd < 0) {
		fprintf(stderr, "notifyd: cannot listen on %s: %s\n",
			NUUBOS_NOTIFY_SOCKET, strerror(errno));
		return 1;
	}
	pid = fopen(PIDFILE, "w");
	if (pid) {
		fprintf(pid, "%ld\n", (long)getpid());
		fclose(pid);
	}

	while (running) {
		struct pollfd fds[1 + MAX_CLIENTS];
		int map[1 + MAX_CLIENTS];
		nfds_t count = 0;

		fds[count] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
		map[count++] = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0)
				continue;
			fds[count] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN };
			map[count++] = i;
		}

		if (poll(fds, count, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (nfds_t i = 0; i < count; i++) {
			if (!fds[i].revents)
				continue;
			if (map[i] < 0) {
				accept_clients(listen_fd);
			} else if (fds[i].revents & POLLIN) {
				service_client(&clients[map[i]]);
			} else {
				close_client(&clients[map[i]]);
			}
		}
	}

	for (int i = 0; i < MAX_CLIENTS; i++)
		close_client(&clients[i]);
	close(listen_fd);
	unlink(NUUBOS_NOTIFY_SOCKET);
	unlink(PIDFILE);
	return 0;
}
