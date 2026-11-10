/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "caststream.h"
#include "common.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <openssl/evp.h>

/* openscreen cast/streaming: constants.h, rtp_defines.h, sender_impl.cc */
#define MAX_UNACKED_FRAMES 120
#define SLOTS 128
#define MAX_RTP_PACKET (1500 - 20 - 8)
#define BASE_HEADER 19          /* RTP 12 + Cast 6 + reference frame id */
#define LATENCY_EXT 4
#define MAX_PAYLOAD (MAX_RTP_PACKET - BASE_HEADER - LATENCY_EXT)
#define REPORT_INTERVAL_US 500000
#define MIN_IN_FLIGHT_US 66000
#define MAX_PACKETS 0xfffe
#define ALL_PACKETS_LOST 0xffff

#define RTCP_SR 200
#define RTCP_RR 201
#define RTCP_APP 204
#define RTCP_PSFB 206
#define RTCP_XR 207
#define PSFB_PLI 1
#define PSFB_CAST 15
#define CAST_WORD 0x43415354u /* 'CAST' */
#define CST2_WORD 0x43535432u /* 'CST2' */
#define SR_HISTORY 8

struct slot {
	bool active;
	uint32_t frame_id;
	uint32_t ref_id;
	bool key;
	uint32_t rtp_ts;
	int64_t reference_us;
	uint8_t *data;      /* encrypted */
	size_t len;
	int packets;
	int64_t *sent_us;   /* per packet, 0 = never */
	uint8_t *need;      /* per packet: must be (re)sent */
};

struct cast_stream {
	struct cast_stream_config cfg;
	int fd;
	int64_t ntp_offset_us; /* CLOCK_REALTIME - CLOCK_MONOTONIC */
	EVP_CIPHER_CTX *aes;

	struct slot slots[SLOTS];
	bool any_enqueued;
	uint32_t last_enqueued;   /* frame id */
	uint32_t checkpoint;      /* every frame <= this is acknowledged */
	bool have_checkpoint;     /* false: checkpoint = "before frame 0" */
	uint32_t latest_expected;
	bool have_expected;
	uint16_t seq;

	/* sender report state */
	uint32_t report_rtp_ts;
	int64_t report_reference_us;
	uint32_t packet_count;
	uint32_t octet_count;
	int64_t next_report_us;
	struct { uint32_t id; int64_t sent_us; } sr[SR_HISTORY];
	int sr_next;

	int64_t rtt_us;
	int64_t last_feedback_us;
	bool picture_loss;
	uint32_t picture_lost_at;
	uint32_t last_key_frame;
	bool have_key_frame;
	int retransmits;
	int skips;
};

/* Frame ids are 32-bit counters; the wire carries the low 8 bits. The
 * "before frame 0" checkpoint is represented as UINT32_MAX (== -1). */
static uint32_t checkpoint_id(const struct cast_stream *s)
{
	return s->have_checkpoint ? s->checkpoint : UINT32_MAX;
}

