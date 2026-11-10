/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "castv2.h"
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

/* Receivers reject messages over 64 KiB. */
#define MAX_MESSAGE (64 * 1024)

struct castv2 {
	int fd;
	SSL_CTX *ctx;
	SSL *ssl;
	struct bytes in;
	char *payload;
};

struct castv2 *castv2_connect(const char *address, int port, int timeout_ms)
{
	struct castv2 *ch = calloc(1, sizeof(*ch));
	int64_t deadline = now_ms() + timeout_ms;

	if (!ch)
		return NULL;
	ch->fd = tcp_connect(address, port, timeout_ms);
	if (ch->fd < 0) {
		log_msg("cannot reach %s:%d", address, port);
		free(ch);
		return NULL;
	}
	ch->ctx = SSL_CTX_new(TLS_client_method());
	if (!ch->ctx)
		goto fail;
	SSL_CTX_set_verify(ch->ctx, SSL_VERIFY_NONE, NULL);
	ch->ssl = SSL_new(ch->ctx);
	if (!ch->ssl || SSL_set_fd(ch->ssl, ch->fd) != 1)
		goto fail;
	set_nonblock(ch->fd);
	for (;;) {
		int rc = SSL_connect(ch->ssl);
		int err;
		struct pollfd pfd = { ch->fd, 0, 0 };
		int64_t left = deadline - now_ms();

		if (rc == 1)
			break;
		err = SSL_get_error(ch->ssl, rc);
		if (err == SSL_ERROR_WANT_READ)
			pfd.events = POLLIN;
		else if (err == SSL_ERROR_WANT_WRITE)
			pfd.events = POLLOUT;
		else
			goto fail;
		if (left <= 0 || poll(&pfd, 1, (int)left) <= 0)
			goto fail;
	}
	return ch;
fail:
	log_msg("TLS handshake with %s failed", address);
	castv2_close(ch);
	return NULL;
}

void castv2_close(struct castv2 *ch)
{
	if (!ch)
		return;
	if (ch->ssl) {
		SSL_shutdown(ch->ssl);
		SSL_free(ch->ssl);
	}
	if (ch->ctx)
		SSL_CTX_free(ch->ctx);
	if (ch->fd >= 0)
		close(ch->fd);
	bytes_free(&ch->in);
	free(ch->payload);
	free(ch);
}

int castv2_fd(const struct castv2 *ch)
{
	return ch->fd;
}

/* Namespace without the common prefix, for the log. */
static const char *short_ns(const char *ns)
{
	static const char prefix[] = "urn:x-cast:com.google.cast.";

	return strncmp(ns, prefix, sizeof(prefix) - 1) ? ns : ns + sizeof(prefix) - 1;
}

/* ---- protobuf (proto2 CastMessage, fields 1-7) ---- */

static size_t put_varint(uint8_t *p, uint64_t v)
{
	size_t n = 0;

	do {
		uint8_t b = v & 0x7f;

		v >>= 7;
		p[n++] = v ? (uint8_t)(b | 0x80) : b;
	} while (v);
	return n;
}

static int put_string(struct bytes *b, int field, const char *s)
{
	uint8_t head[11];
	size_t len = strlen(s);
	size_t n = 0;

	head[n++] = (uint8_t)(field << 3 | 2);
	n += put_varint(head + n, len);
	if (bytes_append(b, head, n) != 0)
		return -1;
	return bytes_append(b, s, len);
}

static int ssl_write_all(struct castv2 *ch, const uint8_t *p, size_t len)
{
	while (len > 0) {
		int n = SSL_write(ch->ssl, p, (int)len);

		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		switch (SSL_get_error(ch->ssl, n)) {
		case SSL_ERROR_WANT_WRITE:
		case SSL_ERROR_WANT_READ: {
			struct pollfd pfd = { ch->fd, POLLOUT | POLLIN, 0 };

			if (poll(&pfd, 1, 3000) <= 0)
				return -1;
			break;
		}
		default:
			return -1;
		}
	}
	return 0;
}

