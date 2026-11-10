/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http.h"
#include "common.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 6
/* A client this far behind (slow Wi-Fi, paused renderer) is dropped. */
#define MAX_BACKLOG (6 * 1024 * 1024)

enum client_state {
	CLIENT_FREE,
	CLIENT_REQUEST,
	CLIENT_STREAMING,
	CLIENT_CLOSING, /* answered (HEAD/404), closes once flushed */
};

struct client {
	int fd;
	enum client_state state;
	char req[2048];
	size_t req_len;
	struct bytes out;
	uint64_t serial;
};

struct http_live {
	int listen_fd;
	int port;
	char path[64];
	char content_type[64];
	bool dlna;
	struct client clients[MAX_CLIENTS];
	int poll_map[MAX_CLIENTS + 1];
	int new_clients;
	int64_t idle_since;
	bool ever;
	uint64_t serial;
};

struct http_live *http_live_open(const char *path, const char *content_type, bool dlna)
{
	struct http_live *h = calloc(1, sizeof(*h));
	struct sockaddr_in addr = { 0 };
	socklen_t len = sizeof(addr);
	int one = 1;

	if (!h)
		return NULL;
	for (int i = 0; i < MAX_CLIENTS; i++)
		h->clients[i].fd = -1;
	copy_text(h->path, sizeof(h->path), path);
	copy_text(h->content_type, sizeof(h->content_type), content_type);
	h->dlna = dlna;
	h->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (h->listen_fd < 0)
		goto fail;
	setsockopt(h->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(h->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(h->listen_fd, 8) != 0 ||
	    getsockname(h->listen_fd, (struct sockaddr *)&addr, &len) != 0)
		goto fail;
	h->port = ntohs(addr.sin_port);
	return h;
fail:
	http_live_close(h);
	return NULL;
}

static void drop_client(struct http_live *h, struct client *c)
{
	bool was_streaming = c->state == CLIENT_STREAMING;

	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->state = CLIENT_FREE;
	c->req_len = 0;
	bytes_free(&c->out);
	if (was_streaming && http_live_streaming_clients(h) == 0)
		h->idle_since = now_ms();
}

void http_live_close(struct http_live *h)
{
	if (!h)
		return;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (h->clients[i].fd >= 0)
			drop_client(h, &h->clients[i]);
	if (h->listen_fd >= 0)
		close(h->listen_fd);
	free(h);
}

int http_live_port(const struct http_live *h)
{
	return h->port;
}

int http_live_pollfds(struct http_live *h, struct pollfd *pfd, int max)
{
	int n = 0;

	if (max < 1)
		return 0;
	pfd[n].fd = h->listen_fd;
	pfd[n].events = POLLIN;
	pfd[n].revents = 0;
	h->poll_map[n++] = -1;
	for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
		struct client *c = &h->clients[i];

		if (c->fd < 0)
			continue;
		pfd[n].fd = c->fd;
		pfd[n].events = POLLIN | (c->out.len ? POLLOUT : 0);
		pfd[n].revents = 0;
		h->poll_map[n++] = i;
	}
	return n;
}

static void queue(struct client *c, const void *data, size_t len)
{
	if (bytes_append(&c->out, data, len) != 0)
		c->out.len = 0;
}

static void answer(struct http_live *h, struct client *c)
{
	char method[8] = "", target[512] = "", header[1024];
	char *line_end = strstr(c->req, "\r\n");
	bool head;
	int n;

	if (line_end)
		*line_end = '\0';
	if (sscanf(c->req, "%7s %511s", method, target) != 2) {
		drop_client(h, c);
		return;
	}
	head = !strcmp(method, "HEAD");
	if ((strcmp(method, "GET") && !head) || strcmp(target, h->path)) {
		static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

		queue(c, nf, sizeof(nf) - 1);
		c->state = CLIENT_CLOSING;
		return;
	}
	/* Close-delimited live body (HTTP/1.0 style): DLNA renderers handle
	 * it better than chunked encoding. */
	n = snprintf(header, sizeof(header),
		     "HTTP/1.1 200 OK\r\n"
		     "Content-Type: %s\r\n"
		     "Cache-Control: no-cache\r\n"
		     "Connection: close\r\n"
		     "Access-Control-Allow-Origin: *\r\n"
		     "%s"
		     "\r\n",
		     h->content_type,
		     h->dlna ? "transferMode.dlna.org: Streaming\r\n"
			       "contentFeatures.dlna.org: DLNA.ORG_OP=00;DLNA.ORG_CI=0;"
			       "DLNA.ORG_FLAGS=01700000000000000000000000000000\r\n" : "");
	queue(c, header, (size_t)n);
	if (head) {
		c->state = CLIENT_CLOSING;
		return;
	}
	c->state = CLIENT_STREAMING;
	c->serial = ++h->serial;
	h->new_clients++;
	h->idle_since = 0;
	h->ever = true;
	log_msg("stream client connected (%d streaming)", http_live_streaming_clients(h));
}