static bool id_gt(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

static struct slot *slot_for(struct cast_stream *s, uint32_t frame_id)
{
	return &s->slots[frame_id % SLOTS];
}

static bool slot_active_for(const struct slot *sl, uint32_t frame_id)
{
	return sl->active && sl->frame_id == frame_id;
}

static void slot_release(struct slot *sl)
{
	free(sl->data);
	free(sl->sent_us);
	free(sl->need);
	memset(sl, 0, sizeof(*sl));
}

static uint64_t to_ntp(const struct cast_stream *s, int64_t mono_us)
{
	int64_t unix_us = mono_us + s->ntp_offset_us;
	uint64_t sec = (uint64_t)(unix_us / 1000000) + 2208988800ULL;
	uint64_t frac = ((uint64_t)(unix_us % 1000000) << 32) / 1000000;

	return sec << 32 | frac;
}

struct cast_stream *cast_stream_new(const struct cast_stream_config *cfg, int udp_fd,
				    int64_t ntp_offset_us)
{
	struct cast_stream *s = calloc(1, sizeof(*s));
	uint16_t seq;

	if (!s)
		return NULL;
	s->cfg = *cfg;
	s->fd = udp_fd;
	s->ntp_offset_us = ntp_offset_us;
	random_bytes(&seq, sizeof(seq));
	s->seq = seq;
	s->aes = EVP_CIPHER_CTX_new();
	if (!s->aes) {
		free(s);
		return NULL;
	}
	return s;
}

void cast_stream_free(struct cast_stream *s)
{
	if (!s)
		return;
	for (int i = 0; i < SLOTS; i++)
		slot_release(&s->slots[i]);
	EVP_CIPHER_CTX_free(s->aes);
	free(s);
}

uint32_t cast_stream_receiver_ssrc(const struct cast_stream *s)
{
	return s->cfg.receiver_ssrc;
}

static int64_t max_in_flight_us(const struct cast_stream *s)
{
	int64_t upper = (int64_t)s->cfg.target_delay_ms * 1000 / 3;
	int64_t v = s->rtt_us * 2;

	if (upper < MIN_IN_FLIGHT_US)
		upper = MIN_IN_FLIGHT_US;
	if (v < MIN_IN_FLIGHT_US)
		v = MIN_IN_FLIGHT_US;
	return v > upper ? upper : v;
}

bool cast_stream_can_enqueue(const struct cast_stream *s, uint32_t rtp_ts)
{
	uint32_t next = s->any_enqueued ? s->last_enqueued + 1 : 0;
	uint32_t oldest = checkpoint_id(s) + 1;
	const struct slot *sl;
	int64_t in_flight;

	if (next - oldest + 1 > MAX_UNACKED_FRAMES)
		return false;
	if (!s->any_enqueued || oldest == next)
		return true; /* nothing in flight */
	sl = &s->slots[oldest % SLOTS];
	if (!slot_active_for(sl, oldest))
		return true;
	in_flight = (int64_t)(int32_t)(rtp_ts - sl->rtp_ts) * 1000000 / s->cfg.rtp_timebase;
	return in_flight <= max_in_flight_us(s);
}

static int encrypt_frame(struct cast_stream *s, uint32_t frame_id, const uint8_t *in, size_t len,
			 uint8_t *out)
{
	uint8_t nonce[16] = { 0 };
	int n1 = 0, n2 = 0;

	put_be32(nonce + 8, frame_id);
	for (int i = 0; i < 16; i++)
		nonce[i] ^= s->cfg.aes_iv_mask[i];
	if (EVP_EncryptInit_ex(s->aes, EVP_aes_128_ctr(), NULL, s->cfg.aes_key, nonce) != 1)
		return -1;
	if (len && EVP_EncryptUpdate(s->aes, out, &n1, in, (int)len) != 1)
		return -1;
	if (EVP_EncryptFinal_ex(s->aes, out + n1, &n2) != 1)
		return -1;
	return 0;
}

static void send_packet(struct cast_stream *s, struct slot *sl, int pid, int64_t now)
{
	uint8_t pkt[MAX_RTP_PACKET];
	bool last = pid == sl->packets - 1;
	bool ext = pid == 0 && false; /* adaptive latency changes are not used */
	size_t start = (size_t)pid * MAX_PAYLOAD;
	size_t chunk = last ? sl->len - start : MAX_PAYLOAD;
	size_t n = 0;

	pkt[n++] = 0x80;
	pkt[n++] = (uint8_t)((last ? 0x80 : 0) | (s->cfg.payload_type & 0x7f));
	put_be16(pkt + n, s->seq++);
	n += 2;
	put_be32(pkt + n, sl->rtp_ts);
	n += 4;
	put_be32(pkt + n, s->cfg.sender_ssrc);
	n += 4;
	pkt[n++] = (uint8_t)((sl->key ? 0x80 : 0) | 0x40 | (ext ? 1 : 0));
	pkt[n++] = (uint8_t)sl->frame_id;
	put_be16(pkt + n, (uint16_t)pid);
	n += 2;
	put_be16(pkt + n, (uint16_t)(sl->packets - 1));
	n += 2;
	pkt[n++] = (uint8_t)sl->ref_id;
	if (chunk)
		memcpy(pkt + n, sl->data + start, chunk);
	n += chunk;
	if (send(s->fd, pkt, n, MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != ECONNREFUSED)
		log_msg("rtp send: %s", strerror(errno));
	sl->sent_us[pid] = now;
	sl->need[pid] = 0;
	s->packet_count++;
	s->octet_count += (uint32_t)(n - BASE_HEADER);
}

static void send_report(struct cast_stream *s, int64_t now)
{
	uint8_t pkt[28];
	uint64_t ntp = to_ntp(s, now);
	uint32_t rtp = s->report_rtp_ts +
		       (uint32_t)((now - s->report_reference_us) * s->cfg.rtp_timebase / 1000000);

	pkt[0] = 0x80;
	pkt[1] = RTCP_SR;
	put_be16(pkt + 2, 6);
	put_be32(pkt + 4, s->cfg.sender_ssrc);
	put_be64(pkt + 8, ntp);
	put_be32(pkt + 16, rtp);
	put_be32(pkt + 20, s->packet_count);
	put_be32(pkt + 24, s->octet_count);
	if (send(s->fd, pkt, sizeof(pkt), MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != ECONNREFUSED)
		log_msg("rtcp send: %s", strerror(errno));
	s->sr[s->sr_next].id = (uint32_t)(ntp >> 16);
	s->sr[s->sr_next].sent_us = now;
	s->sr_next = (s->sr_next + 1) % SR_HISTORY;
	s->next_report_us = now + REPORT_INTERVAL_US;
}

/* Sends every packet marked as needed, oldest frame first. */
static void send_needed(struct cast_stream *s, int64_t now)
{
	if (!s->any_enqueued)
		return;
	for (uint32_t id = checkpoint_id(s) + 1; !id_gt(id, s->last_enqueued); id++) {
		struct slot *sl = slot_for(s, id);

		if (!slot_active_for(sl, id))
			continue;
		for (int p = 0; p < sl->packets; p++)
			if (sl->need[p])
				send_packet(s, sl, p, now);
	}
}

int cast_stream_enqueue(struct cast_stream *s, const uint8_t *data, size_t len, bool key,
			uint32_t rtp_ts, int64_t reference_us)
{
	uint32_t id = s->any_enqueued ? s->last_enqueued + 1 : 0;
	struct slot *sl = slot_for(s, id);
	int packets = len ? (int)((len + MAX_PAYLOAD - 1) / MAX_PAYLOAD) : 1;
	int64_t now = now_us();

	if (packets > MAX_PACKETS)
		return -1;
	if (sl->active)
		slot_release(sl); /* only reachable for a long-cancelled frame */
	sl->data = malloc(len ? len : 1);
	sl->sent_us = calloc((size_t)packets, sizeof(int64_t));
	sl->need = malloc((size_t)packets);
	if (!sl->data || !sl->sent_us || !sl->need || encrypt_frame(s, id, data, len, sl->data) != 0) {
		slot_release(sl);
		return -1;
	}
	memset(sl->need, 1, (size_t)packets);
	sl->active = true;
	sl->frame_id = id;
	/* Video: delta frames reference the previous frame (castv2 mode,
	 * no frame is ever dropped by the receiver); key frames themselves.
	 * Audio frames are all independent. */
	sl->key = key || !s->cfg.video;
	sl->ref_id = sl->key ? id : id - 1;
	sl->rtp_ts = rtp_ts;
	sl->reference_us = reference_us;
	sl->len = len;
	sl->packets = packets;
	s->last_enqueued = id;
	s->any_enqueued = true;
	if (s->cfg.video && key) {
		s->last_key_frame = id;
		s->have_key_frame = true;
	}
	if (id == 0 || reference_us > s->report_reference_us)
		s->report_reference_us = reference_us;
	s->report_rtp_ts = rtp_ts;
	/* Until a receiver report measured the round trip, the receiver has
	 * not seen a sender report: it needs one for lip sync, now. */
	if (s->rtt_us == 0)
		send_report(s, now);
	send_needed(s, now);
	return 0;
}

static void ack_through(struct cast_stream *s, uint32_t frame_id)
{
	while (!s->have_checkpoint || id_gt(frame_id, s->checkpoint)) {
		uint32_t next = checkpoint_id(s) + 1;
		struct slot *sl = slot_for(s, next);

		if (slot_active_for(sl, next))
			slot_release(sl);
		s->checkpoint = next;
		s->have_checkpoint = true;
	}
	if (!s->have_expected || id_gt(frame_id, s->latest_expected)) {
		s->latest_expected = frame_id;
		s->have_expected = true;
	}
}

static void note_expected(struct cast_stream *s, uint32_t frame_id)
{
	if (!s->have_expected || id_gt(frame_id, s->latest_expected)) {
		s->latest_expected = frame_id;
		s->have_expected = true;
	}
}

/* Largest id <= max whose low byte is `low`. */
static uint32_t expand_le(uint32_t max, uint8_t low)
{
	uint32_t v = (max & ~0xffu) | low;

	return id_gt(v, max) ? v - 256 : v;
}

/* Smallest id > min whose low byte is `low`. */
static uint32_t expand_gt(uint32_t min, uint8_t low)
{
	uint32_t v = (min & ~0xffu) | low;

	return id_gt(v, min) ? v : v + 256;
}

static void handle_nack(struct cast_stream *s, uint32_t frame_id, uint16_t pid, int64_t arrival)
{
	struct slot *sl;
	int64_t too_recent = arrival - s->rtt_us;

	if (!s->any_enqueued || id_gt(frame_id, s->last_enqueued))
		return;
	sl = slot_for(s, frame_id);
	if (!slot_active_for(sl, frame_id))
		return;
	note_expected(s, frame_id);
	if (pid == ALL_PACKETS_LOST) {
		for (int p = 0; p < sl->packets; p++)
			if (sl->sent_us[p] <= too_recent) {
				sl->need[p] = 1;
				s->retransmits++;
			}
	} else if (pid < sl->packets && sl->sent_us[pid] <= too_recent) {
		sl->need[pid] = 1;
		s->retransmits++;
	}
}

static void parse_feedback(struct cast_stream *s, const uint8_t *p, size_t len, int64_t arrival)
{
	uint32_t checkpoint;
	int losses;

	if (len < 16 || get_be32(p) != s->cfg.receiver_ssrc || get_be32(p + 4) != s->cfg.sender_ssrc ||
	    get_be32(p + 8) != CAST_WORD || !s->any_enqueued)
		return;
	checkpoint = expand_le(s->last_enqueued, p[12]);
	losses = p[13];
	/* Feedback for frames this sender never sent is stale/out of order. */
	if (s->have_checkpoint && id_gt(s->checkpoint, checkpoint))
		return;
	if (checkpoint != UINT32_MAX)
		ack_through(s, checkpoint);
	p += 16;
	len -= 16;
	if (len < (size_t)losses * 4)
		return;
	for (int i = 0; i < losses; i++, p += 4, len -= 4) {
		uint32_t frame_id = expand_gt(checkpoint, p[0]);
		uint16_t pid = get_be16(p + 1);
		uint8_t bits = p[3];

		handle_nack(s, frame_id, pid, arrival);
		if (pid != ALL_PACKETS_LOST)
			for (int b = 1; bits; b++, bits >>= 1)
				if (bits & 1)
					handle_nack(s, frame_id, (uint16_t)(pid + b), arrival);
	}
	/* Optional frame-level ACK bit vector ("CST2"). */
	if (len >= 6 && get_be32(p) == CST2_WORD) {
		int octets = p[5];
		uint32_t start = checkpoint + 2;

		p += 6;
		len -= 6;
		for (int i = 0; i < octets && (size_t)i < len; i++, start += 8) {
			for (int b = 0; b < 8; b++) {
				uint32_t id = start + (uint32_t)b;
				struct slot *sl;

				if (!(p[i] & (1u << b)) || id_gt(id, s->last_enqueued))
					continue;
				sl = slot_for(s, id);
				if (slot_active_for(sl, id))
					slot_release(sl);
				note_expected(s, id);
			}
		}
	}
}

static void parse_receiver_report(struct cast_stream *s, const uint8_t *p, size_t len, int count,
				  int64_t arrival)
{
	if (len < 4 || get_be32(p) != s->cfg.receiver_ssrc)
		return;
	p += 4;
	len -= 4;
	for (int i = 0; i < count && len >= 24; i++, p += 24, len -= 24) {
		uint32_t lsr, dlsr;

		if (get_be32(p) != s->cfg.sender_ssrc)
			continue;
		lsr = get_be32(p + 16);
		dlsr = get_be32(p + 20);
		for (int k = 0; k < SR_HISTORY; k++) {
			int64_t measured;

			if (!s->sr[k].sent_us || s->sr[k].id != lsr)
				continue;
			measured = arrival - s->sr[k].sent_us - (int64_t)dlsr * 1000000 / 65536;
			if (measured < 75)
				measured = 75;
			if (measured > (int64_t)s->cfg.target_delay_ms * 1000)
				measured = (int64_t)s->cfg.target_delay_ms * 1000;
			/* fast attack, slow decay (openscreen SmoothRoundTripTime) */
			if (s->rtt_us == 0)
				s->rtt_us = measured;
			else if (measured > s->rtt_us)
				s->rtt_us = (s->rtt_us + measured) / 2;
			else
				s->rtt_us = (7 * s->rtt_us + measured) / 8;
			break;
		}
	}
}

void cast_stream_rtcp(struct cast_stream *s, const uint8_t *data, size_t len, int64_t arrival_us)
{
	s->last_feedback_us = arrival_us;
	while (len >= 4) {
		int fmt = data[0] & 0x1f;
		int type = data[1];
		size_t body = (size_t)get_be16(data + 2) * 4;

		if ((data[0] >> 6) != 2 || 4 + body > len)
			return;
		if (type == RTCP_RR)
			parse_receiver_report(s, data + 4, body, fmt, arrival_us);
		else if (type == RTCP_PSFB && fmt == PSFB_CAST)
			parse_feedback(s, data + 4, body, arrival_us);
		else if (type == RTCP_PSFB && fmt == PSFB_PLI && body >= 8 &&
			 get_be32(data + 4) == s->cfg.receiver_ssrc && get_be32(data + 8) == s->cfg.sender_ssrc) {
			/* The receiver repeats PLI until it gets a key frame: ignore
			 * it while one is in flight. */
			bool key_in_flight = s->have_key_frame &&
					     (!s->have_checkpoint || id_gt(s->last_key_frame, s->checkpoint));

			if (!key_in_flight)
				s->picture_loss = true;
		}
		/* XR (receiver reference time) and APP (event logs) are not used. */
		data += 4 + body;
		len -= 4 + body;
	}
	send_needed(s, arrival_us);
}

/* Kickstart: the receiver may not know the newest frames if all their
 * packets were lost; resend the last packet of the last frame. */
static int64_t kickstart_due(const struct cast_stream *s, struct slot **out)
{
	struct slot *sl;
	int64_t interval;

	if (!s->any_enqueued || (s->have_expected && !id_gt(s->last_enqueued, s->latest_expected)))
		return -1;
	sl = (struct slot *)&s->slots[s->last_enqueued % SLOTS];
	if (!slot_active_for(sl, s->last_enqueued) || sl->need[sl->packets - 1] ||
	    !sl->sent_us[sl->packets - 1])
		return -1;
	interval = (int64_t)s->cfg.target_delay_ms * 1000 / 20;
	if (interval < s->rtt_us * 2)
		interval = s->rtt_us * 2;
	*out = sl;
	return sl->sent_us[sl->packets - 1] + interval;
}

int64_t cast_stream_next_deadline(const struct cast_stream *s)
{
	struct slot *sl;
	int64_t k, d = -1;

	if (!s->any_enqueued)
		return -1;
	d = s->next_report_us;
	k = kickstart_due(s, &sl);
	if (k >= 0 && (d < 0 || k < d))
		d = k;
	return d;
}

void cast_stream_run(struct cast_stream *s, int64_t now)
{
	struct slot *sl;
	int64_t k;

	if (!s->any_enqueued)
		return;
	if (now >= s->next_report_us)
		send_report(s, now);
	k = kickstart_due(s, &sl);
	if (k >= 0 && now >= k)
		send_packet(s, sl, sl->packets - 1, now);
}

bool cast_stream_take_picture_loss(struct cast_stream *s)
{
	bool v = s->picture_loss;

	s->picture_loss = false;
	return v;
}

int64_t cast_stream_last_feedback_us(const struct cast_stream *s)
{
	return s->last_feedback_us;
}

int64_t cast_stream_rtt_us(const struct cast_stream *s)
{
	return s->rtt_us;
}

void cast_stream_take_counters(struct cast_stream *s, int *retransmits, int *skips)
{
	*retransmits = s->retransmits;
	*skips = s->skips;
	s->retransmits = 0;
	s->skips = 0;
}

void cast_stream_note_skip(struct cast_stream *s)
{
	s->skips++;
}
