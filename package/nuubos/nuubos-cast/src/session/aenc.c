/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aenc.h"
#include "audio.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>

/* Cast senders use 10 ms Opus frames (openscreen kDefaultCastAudioFramesPerSecond). */
#define OPUS_FRAME 480
#define MAX_FRAME 1152

struct aenc {
	enum aenc_codec codec;
	int frame_samples;

	/* input FIFO */
	int16_t *fifo;
	size_t fifo_frames;
	size_t fifo_cap;
	int64_t fifo_time_us; /* time of fifo[0] */

	OpusEncoder *opus;
	uint8_t opus_out[4000];
	uint8_t opus_head[19];

	AVCodecContext *av;
	AVFrame *frame;
	AVPacket *pkt;
};

struct aenc *aenc_open(enum aenc_codec codec, int kbps)
{
	struct aenc *a = calloc(1, sizeof(*a));
	int err;

	if (!a)
		return NULL;
	a->codec = codec;
	a->fifo_cap = AUDIO_RATE / 2;
	a->fifo = malloc(a->fifo_cap * AUDIO_CHANNELS * sizeof(int16_t));
	if (!a->fifo)
		goto fail;

	if (codec == AENC_OPUS) {
		a->frame_samples = OPUS_FRAME;
		a->opus = opus_encoder_create(AUDIO_RATE, AUDIO_CHANNELS, OPUS_APPLICATION_AUDIO, &err);
		if (!a->opus || err != OPUS_OK)
			goto fail;
		opus_encoder_ctl(a->opus, OPUS_SET_BITRATE(kbps * 1000));
		opus_encoder_ctl(a->opus, OPUS_SET_COMPLEXITY(5));
		/* OpusHead (RFC 7845) for containers. */
		memcpy(a->opus_head, "OpusHead", 8);
		a->opus_head[8] = 1;
		a->opus_head[9] = AUDIO_CHANNELS;
		a->opus_head[10] = 312 & 0xff; /* pre-skip, little endian */
		a->opus_head[11] = 312 >> 8;
		a->opus_head[12] = AUDIO_RATE & 0xff;
		a->opus_head[13] = (AUDIO_RATE >> 8) & 0xff;
		a->opus_head[14] = (AUDIO_RATE >> 16) & 0xff;
		a->opus_head[15] = 0;
		a->opus_head[16] = 0;
		a->opus_head[17] = 0;
		a->opus_head[18] = 0;
		return a;
	} else {
		const AVCodec *c = avcodec_find_encoder(codec == AENC_AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_MP2);

		if (!c) {
			log_msg("FFmpeg has no %s encoder", codec == AENC_AAC ? "AAC" : "MP2");
			goto fail;
		}
		a->av = avcodec_alloc_context3(c);
		a->frame = av_frame_alloc();
		a->pkt = av_packet_alloc();
		if (!a->av || !a->frame || !a->pkt)
			goto fail;
		a->av->sample_rate = AUDIO_RATE;
		av_channel_layout_default(&a->av->ch_layout, AUDIO_CHANNELS);
		a->av->sample_fmt = codec == AENC_AAC ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16;
		a->av->bit_rate = (int64_t)kbps * 1000;
		a->av->time_base = (AVRational){ 1, AUDIO_RATE };
		/* AudioSpecificConfig in extradata: the TS muxer writes ADTS. */
		a->av->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		if (avcodec_open2(a->av, c, NULL) < 0)
			goto fail;
		a->frame_samples = a->av->frame_size;
		if (a->frame_samples <= 0 || a->frame_samples > MAX_FRAME)
			goto fail;
		a->frame->format = a->av->sample_fmt;
		a->frame->nb_samples = a->frame_samples;
		av_channel_layout_copy(&a->frame->ch_layout, &a->av->ch_layout);
		a->frame->sample_rate = AUDIO_RATE;
		if (av_frame_get_buffer(a->frame, 0) < 0)
			goto fail;
		return a;
	}
fail:
	log_msg("audio encoder init failed");
	aenc_close(a);
	return NULL;
}

