/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Live container writer (libavformat): MPEG-TS (DLNA, Miracast) or WebM
 * (Google Cast Default Media Receiver fallback). Output goes to a callback
 * as it is produced.
 */
#ifndef NUUBOS_CAST_MUX_H
#define NUUBOS_CAST_MUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aenc.h"
#include "venc.h"

enum mux_format {
	MUX_MPEGTS,
	MUX_WEBM,
};

typedef void (*mux_write_fn)(void *user, const uint8_t *data, size_t len);

struct mux;

/* `audio` may be NULL (no audio stream). */
struct mux *mux_open(enum mux_format format, enum venc_codec vcodec, int width, int height, int fps,
		     const struct aenc *audio, enum aenc_codec acodec, mux_write_fn write, void *user);
void mux_close(struct mux *m);
int mux_video(struct mux *m, const struct venc_packet *pkt);
int mux_audio(struct mux *m, const struct aenc_packet *pkt);
/* Pushes buffered bytes out (TS packets are written in 188-byte units). */
void mux_flush(struct mux *m);

#endif
