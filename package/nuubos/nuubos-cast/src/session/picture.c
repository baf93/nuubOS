/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "picture.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

struct converter {
	struct SwsContext *sws;
	uint32_t *rotated;  /* upright copy for rotated/flipped outputs */
	size_t rotated_size;
	int last_x, last_y, last_w, last_h; /* letterbox rectangle drawn last */
	int last_dst_w, last_dst_h;
};

int picture_alloc(struct picture *p, int width, int height)
{
	size_t luma = (size_t)width * (size_t)height;
	uint8_t *mem = malloc(luma + luma / 2);

	if (!mem)
		return -1;
	p->width = width;
	p->height = height;
	p->plane[0] = mem;
	p->plane[1] = mem + luma;
	p->plane[2] = mem + luma + luma / 4;
	p->stride[0] = width;
	p->stride[1] = width / 2;
	p->stride[2] = width / 2;
	p->time_us = 0;
	picture_black(p);
	return 0;
}

void picture_free(struct picture *p)
{
	free(p->plane[0]);
	memset(p, 0, sizeof(*p));
}

void picture_black(struct picture *p)
{
	size_t luma = (size_t)p->width * (size_t)p->height;

	memset(p->plane[0], 16, luma);
	memset(p->plane[1], 128, luma / 2);
}

static bool transform_swaps(int transform)
{
	return transform == WL_OUTPUT_TRANSFORM_90 || transform == WL_OUTPUT_TRANSFORM_270 ||
	       transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
}

void capture_logical_size(const struct capture_image *img, int *width, int *height)
{
	if (transform_swaps(img->transform)) {
		*width = img->height;
		*height = img->width;
	} else {
		*width = img->width;
		*height = img->height;
	}
}

void fit_size(int src_w, int src_h, int max_w, int max_h, int *out_w, int *out_h)
{
	double scale = 1.0;
	int w, h;

	if (src_w <= 0 || src_h <= 0) {
		*out_w = max_w & ~1;
		*out_h = max_h & ~1;
		return;
	}
	if (src_w > max_w)
		scale = (double)max_w / src_w;
	if (src_h * scale > max_h)
		scale = (double)max_h / src_h;
	w = (int)(src_w * scale + 0.5);
	h = (int)(src_h * scale + 0.5);
	*out_w = w < 2 ? 2 : w & ~1;
	*out_h = h < 2 ? 2 : h & ~1;
}

struct converter *converter_new(void)
{
	struct converter *cv = calloc(1, sizeof(*cv));

	if (cv)
		cv->last_w = -1;
	return cv;
}

void converter_free(struct converter *cv)
{
	if (!cv)
		return;
	sws_freeContext(cv->sws);
	free(cv->rotated);
	free(cv);
}

static enum AVPixelFormat av_format(uint32_t shm_format)
{
	switch (shm_format) {
	case WL_SHM_FORMAT_XRGB8888:
		return AV_PIX_FMT_BGR0; /* little endian 0xXXRRGGBB = B G R X */
	case WL_SHM_FORMAT_ARGB8888:
		return AV_PIX_FMT_BGRA;
	case WL_SHM_FORMAT_XBGR8888:
		return AV_PIX_FMT_RGB0;
	case WL_SHM_FORMAT_ABGR8888:
		return AV_PIX_FMT_RGBA;
	default:
		return AV_PIX_FMT_NONE;
	}
}

/*
 * wl_output.transform is what the compositor applied to the upright
 * (logical) picture to fill the panel's buffer: flip around the vertical
 * axis first (FLIPPED_*), then rotate counter-clockwise. Undo it: for each
 * logical pixel find its buffer pixel.
 */
static void untransform(const struct capture_image *img, uint32_t *out, int lw, int lh)
{
	const int bw = img->width, bh = img->height;
	const int t = img->transform;
	const int rot = t & 3;
	const bool flip = t >= 4;

	for (int y = 0; y < lh; y++) {
		for (int x = 0; x < lw; x++) {
			int fx = flip ? lw - 1 - x : x; /* undo the flip last */
			int bx, by;

			switch (rot) {
			case 1: /* 90 ccw */
				bx = y;
				by = bh - 1 - fx;
				break;
			case 2:
				bx = bw - 1 - fx;
				by = bh - 1 - y;
				break;
			case 3: /* 270 ccw */
				bx = bw - 1 - y;
				by = fx;
				break;
			default:
				bx = fx;
				by = y;
				break;
			}
			if (img->y_invert)
				by = bh - 1 - by;
			out[(size_t)y * lw + x] =
				*(const uint32_t *)(img->data + (size_t)by * img->stride + (size_t)bx * 4);
		}
	}
}

/*
 * Same size, upright 32-bit RGB → I420, BT.601 limited range, chroma
 * averaged over 2x2. swscale's generic path took 17 ms for 720x720 on the
 * Cortex-A53; this loop is what the compiler vectorizes well.
 */
