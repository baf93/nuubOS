/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-cast-session — shared helpers (logging, time, events, sockets).
 */
#ifndef NUUBOS_CAST_COMMON_H
#define NUUBOS_CAST_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Monotonic clock (CLOCK_MONOTONIC), the time base of every timestamp in
 * the session: capture times, RTP timestamps and RTCP NTP conversion. */
int64_t now_us(void);
static inline int64_t now_ms(void) { return now_us() / 1000; }

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Machine readable lines for nuubos-castd on stdout ("@state casting",
 * "@error unreachable", ...). One write per line. */
void emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void copy_text(char *dst, size_t size, const char *src);
/* Replaces tabs, newlines and '|' (snapshot separators). */
void sanitize(char *text);

int write_all(int fd, const void *data, size_t len);
int set_nonblock(int fd);

/* Blocking TCP connect with a timeout; returns the fd (blocking) or -1. */
int tcp_connect(const char *address, int port, int timeout_ms);
/* Local IPv4 address the kernel uses to reach `peer` (for URLs the
 * receiver fetches back from the console). */
bool local_address_for(const char *peer, char *out, size_t size);

/* Big endian helpers. */
static inline void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static inline void put_be64(uint8_t *p, uint64_t v)
{
	put_be32(p, (uint32_t)(v >> 32));
	put_be32(p + 4, (uint32_t)v);
}
static inline uint16_t get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

void random_bytes(void *out, size_t len);
void hex_encode(const uint8_t *in, size_t len, char *out);

/* A growable byte buffer. */
struct bytes {
	uint8_t *data;
	size_t len;
	size_t cap;
};

int bytes_append(struct bytes *b, const void *data, size_t len);
void bytes_consume(struct bytes *b, size_t len);
void bytes_free(struct bytes *b);

#endif
