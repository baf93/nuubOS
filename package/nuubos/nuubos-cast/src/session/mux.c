/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "audio.h"
#include "common.h"

#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

/* 7 TS packets: one RTP payload (Miracast), a good HTTP chunk. */
#define TS_IO_SIZE (188 * 7)
#define WEBM_IO_SIZE 4096
/* Timestamps start here: audio encoders deliver their first packet a few
 * ms before the first capture time (priming). */
#define BASE_OFFSET_US 1000000

struct mux {
	AVFormatContext *fmt;
	AVIOContext *io;
	AVStream *video;
	AVStream *audio;
	AVPacket *pkt;
	mux_write_fn write;
	void *user;
	int64_t base_us;
	bool have_base;
	int64_t last_video_dts;
	int64_t last_audio_dts;
	bool header_written;
};

static int io_write(void *opaque, const uint8_t *buf, int size)
{
	struct mux *m = opaque;

	if (size > 0)
		m->write(m->user, buf, (size_t)size);
	return size;
}

struct mux *mux_open(enum mux_format format, enum venc_codec vcodec, int width, int height, int fps,
		     const struct aenc *audio, enum aenc_codec acodec, mux_write_fn write, void *user)
{
	struct mux *m = calloc(1, sizeof(*m));
	int io_size = format == MUX_MPEGTS ? TS_IO_SIZE : WEBM_IO_SIZE;
	uint8_t *io_buf;
	AVDictionary *opts = NULL;

	if (!m)
		return NULL;
	m->write = write;
	m->user = user;
	m->last_video_dts = INT64_MIN;
	m->last_audio_dts = INT64_MIN;
	if (avformat_alloc_output_context2(&m->fmt, NULL, format == MUX_MPEGTS ? "mpegts" : "webm", NULL) < 0 ||
	    !m->fmt) {
		log_msg("libavformat lacks the %s muxer", format == MUX_MPEGTS ? "mpegts" : "webm");
		goto fail;
	}
	io_buf = av_malloc((size_t)io_size);
	m->io = io_buf ? avio_alloc_context(io_buf, io_size, 1, m, NULL, io_write, NULL) : NULL;
	if (!m->io)
		goto fail;
	m->fmt->pb = m->io;
	m->fmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
	m->fmt->max_delay = 0;

	m->video = avformat_new_stream(m->fmt, NULL);
	if (!m->video)
		goto fail;
	m->video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
	m->video->codecpar->codec_id = vcodec == VENC_VP8 ? AV_CODEC_ID_VP8 :
				       vcodec == VENC_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG2VIDEO;
	m->video->codecpar->width = width;
	m->video->codecpar->height = height;
	m->video->time_base = (AVRational){ 1, 90000 };
	m->video->avg_frame_rate = (AVRational){ fps, 1 };

	if (audio) {
		size_t extra_len = 0;
		const uint8_t *extra = aenc_extradata(audio, &extra_len);

		m->audio = avformat_new_stream(m->fmt, NULL);
		if (!m->audio)
			goto fail;
		m->audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
		m->audio->codecpar->codec_id = acodec == AENC_OPUS ? AV_CODEC_ID_OPUS :
					       acodec == AENC_AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_MP2;
		m->audio->codecpar->sample_rate = AUDIO_RATE;
		av_channel_layout_default(&m->audio->codecpar->ch_layout, AUDIO_CHANNELS);
		m->audio->codecpar->frame_size = aenc_frame_samples(audio);
		if (acodec == AENC_OPUS)
			m->audio->codecpar->initial_padding = 312;
		if (extra && extra_len) {
			m->audio->codecpar->extradata = av_mallocz(extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
			if (!m->audio->codecpar->extradata)
				goto fail;
			memcpy(m->audio->codecpar->extradata, extra, extra_len);
			m->audio->codecpar->extradata_size = (int)extra_len;
		}
		m->audio->time_base = (AVRational){ 1, AUDIO_RATE };
	}

	if (format == MUX_MPEGTS) {
		/* PAT/PMT with every key frame too: a TV that (re)joins starts
		 * decoding at once. */
		av_dict_set(&opts, "mpegts_flags", "+resend_headers", 0);
		av_dict_set(&opts, "pcr_period", "40", 0);
	} else {
		av_dict_set(&opts, "live", "1", 0);
		av_dict_set(&opts, "cluster_time_limit", "1000", 0);
		av_dict_set(&opts, "cluster_size_limit", "2000000", 0);
	}
	if (avformat_write_header(m->fmt, &opts) < 0) {
		av_dict_free(&opts);
		log_msg("container header failed");
		goto fail;
	}
	av_dict_free(&opts);
	m->header_written = true;
	m->pkt = av_packet_alloc();
	if (!m->pkt)
		goto fail;
	avio_flush(m->io);
	return m;
fail:
	mux_close(m);
	return NULL;
}

void mux_close(struct mux *m)
{
	if (!m)
		return;
	if (m->header_written)
		av_write_trailer(m->fmt);
	if (m->io) {
		av_freep(&m->io->buffer);
		avio_context_free(&m->io);
	}
	if (m->fmt)
		avformat_free_context(m->fmt);
	av_packet_free(&m->pkt);
	free(m);
}

static int64_t stream_ts(struct mux *m, AVStream *st, int64_t time_us)
{
	if (!m->have_base) {
		m->base_us = time_us;
		m->have_base = true;
	}
	return av_rescale_q(time_us - m->base_us + BASE_OFFSET_US, (AVRational){ 1, 1000000 }, st->time_base);
}

int mux_video(struct mux *m, const struct venc_packet *pkt)
{
	int64_t ts = stream_ts(m, m->video, pkt->time_us);

	if (ts <= m->last_video_dts)
		ts = m->last_video_dts + 1;
	m->last_video_dts = ts;
	av_packet_unref(m->pkt);
	m->pkt->data = (uint8_t *)pkt->data;
	m->pkt->size = (int)pkt->len;
	m->pkt->stream_index = m->video->index;
	m->pkt->pts = m->pkt->dts = ts;
	m->pkt->flags = pkt->key ? AV_PKT_FLAG_KEY : 0;
	if (av_write_frame(m->fmt, m->pkt) < 0)
		return -1;
	m->pkt->data = NULL;
	m->pkt->size = 0;
	return 0;
}

int mux_audio(struct mux *m, const struct aenc_packet *pkt)
{
	int64_t ts;

	if (!m->audio)
		return 0;
	ts = stream_ts(m, m->audio, pkt->time_us);
	if (ts <= m->last_audio_dts)
		ts = m->last_audio_dts + 1;
	m->last_audio_dts = ts;
	av_packet_unref(m->pkt);
	m->pkt->data = (uint8_t *)pkt->data;
	m->pkt->size = (int)pkt->len;
	m->pkt->stream_index = m->audio->index;
	m->pkt->pts = m->pkt->dts = ts;
	m->pkt->duration = av_rescale_q(pkt->samples, (AVRational){ 1, AUDIO_RATE }, m->audio->time_base);
	m->pkt->flags = AV_PKT_FLAG_KEY;
	if (av_write_frame(m->fmt, m->pkt) < 0)
		return -1;
	m->pkt->data = NULL;
	m->pkt->size = 0;
	return 0;
}

void mux_flush(struct mux *m)
{
	avio_flush(m->io);
}
