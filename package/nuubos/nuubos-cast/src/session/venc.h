/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Video encoders, all software (Cedrus is decode-only):
 *   VP8   libvpx (BSD, royalty-free): Google Cast.
 *   MPEG-2 FFmpeg mpeg2video (patents expired): DLNA renderers.
 *   H.264 Cisco's OpenH264 binary, downloaded by the user (Cisco pays the
 *         AVC royalties for its binary; nuubOS never ships it): Miracast.
 */
#ifndef NUUBOS_CAST_VENC_H
#define NUUBOS_CAST_VENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "picture.h"

enum venc_codec {
	VENC_VP8,
	VENC_MPEG2,
	VENC_H264,
};

struct venc_packet {
	const uint8_t *data;
	size_t len;
	bool key;
	int64_t time_us; /* capture time of the picture */
};

struct venc;

/* `openh264` is the path of Cisco's library (VENC_H264 only). */
struct venc *venc_open(enum venc_codec codec, int width, int height, int fps, int kbps,
		       const char *openh264);
void venc_close(struct venc *v);
/* 0 = a packet in `out` (valid until the next call), 1 = none, -1 error. */
int venc_encode(struct venc *v, const struct picture *pic, bool force_key, struct venc_packet *out);
void venc_set_bitrate(struct venc *v, int kbps);
int venc_bitrate(const struct venc *v);

#endif