void aenc_close(struct aenc *a)
{
	if (!a)
		return;
	if (a->opus)
		opus_encoder_destroy(a->opus);
	avcodec_free_context(&a->av);
	av_frame_free(&a->frame);
	av_packet_free(&a->pkt);
	free(a->fifo);
	free(a);
}

int aenc_push(struct aenc *a, const int16_t *pcm, size_t frames, int64_t time_us)
{
	if (a->fifo_frames == 0)
		a->fifo_time_us = time_us;
	if (a->fifo_frames + frames > a->fifo_cap) {
		/* Never expected (pulled every 10 ms): drop the backlog. */
		a->fifo_frames = 0;
		a->fifo_time_us = time_us;
		if (frames > a->fifo_cap)
			return -1;
	}
	memcpy(a->fifo + a->fifo_frames * AUDIO_CHANNELS, pcm, frames * AUDIO_CHANNELS * sizeof(int16_t));
	a->fifo_frames += frames;
	return 0;
}

static void fifo_consume(struct aenc *a, size_t frames)
{
	memmove(a->fifo, a->fifo + frames * AUDIO_CHANNELS,
		(a->fifo_frames - frames) * AUDIO_CHANNELS * sizeof(int16_t));
	a->fifo_frames -= frames;
	a->fifo_time_us += (int64_t)frames * 1000000 / AUDIO_RATE;
}

int aenc_pull(struct aenc *a, struct aenc_packet *out)
{
	int64_t t = a->fifo_time_us;
	int ret;

	if (a->fifo_frames < (size_t)a->frame_samples)
		return 1;
	if (a->opus) {
		int n = opus_encode(a->opus, a->fifo, a->frame_samples, a->opus_out, sizeof(a->opus_out));

		fifo_consume(a, (size_t)a->frame_samples);
		if (n < 0)
			return -1;
		out->data = a->opus_out;
		out->len = (size_t)n;
		out->time_us = t;
		out->samples = a->frame_samples;
		return 0;
	}

	if (av_frame_make_writable(a->frame) < 0)
		return -1;
	if (a->av->sample_fmt == AV_SAMPLE_FMT_FLTP) {
		float *l = (float *)a->frame->data[0], *r = (float *)a->frame->data[1];

		for (int i = 0; i < a->frame_samples; i++) {
			l[i] = a->fifo[2 * i] / 32768.0f;
			r[i] = a->fifo[2 * i + 1] / 32768.0f;
		}
	} else {
		memcpy(a->frame->data[0], a->fifo, (size_t)a->frame_samples * AUDIO_CHANNELS * sizeof(int16_t));
	}
	a->frame->pts = av_rescale_q(t, (AVRational){ 1, 1000000 }, a->av->time_base);
	fifo_consume(a, (size_t)a->frame_samples);
	if (avcodec_send_frame(a->av, a->frame) < 0)
		return -1;
	av_packet_unref(a->pkt);
	ret = avcodec_receive_packet(a->av, a->pkt);
	if (ret == AVERROR(EAGAIN))
		return 1;
	if (ret < 0)
		return -1;
	out->data = a->pkt->data;
	out->len = (size_t)a->pkt->size;
	/* The encoder's delay shifts packets: their pts is authoritative. */
	out->time_us = av_rescale_q(a->pkt->pts, a->av->time_base, (AVRational){ 1, 1000000 });
	out->samples = a->frame_samples;
	return 0;
}

const uint8_t *aenc_extradata(const struct aenc *a, size_t *len)
{
	if (a->opus) {
		*len = sizeof(a->opus_head);
		return a->opus_head;
	}
	*len = a->av && a->av->extradata ? (size_t)a->av->extradata_size : 0;
	return *len ? a->av->extradata : NULL;
}

int aenc_frame_samples(const struct aenc *a)
{
	return a->frame_samples;
}
