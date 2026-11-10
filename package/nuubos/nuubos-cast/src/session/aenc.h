/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Audio encoders: Opus (libopus, Google Cast), AAC-LC (FFmpeg native
 * encoder, Miracast), MPEG-1 Layer II (FFmpeg, patents expired, DLNA).
 * Input: 48 kHz stereo S16 in any amount; output: one codec frame at a
 * time.
 */
#ifndef NUUBOS_CAST_AENC_H
#define NUUBOS_CAST_AENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum aenc_codec {
	AENC_OPUS,
	AENC_AAC,
	AENC_MP2,
};

struct aenc_packet {
	const uint8_t *data;
	size_t len;
	int64_t time_us;  /* capture time of the first sample */
	int samples;      /* per channel */
};

struct aenc;

struct aenc *aenc_open(enum aenc_codec codec, int kbps);
void aenc_close(struct aenc *a);
/* Queues samples captured at `time_us` (first frame). */
int aenc_push(struct aenc *a, const int16_t *pcm, size_t frames, int64_t time_us);
/* 0 = a packet (valid until the next call), 1 = needs more input. */
int aenc_pull(struct aenc *a, struct aenc_packet *out);
/* Codec configuration for containers (AAC AudioSpecificConfig, OpusHead). */
const uint8_t *aenc_extradata(const struct aenc *a, size_t *len);
int aenc_frame_samples(const struct aenc *a);

#endif
