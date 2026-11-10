/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Live HTTP stream server: the receiver fetches the screen from the
 * console (DLNA renderers, the Google Cast Default Media Receiver). One
 * path, no length (close-delimited), several clients.
 */
#ifndef NUUBOS_CAST_HTTP_H
#define NUUBOS_CAST_HTTP_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct http_live;

struct http_live *http_live_open(const char *path, const char *content_type, bool dlna);
void http_live_close(struct http_live *h);
int http_live_port(const struct http_live *h);
/* Fills poll entries (listen socket + clients); returns their count. */
int http_live_pollfds(struct http_live *h, struct pollfd *pfd, int max);
/* Handles the entries filled by the last http_live_pollfds(). */
void http_live_dispatch(struct http_live *h, const struct pollfd *pfd, int count);
/* True once for each client that just asked for the stream: the caller
 * restarts the container (headers, key frame) for it. */
bool http_live_take_new_client(struct http_live *h);
/* Sends to every streaming client. */
void http_live_send(struct http_live *h, const uint8_t *data, size_t len);
int http_live_streaming_clients(const struct http_live *h);
/* Monotonic ms of the last time the client count dropped to zero (0 = a
 * client is connected or none ever was). */
int64_t http_live_idle_since(const struct http_live *h);
bool http_live_ever_streamed(const struct http_live *h);
/* Drops every client but the newest streaming one (WebM: one decoder). */
void http_live_keep_newest(struct http_live *h);

#endif
