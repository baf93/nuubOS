/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Screen capture through wlr-screencopy-unstable-v1 (labwc/wlroots).
 */
#ifndef NUUBOS_CAST_CAPTURE_H
#define NUUBOS_CAST_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

struct capture;

struct capture_image {
	const uint8_t *data;
	int width;      /* buffer size (output pixels, before the transform) */
	int height;
	int stride;     /* bytes */
	uint32_t format; /* wl_shm format */
	bool y_invert;
	int transform;  /* wl_output transform of the captured output */
	int64_t time_us; /* capture time, CLOCK_MONOTONIC */
};

struct capture *capture_open(void);
void capture_close(struct capture *c);
int capture_fd(struct capture *c);
/* Before poll(): flush requests and prepare the read. */
int capture_prepare(struct capture *c);
/* After poll(): read and dispatch events (`readable` = POLLIN seen). */
int capture_dispatch(struct capture *c, bool readable);
/* Starts the copy of the next frame. With `damage` the compositor answers
 * only once the screen changed (no work while it is static). */
int capture_request(struct capture *c, bool damage);
bool capture_busy(const struct capture *c);
/* Sends queued requests now (a request made between two polls). */
void capture_flush(struct capture *c);
/* A finished frame: valid until the second capture_request() after it
 * (double buffering: request the next frame first, then process this one). */
bool capture_take(struct capture *c, struct capture_image *out);
/* The output disappeared or the compositor went away. */
bool capture_broken(const struct capture *c);

#endif