static void flush_client(struct http_live *h, struct client *c)
{
	while (c->out.len) {
		ssize_t n = send(c->fd, c->out.data, c->out.len, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return;
			drop_client(h, c);
			return;
		}
		bytes_consume(&c->out, (size_t)n);
	}
	if (c->state == CLIENT_CLOSING)
		drop_client(h, c);
}

void http_live_dispatch(struct http_live *h, const struct pollfd *pfd, int count)
{
	for (int i = 0; i < count; i++) {
		struct client *c;

		if (!pfd[i].revents)
			continue;
		if (h->poll_map[i] < 0) {
			int fd = accept4(h->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
			int one = 1;

			if (fd < 0)
				continue;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			for (int k = 0; k < MAX_CLIENTS; k++) {
				if (h->clients[k].fd >= 0)
					continue;
				memset(&h->clients[k], 0, sizeof(h->clients[k]));
				h->clients[k].fd = fd;
				h->clients[k].state = CLIENT_REQUEST;
				fd = -1;
				break;
			}
			if (fd >= 0)
				close(fd);
			continue;
		}
		c = &h->clients[h->poll_map[i]];
		if (c->fd < 0)
			continue;
		if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			drop_client(h, c);
			continue;
		}
		if (pfd[i].revents & POLLIN) {
			char buf[1024];
			ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);

			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
				drop_client(h, c);
				continue;
			}
			if (n > 0 && c->state == CLIENT_REQUEST) {
				size_t room = sizeof(c->req) - 1 - c->req_len;

				if ((size_t)n > room)
					n = (ssize_t)room;
				memcpy(c->req + c->req_len, buf, (size_t)n);
				c->req_len += (size_t)n;
				c->req[c->req_len] = '\0';
				if (strstr(c->req, "\r\n\r\n"))
					answer(h, c);
				else if (c->req_len >= sizeof(c->req) - 1)
					drop_client(h, c);
			}
			/* Bytes after the request are ignored. */
		}
		if (c->fd >= 0 && c->out.len)
			flush_client(h, c);
	}
}

bool http_live_take_new_client(struct http_live *h)
{
	if (h->new_clients == 0)
		return false;
	h->new_clients = 0;
	return true;
}

void http_live_send(struct http_live *h, const uint8_t *data, size_t len)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		struct client *c = &h->clients[i];

		if (c->fd < 0 || c->state != CLIENT_STREAMING)
			continue;
		if (c->out.len + len > MAX_BACKLOG) {
			log_msg("stream client too slow, dropped");
			drop_client(h, c);
			continue;
		}
		queue(c, data, len);
		flush_client(h, c);
	}
}

int http_live_streaming_clients(const struct http_live *h)
{
	int n = 0;

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (h->clients[i].fd >= 0 && h->clients[i].state == CLIENT_STREAMING)
			n++;
	return n;
}

int64_t http_live_idle_since(const struct http_live *h)
{
	return h->idle_since;
}

bool http_live_ever_streamed(const struct http_live *h)
{
	return h->ever;
}

void http_live_keep_newest(struct http_live *h)
{
	uint64_t newest = 0;

	for (int i = 0; i < MAX_CLIENTS; i++)
		if (h->clients[i].fd >= 0 && h->clients[i].state == CLIENT_STREAMING &&
		    h->clients[i].serial > newest)
			newest = h->clients[i].serial;
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (h->clients[i].fd >= 0 && h->clients[i].state == CLIENT_STREAMING &&
		    h->clients[i].serial != newest)
			drop_client(h, &h->clients[i]);
}
