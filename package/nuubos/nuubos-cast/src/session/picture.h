/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Captured screen → I420 (YUV 4:2:0, BT.601 limited range) at the encoded
 * size: output transform undone, scaled, letterboxed when the aspect ratio
 * of the stream is fixed (Miracast modes).
 */
#ifndef NUUBOS_CAST_PICTURE_H
#define NUUBOS_CAST_PICTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "capture.h"

struct picture {
	int width;
	int height;
	uint8_t *plane[3];
	int stride[3];
	int64_t time_us;
};

int picture_alloc(struct picture *p, int width, int height);
void picture_free(struct picture *p);
void picture_black(struct picture *p);

/* Logical (upright) size of a captured image. */
void capture_logical_size(const struct capture_image *img, int *width, int *height);

/* Stream size for a screen: the logical size fitted in max_w x max_h,
 * even dimensions. */
void fit_size(int src_w, int src_h, int max_w, int max_h, int *out_w, int *out_h);

struct converter;
struct converter *converter_new(void);
void converter_free(struct converter *cv);
/* Converts `img` into `dst` (its full size, or the centred letterbox
 * rectangle when `letterbox`). */
int converter_run(struct converter *cv, const struct capture_image *img, struct picture *dst,
		  bool letterbox);

#endif