static void rgb32_to_i420(const uint8_t *src, int src_stride, bool bgr, int w, int h, struct picture *dst,
			  int dx, int dy)
{
	const int ri = bgr ? 2 : 0, bi = bgr ? 0 : 2;

	for (int y = 0; y + 1 < h; y += 2) {
		const uint8_t *r0 = src + (size_t)y * src_stride;
		const uint8_t *r1 = r0 + src_stride;
		uint8_t *y0 = dst->plane[0] + (size_t)(dy + y) * dst->stride[0] + dx;
		uint8_t *y1 = y0 + dst->stride[0];
		uint8_t *u = dst->plane[1] + (size_t)((dy + y) / 2) * dst->stride[1] + dx / 2;
		uint8_t *v = dst->plane[2] + (size_t)((dy + y) / 2) * dst->stride[2] + dx / 2;

		for (int x = 0; x + 1 < w; x += 2) {
			const uint8_t *a = r0 + 4 * x, *b = a + 4, *c = r1 + 4 * x, *d = c + 4;
			int ra = a[ri], ga = a[1], ba = a[bi];
			int rb = b[ri], gb = b[1], bb = b[bi];
			int rc = c[ri], gc = c[1], bc = c[bi];
			int rd = d[ri], gd = d[1], bd = d[bi];
			int rs = ra + rb + rc + rd, gs = ga + gb + gc + gd, bs = ba + bb + bc + bd;

			y0[x] = (uint8_t)(((66 * ra + 129 * ga + 25 * ba + 128) >> 8) + 16);
			y0[x + 1] = (uint8_t)(((66 * rb + 129 * gb + 25 * bb + 128) >> 8) + 16);
			y1[x] = (uint8_t)(((66 * rc + 129 * gc + 25 * bc + 128) >> 8) + 16);
			y1[x + 1] = (uint8_t)(((66 * rd + 129 * gd + 25 * bd + 128) >> 8) + 16);
			u[x / 2] = (uint8_t)(((-38 * rs - 74 * gs + 112 * bs + 512) >> 10) + 128);
			v[x / 2] = (uint8_t)(((112 * rs - 94 * gs - 18 * bs + 512) >> 10) + 128);
		}
	}
}

int converter_run(struct converter *cv, const struct capture_image *img, struct picture *dst,
		  bool letterbox)
{
	enum AVPixelFormat fmt = av_format(img->format);
	const uint8_t *src_data[4] = { 0 };
	int src_stride[4] = { 0 };
	uint8_t *dst_data[4] = { 0 };
	int lw, lh, rx = 0, ry = 0, rw = dst->width, rh = dst->height;

	if (fmt == AV_PIX_FMT_NONE)
		return -1;
	capture_logical_size(img, &lw, &lh);
	if (letterbox) {
		fit_size(lw, lh, dst->width, dst->height, &rw, &rh);
		rx = ((dst->width - rw) / 2) & ~1;
		ry = ((dst->height - rh) / 2) & ~1;
	}
	if (rx != cv->last_x || ry != cv->last_y || rw != cv->last_w || rh != cv->last_h ||
	    dst->width != cv->last_dst_w || dst->height != cv->last_dst_h) {
		/* New geometry: clear the borders once. */
		picture_black(dst);
		cv->last_x = rx;
		cv->last_y = ry;
		cv->last_w = rw;
		cv->last_h = rh;
		cv->last_dst_w = dst->width;
		cv->last_dst_h = dst->height;
	}

	if (img->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		size_t need = (size_t)lw * (size_t)lh * 4;

		if (need > cv->rotated_size) {
			free(cv->rotated);
			cv->rotated = malloc(need);
			cv->rotated_size = cv->rotated ? need : 0;
			if (!cv->rotated)
				return -1;
		}
		untransform(img, cv->rotated, lw, lh);
		src_data[0] = (const uint8_t *)cv->rotated;
		src_stride[0] = lw * 4;
	} else if (img->y_invert) {
		src_data[0] = img->data + (size_t)(img->height - 1) * img->stride;
		src_stride[0] = -img->stride;
	} else {
		src_data[0] = img->data;
		src_stride[0] = img->stride;
	}

	if (rw == lw && rh == lh && !(lw & 1) && !(lh & 1)) {
		rgb32_to_i420(src_data[0], src_stride[0], fmt == AV_PIX_FMT_BGR0 || fmt == AV_PIX_FMT_BGRA, lw, lh,
			      dst, rx, ry);
		dst->time_us = img->time_us;
		return 0;
	}
	cv->sws = sws_getCachedContext(cv->sws, lw, lh, fmt, rw, rh, AV_PIX_FMT_YUV420P,
				       SWS_BILINEAR, NULL, NULL, NULL);
	if (!cv->sws)
		return -1;
	dst_data[0] = dst->plane[0] + (size_t)ry * dst->stride[0] + rx;
	dst_data[1] = dst->plane[1] + (size_t)(ry / 2) * dst->stride[1] + rx / 2;
	dst_data[2] = dst->plane[2] + (size_t)(ry / 2) * dst->stride[2] + rx / 2;
	sws_scale(cv->sws, src_data, src_stride, 0, lh, dst_data, dst->stride);
	dst->time_us = img->time_us;
	return 0;
}