int castv2_send(struct castv2 *ch, const char *source, const char *destination, const char *ns,
		const char *payload)
{
	struct bytes msg = { 0 };
	uint8_t frame[4];
	static const uint8_t version[] = { 0x08, 0x00 };      /* protocol_version CASTV2_1_0 */
	static const uint8_t payload_type[] = { 0x28, 0x00 }; /* payload_type STRING */
	int rc = -1;

	if (bytes_append(&msg, version, sizeof(version)) == 0 &&
	    put_string(&msg, 2, source) == 0 &&
	    put_string(&msg, 3, destination) == 0 &&
	    put_string(&msg, 4, ns) == 0 &&
	    bytes_append(&msg, payload_type, sizeof(payload_type)) == 0 &&
	    put_string(&msg, 6, payload) == 0 &&
	    msg.len <= MAX_MESSAGE) {
		put_be32(frame, (uint32_t)msg.len);
		if (ssl_write_all(ch, frame, 4) == 0 && ssl_write_all(ch, msg.data, msg.len) == 0)
			rc = 0;
	}
	if (strcmp(ns, NS_HEARTBEAT))
		log_msg("cast > %s %s %.300s", destination, short_ns(ns), payload);
	bytes_free(&msg);
	return rc;
}

int castv2_read(struct castv2 *ch)
{
	uint8_t buf[8192];

	for (;;) {
		int n = SSL_read(ch->ssl, buf, sizeof(buf));

		if (n > 0) {
			if (bytes_append(&ch->in, buf, (size_t)n) != 0 || ch->in.len > 4 * MAX_MESSAGE)
				return -1;
			continue;
		}
		switch (SSL_get_error(ch->ssl, n)) {
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
			return 0;
		default:
			return -1;
		}
	}
}

bool castv2_pending(const struct castv2 *ch)
{
	return SSL_pending(ch->ssl) > 0;
}

static bool get_varint(const uint8_t **p, const uint8_t *end, uint64_t *out)
{
	uint64_t v = 0;
	int shift = 0;

	while (*p < end && shift < 64) {
		uint8_t b = *(*p)++;

		v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80)) {
			*out = v;
			return true;
		}
		shift += 7;
	}
	return false;
}

bool castv2_next(struct castv2 *ch, struct castv2_message *out)
{
	while (ch->in.len >= 4) {
		uint32_t len = get_be32(ch->in.data);
		const uint8_t *p, *end;
		bool ok = true;

		if (len > MAX_MESSAGE) {
			/* Corrupt stream: drop everything. */
			ch->in.len = 0;
			return false;
		}
		if (ch->in.len < 4 + (size_t)len)
			return false;
		memset(out, 0, sizeof(*out));
		free(ch->payload);
		ch->payload = NULL;
		p = ch->in.data + 4;
		end = p + len;
		while (p < end && ok) {
			uint64_t key, v;
			int field, wire;

			if (!get_varint(&p, end, &key)) {
				ok = false;
				break;
			}
			field = (int)(key >> 3);
			wire = (int)(key & 7);
			if (wire == 0) {
				ok = get_varint(&p, end, &v);
			} else if (wire == 2) {
				if (!get_varint(&p, end, &v) || v > (uint64_t)(end - p)) {
					ok = false;
					break;
				}
				if (field == 2)
					snprintf(out->source, sizeof(out->source), "%.*s", (int)v, (const char *)p);
				else if (field == 3)
					snprintf(out->destination, sizeof(out->destination), "%.*s", (int)v, (const char *)p);
				else if (field == 4)
					snprintf(out->ns, sizeof(out->ns), "%.*s", (int)v, (const char *)p);
				else if (field == 6 && !ch->payload) {
					ch->payload = malloc(v + 1);
					if (ch->payload) {
						memcpy(ch->payload, p, v);
						ch->payload[v] = '\0';
					}
				}
				p += v;
			} else if (wire == 5) {
				p += 4;
			} else if (wire == 1) {
				p += 8;
			} else {
				ok = false;
			}
		}
		bytes_consume(&ch->in, 4 + (size_t)len);
		if (!ok || !ch->payload)
			continue; /* binary payloads (deviceauth) are not used */
		out->payload = ch->payload;
		if (strcmp(out->ns, NS_HEARTBEAT))
			log_msg("cast < %s %s %.300s", out->source, short_ns(out->ns), out->payload);
		return true;
	}
	return false;
}
