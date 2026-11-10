/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Cast Streaming sender for one stream (audio or video): Cast RTP
 * packetization, AES-128-CTR frame encryption, RTCP sender reports,
 * receiver feedback (ACK/NACK/picture loss), retransmission and kickstart.
 * Wire formats and timing rules follow the libcast (openscreen) reference
 * implementation, cast/streaming/impl (BSD licence).
 */
#ifndef NUUBOS_CAST_CASTSTREAM_H
#define NUUBOS_CAST_CASTSTREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cast_stream_config {
	bool video;
	uint32_t sender_ssrc;
	uint32_t receiver_ssrc;
	int payload_type;
	int rtp_timebase;
	int target_delay_ms;
	uint8_t aes_key[16];
	uint8_t aes_iv_mask[16];
};

struct cast_stream;

/* `udp_fd`: connected UDP socket to the receiver (shared by both streams). */
struct cast_stream *cast_stream_new(const struct cast_stream_config *cfg, int udp_fd,
				    int64_t ntp_offset_us);
void cast_stream_free(struct cast_stream *s);
uint32_t cast_stream_receiver_ssrc(const struct cast_stream *s);
/* Whether a frame with this RTP timestamp may be sent now (frames in
 * flight within the latency budget). Check before encoding, so a skipped
 * picture never breaks the encoder's reference chain. */
bool cast_stream_can_enqueue(const struct cast_stream *s, uint32_t rtp_ts);
int cast_stream_enqueue(struct cast_stream *s, const uint8_t *data, size_t len, bool key,
			uint32_t rtp_ts, int64_t reference_us);
/* An RTCP packet whose first SSRC is this stream's receiver. */
void cast_stream_rtcp(struct cast_stream *s, const uint8_t *data, size_t len, int64_t arrival_us);
/* Next deadline (sender report, kickstart), CLOCK_MONOTONIC µs, or -1. */
int64_t cast_stream_next_deadline(const struct cast_stream *s);
void cast_stream_run(struct cast_stream *s, int64_t now);
bool cast_stream_take_picture_loss(struct cast_stream *s);
int64_t cast_stream_last_feedback_us(const struct cast_stream *s);
int64_t cast_stream_rtt_us(const struct cast_stream *s);
/* Counters since the last call (adaptive bit rate). */
void cast_stream_take_counters(struct cast_stream *s, int *retransmits, int *skips);
void cast_stream_note_skip(struct cast_stream *s);

#endif
