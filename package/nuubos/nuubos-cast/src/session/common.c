/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void log_msg(const char *fmt, ...)
{
	va_list ap;

	fputs("cast-session: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

void emit(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n > sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	(void)write_all(STDOUT_FILENO, line, (size_t)n);
}

void copy_text(char *dst, size_t size, const char *src)
{
	snprintf(dst, size, "%s", src ? src : "");
}

void sanitize(char *text)
{
	for (char *p = text; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r' || *p == '|')
			*p = ' ';
}

int write_all(int fd, const void *data, size_t len)
{
	const uint8_t *p = data;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n < 0 && errno == ENOTSOCK)
			n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd pfd = { fd, POLLOUT, 0 };

				if (poll(&pfd, 1, 2000) <= 0)
					return -1;
				continue;
			}
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int set_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int tcp_connect(const char *address, int port, int timeout_ms)
{
	struct addrinfo hints = { 0 }, *res = NULL;
	char service[16];
	int fd = -1;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%d", port);
	if (getaddrinfo(address, service, &hints, &res) != 0 || !res)
		return -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		struct pollfd pfd;
		int err = 0;
		socklen_t err_len = sizeof(err);
		int one = 1;

		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}
		pfd.fd = fd;
		pfd.events = POLLOUT;
		if (poll(&pfd, 1, timeout_ms) != 1 ||
		    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
			close(fd);
			fd = -1;
			continue;
		}
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
		break;
	}
	freeaddrinfo(res);
	return fd;
}

bool local_address_for(const char *peer, char *out, size_t size)
{
	struct sockaddr_in to = { 0 }, me;
	socklen_t len = sizeof(me);
	int fd;
	bool ok = false;

	to.sin_family = AF_INET;
	to.sin_port = htons(9);
	if (inet_pton(AF_INET, peer, &to.sin_addr) != 1)
		return false;
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	/* A UDP connect only selects the route: nothing is sent. */
	if (connect(fd, (struct sockaddr *)&to, sizeof(to)) == 0 &&
	    getsockname(fd, (struct sockaddr *)&me, &len) == 0 &&
	    inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)size))
		ok = true;
	close(fd);
	return ok;
}

void random_bytes(void *out, size_t len)
{
	uint8_t *p = out;

	while (len > 0) {
		ssize_t n = getrandom(p, len, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			/* No entropy source: never expected on Linux >= 3.17. */
			abort();
		}
		p += n;
		len -= (size_t)n;
	}
}

void hex_encode(const uint8_t *in, size_t len, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < len; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * len] = '\0';
}

int bytes_append(struct bytes *b, const void *data, size_t len)
{
	if (b->len + len > b->cap) {
		size_t cap = b->cap ? b->cap : 1024;
		uint8_t *grown;

		while (cap < b->len + len)
			cap *= 2;
		grown = realloc(b->data, cap);
		if (!grown)
			return -1;
		b->data = grown;
		b->cap = cap;
	}
	if (len)
		memcpy(b->data + b->len, data, len);
	b->len += len;
	return 0;
}

void bytes_consume(struct bytes *b, size_t len)
{
	if (len >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->data, b->data + len, b->len - len);
	b->len -= len;
}

void bytes_free(struct bytes *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}
