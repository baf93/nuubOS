/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Google Cast sender.
 *
 * Mirroring (low latency): the receiver's built-in Cast Streaming
 * application (app id 0F5096E8, the one Chrome's tab/desktop casting
 * uses), negotiated with OFFER/ANSWER on urn:x-cast:com.google.cast.webrtc,
 * then VP8 + Opus over encrypted Cast RTP (caststream.c). Message flow per
 * the libcast streaming session protocol (openscreen
 * cast/protocol/streaming_session_protocol.md).
 *
 * Fallback (high latency, a few seconds): receivers without the streaming
 * application get the Default Media Receiver (CC1AD845) loading a live WebM
 * (VP8 + Opus) stream served by this process over HTTP.
 */
#include "session.h"
#include "aenc.h"
#include "caststream.h"
#include "castv2.h"
#include "common.h"
#include "http.h"
#include "mux.h"
#include "venc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#define MIRRORING_APP "0F5096E8"
#define MEDIA_RECEIVER_APP "CC1AD845"
#define RECEIVER_ID "receiver-0"
#define PLATFORM_SENDER "sender-0"
/* Chrome's payload types (openscreen "android RTP hack"): every receiver
 * accepts them, some Android TV receivers accept nothing else. */
#define AUDIO_PT 127
#define VIDEO_PT 96
#define FPS 30
#define MAX_W 1280
#define MAX_H 720
#define MAX_VIDEO_BPS 5000000
#define AUDIO_KBPS 128
#define PING_US 5000000
#define FEEDBACK_TIMEOUT_US 15000000
#define START_TIMEOUT_MS 20000
#define AUTH_TIMEOUT_MS 45000
#define ADAPT_US 2000000

enum phase {
	PH_LAUNCH,      /* LAUNCH sent, waiting for the application */
	PH_OFFER,       /* OFFER sent, waiting for the ANSWER */
	PH_STREAMING,
	PH_LOAD,        /* fallback: LOAD sent, HTTP stream served */
};

struct cast {
	struct castv2 *ch;
	char sender_id[32];
	const char *app;
	char session_id[128];
	char transport_id[128];
	bool app_connected;
	int request_id;
	int seq_num;
	enum phase phase;
	bool fallback;

	int width, height, kbps, max_kbps;
	int64_t t0_us;
	int64_t next_ping_us;
	int64_t started_us;
	int64_t next_adapt_us;
	bool force_key;

	/* mirroring */
	int udp_fd;
	struct cast_stream *video;
	struct cast_stream *audio;
	struct cast_stream_config vcfg, acfg;
	bool offer_audio;
	struct venc *venc;
	struct aenc *aenc;
	uint32_t last_video_rtp;
	bool video_sent;
	uint32_t audio_rtp;
	bool audio_rtp_set;

	/* fallback */
	struct http_live *http;
	struct mux *mux;
	char local_ip[64];
	bool load_sent;
	bool waiting_user; /* the TV asks its user to allow the cast */
};

static struct cast *priv(struct session *s)
{
	return s->priv;
}

static int send_json(struct cast *c, const char *dest, const char *source, const char *ns, const char *json)
{
	return castv2_send(c->ch, source, dest, ns, json);
}

static int connect_to(struct cast *c, const char *source, const char *dest)
{
	char json[512];

	/* CONNECT as libcast's MakeConnectMessage() builds it. */
	snprintf(json, sizeof(json),
		 "{\"type\":\"CONNECT\",\"protocolVersionList\":[0,1,2,3],"
		 "\"userAgent\":\"nuubOS\",\"connType\":0,\"origin\":{},"
		 "\"senderInfo\":{\"sdkType\":2,\"version\":\"nuubOS\",\"browserVersion\":\"nuubOS\","
		 "\"platform\":6,\"connectionType\":1}}");
	return send_json(c, dest, source, NS_CONNECTION, json);
}

static int launch(struct cast *c, const char *app)
{
	char json[256];

	c->app = app;
	c->app_connected = false;
	c->session_id[0] = '\0';
	c->transport_id[0] = '\0';
	c->phase = PH_LAUNCH;
	snprintf(json, sizeof(json),
		 "{\"type\":\"LAUNCH\",\"requestId\":%d,\"appId\":\"%s\",\"language\":\"en-US\","
		 "\"supportedAppTypes\":[\"WEB\"]}", ++c->request_id, app);
	return send_json(c, RECEIVER_ID, PLATFORM_SENDER, NS_RECEIVER, json);
}

/* ------------------------------------------------------------------ */
/* OFFER / ANSWER                                                      */
/* ------------------------------------------------------------------ */

static void stream_keys(struct cast_stream_config *cfg, char key[33], char mask[33])
{
	random_bytes(cfg->aes_key, 16);
	random_bytes(cfg->aes_iv_mask, 16);
	hex_encode(cfg->aes_key, 16, key);
	hex_encode(cfg->aes_iv_mask, 16, mask);
}

static uint32_t random_in(uint32_t lo, uint32_t hi)
{
	uint32_t r;

	random_bytes(&r, sizeof(r));
	return lo + r % (hi - lo + 1);
}

static int send_offer(struct session *s)
{
	struct cast *c = priv(s);
	char json[2048], akey[33], amask[33], vkey[33], vmask[33];
	char audio_json[700] = "";
	int n;

	memset(&c->vcfg, 0, sizeof(c->vcfg));
	memset(&c->acfg, 0, sizeof(c->acfg));
	c->offer_audio = s->audio != NULL;
	/* libcast GenerateSsrc(): audio "higher priority" range. */
	c->acfg.sender_ssrc = random_in(1, 50000);
	c->vcfg.sender_ssrc = random_in(50001, 100000);
	c->acfg.payload_type = AUDIO_PT;
	c->vcfg.payload_type = VIDEO_PT;
	c->acfg.rtp_timebase = AUDIO_RATE;
	c->vcfg.rtp_timebase = 90000;
	c->acfg.target_delay_ms = c->vcfg.target_delay_ms = s->latency_ms;
	c->vcfg.video = true;
	stream_keys(&c->acfg, akey, amask);
	stream_keys(&c->vcfg, vkey, vmask);
	if (c->offer_audio)
		snprintf(audio_json, sizeof(audio_json),
			 "{\"index\":0,\"type\":\"audio_source\",\"codecName\":\"opus\","
			 "\"rtpProfile\":\"cast\",\"rtpPayloadType\":%d,\"ssrc\":%u,"
			 "\"targetDelay\":%d,\"aesKey\":\"%s\",\"aesIvMask\":\"%s\","
			 "\"receiverRtcpEventLog\":false,\"timeBase\":\"1/%d\",\"channels\":%d,"
			 "\"bitRate\":%d},",
			 AUDIO_PT, c->acfg.sender_ssrc, s->latency_ms, akey, amask, AUDIO_RATE,
			 AUDIO_CHANNELS, AUDIO_KBPS * 1000);
	n = snprintf(json, sizeof(json),
		     "{\"type\":\"OFFER\",\"seqNum\":%d,\"offer\":{\"castMode\":\"mirroring\","
		     "\"receiverGetStatus\":true,\"supportedStreams\":[%s"
		     "{\"index\":%d,\"type\":\"video_source\",\"codecName\":\"vp8\","
		     "\"rtpProfile\":\"cast\",\"rtpPayloadType\":%d,\"ssrc\":%u,"
		     "\"targetDelay\":%d,\"aesKey\":\"%s\",\"aesIvMask\":\"%s\","
		     "\"receiverRtcpEventLog\":false,\"timeBase\":\"1/90000\","
		     "\"maxFrameRate\":\"%d\",\"maxBitRate\":%d,"
		     "\"resolutions\":[{\"width\":%d,\"height\":%d}]}]}}",
		     ++c->seq_num, audio_json, c->offer_audio ? 1 : 0, VIDEO_PT, c->vcfg.sender_ssrc,
		     s->latency_ms, vkey, vmask, FPS, MAX_VIDEO_BPS, c->width, c->height);
	if (n <= 0 || (size_t)n >= sizeof(json))
		return -1;
	c->phase = PH_OFFER;
	return send_json(c, c->transport_id, c->sender_id, NS_WEBRTC, json);
}

static int open_udp(struct cast *c, const char *address, int port)
{
	struct sockaddr_in to = { 0 };
	int size = 512 * 1024;

	c->udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (c->udp_fd < 0)
		return -1;
	setsockopt(c->udp_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
	to.sin_family = AF_INET;
	to.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, address, &to.sin_addr) != 1 ||
	    connect(c->udp_fd, (struct sockaddr *)&to, sizeof(to)) != 0)
		return -1;
	return 0;
}

static int64_t ntp_offset(void)
{
	struct timespec real;

	clock_gettime(CLOCK_REALTIME, &real);
	return (int64_t)real.tv_sec * 1000000 + real.tv_nsec / 1000 - now_us();
}

static void initial_bitrate(struct cast *c)
{
	long pixels = (long)c->width * c->height;

	c->max_kbps = pixels > 600000 ? 4500 : pixels > 400000 ? 3500 : 2800;
	if (c->max_kbps > MAX_VIDEO_BPS / 1000)
		c->max_kbps = MAX_VIDEO_BPS / 1000;
	c->kbps = c->max_kbps;
}

static int handle_answer(struct session *s, cJSON *msg)
{
	struct cast *c = priv(s);
	cJSON *answer = cJSON_GetObjectItem(msg, "answer");
	cJSON *result = cJSON_GetObjectItem(msg, "result");
	cJSON *port = answer ? cJSON_GetObjectItem(answer, "udpPort") : NULL;
	cJSON *indexes = answer ? cJSON_GetObjectItem(answer, "sendIndexes") : NULL;
	cJSON *ssrcs = answer ? cJSON_GetObjectItem(answer, "ssrcs") : NULL;
	cJSON *constraints = answer ? cJSON_GetObjectItem(answer, "constraints") : NULL;
	int64_t offset;
	bool have_video = false, have_audio = false;

	if (!cJSON_IsString(result) || strcmp(result->valuestring, "ok") || !cJSON_IsNumber(port) ||
	    !cJSON_IsArray(indexes) || !cJSON_IsArray(ssrcs) ||
	    cJSON_GetArraySize(indexes) != cJSON_GetArraySize(ssrcs)) {
		log_msg("receiver refused the mirroring offer");
		return -1;
	}
	for (int i = 0; i < cJSON_GetArraySize(indexes); i++) {
		cJSON *idx = cJSON_GetArrayItem(indexes, i), *ssrc = cJSON_GetArrayItem(ssrcs, i);

		if (!cJSON_IsNumber(idx) || !cJSON_IsNumber(ssrc))
			continue;
		if (c->offer_audio && idx->valueint == 0) {
			c->acfg.receiver_ssrc = (uint32_t)ssrc->valuedouble;
			have_audio = true;
		} else if (idx->valueint == (c->offer_audio ? 1 : 0)) {
			c->vcfg.receiver_ssrc = (uint32_t)ssrc->valuedouble;
			have_video = true;
		}
	}
	if (!have_video) {
		log_msg("receiver did not pick the video stream");
		return -1;
	}
	/* Receiver limits: fit the picture and the rate. */
	if (constraints) {
		cJSON *video = cJSON_GetObjectItem(constraints, "video");
		cJSON *dims = video ? cJSON_GetObjectItem(video, "maxDimensions") : NULL;
		cJSON *rate = video ? cJSON_GetObjectItem(video, "maxBitRate") : NULL;
		cJSON *w = dims ? cJSON_GetObjectItem(dims, "width") : NULL;
		cJSON *h = dims ? cJSON_GetObjectItem(dims, "height") : NULL;

		if (cJSON_IsNumber(w) && cJSON_IsNumber(h) && w->valueint > 0 && h->valueint > 0)
			fit_size(c->width, c->height, w->valueint, h->valueint, &c->width, &c->height);
		if (cJSON_IsNumber(rate) && rate->valuedouble >= 300000 && rate->valuedouble / 1000 < c->max_kbps)
			c->max_kbps = c->kbps = (int)(rate->valuedouble / 1000);
	}
	if (open_udp(c, s->address, port->valueint) != 0) {
		log_msg("cannot open the media socket");
		return -1;
	}
	offset = ntp_offset();
	c->video = cast_stream_new(&c->vcfg, c->udp_fd, offset);
	if (have_audio)
		c->audio = cast_stream_new(&c->acfg, c->udp_fd, offset);
	c->venc = venc_open(VENC_VP8, c->width, c->height, FPS, c->kbps, NULL);
	if (have_audio && c->audio)
		c->aenc = aenc_open(AENC_OPUS, AUDIO_KBPS);
	if (!c->video || !c->venc || (have_audio && (!c->audio || !c->aenc))) {
		session_fail(s, "encoder");
		return -1;
	}
	c->t0_us = now_us();
	c->started_us = c->t0_us;
	c->next_adapt_us = c->t0_us + ADAPT_US;
	c->force_key = true;
	c->phase = PH_STREAMING;
	s->casting = true;
	emit("@info delay=low");
	emit("@info size=%dx%d", c->width, c->height);
	emit("@state casting");
	log_msg("mirroring to %s:%d, video ssrc %u->%u%s", s->address, port->valueint,
		c->vcfg.sender_ssrc, c->vcfg.receiver_ssrc, have_audio ? ", with audio" : "");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Fallback: Default Media Receiver + live WebM                        */
/* ------------------------------------------------------------------ */

static void http_write(void *user, const uint8_t *data, size_t len)
{
	http_live_send(user, data, len);
}

static int start_fallback(struct session *s)
{
	struct cast *c = priv(s);

	if (c->fallback)
		return -1;
	log_msg("no mirroring application: falling back to the Default Media Receiver");
	c->fallback = true;
	return launch(c, MEDIA_RECEIVER_APP);
}

static int send_load(struct session *s)
{
	struct cast *c = priv(s);
	char json[1024], title[128];

	if (!local_address_for(s->address, c->local_ip, sizeof(c->local_ip)))
		return -1;
	c->http = http_live_open("/nuubos.webm", "video/webm", false);
	if (!c->http)
		return -1;
	c->venc = venc_open(VENC_VP8, c->width, c->height, FPS, c->kbps, NULL);
	if (s->audio)
		c->aenc = aenc_open(AENC_OPUS, AUDIO_KBPS);
	if (!c->venc || (s->audio && !c->aenc)) {
		session_fail(s, "encoder");
		return -1;
	}
	copy_text(title, sizeof(title), s->console_name);
	for (char *p = title; *p; p++)
		if (*p == '"' || *p == '\\')
			*p = ' ';
	snprintf(json, sizeof(json),
		 "{\"type\":\"LOAD\",\"requestId\":%d,\"sessionId\":\"%s\",\"media\":{"
		 "\"contentId\":\"http://%s:%d/nuubos.webm\",\"contentUrl\":\"http://%s:%d/nuubos.webm\","
		 "\"streamType\":\"LIVE\",\"contentType\":\"video/webm\","
		 "\"metadata\":{\"metadataType\":0,\"title\":\"%s\"}},\"autoplay\":true,\"currentTime\":0}",
		 ++c->request_id, c->session_id, c->local_ip, http_live_port(c->http), c->local_ip,
		 http_live_port(c->http), title);
	c->phase = PH_LOAD;
	c->load_sent = true;
	c->started_us = now_us();
	s->casting = true;
	emit("@info delay=high");
	emit("@info fallback=1");
	emit("@info size=%dx%d", c->width, c->height);
	emit("@state casting");
	return send_json(c, c->transport_id, c->sender_id, NS_MEDIA, json);
}

static void restart_mux(struct session *s)
{
	struct cast *c = priv(s);

	http_live_keep_newest(c->http);
	mux_close(c->mux);
	c->mux = mux_open(MUX_WEBM, VENC_VP8, c->width, c->height, FPS, c->aenc, AENC_OPUS,
			  http_write, c->http);
	c->force_key = true;
	if (!c->mux)
		session_fail(s, "encoder");
}

/* ------------------------------------------------------------------ */
/* Messages                                                            */
/* ------------------------------------------------------------------ */

/* Our application in a RECEIVER_STATUS; false when it is gone. */
static bool find_app(struct cast *c, cJSON *msg, cJSON **app_out)
{
	cJSON *status = cJSON_GetObjectItem(msg, "status");
	cJSON *apps = status ? cJSON_GetObjectItem(status, "applications") : NULL;
	cJSON *app;

	cJSON_ArrayForEach(app, apps) {
		cJSON *id = cJSON_GetObjectItem(app, "appId");
		cJSON *sid = cJSON_GetObjectItem(app, "sessionId");

		if (!cJSON_IsString(id) || strcmp(id->valuestring, c->app))
			continue;
		if (c->session_id[0] && (!cJSON_IsString(sid) || strcmp(sid->valuestring, c->session_id)))
			continue;
		*app_out = app;
		return true;
	}
	return false;
}

static int handle_receiver(struct session *s, cJSON *msg, const char *type)
{
	struct cast *c = priv(s);
	cJSON *app = NULL;

	if (!strcmp(type, "RECEIVER_STATUS")) {
		if (!find_app(c, msg, &app)) {
			/* Not (yet) ours: before the launch completes the idle
			 * screen is reported; afterwards our app was closed. */
			if (c->app_connected) {
				session_fail(s, "stopped");
				return -1;
			}
			return 0;
		}
		if (!c->app_connected) {
			cJSON *sid = cJSON_GetObjectItem(app, "sessionId");
			cJSON *tid = cJSON_GetObjectItem(app, "transportId");

			if (!cJSON_IsString(sid) || !cJSON_IsString(tid))
				return 0;
			copy_text(c->session_id, sizeof(c->session_id), sid->valuestring);
			copy_text(c->transport_id, sizeof(c->transport_id), tid->valuestring);
			c->app_connected = true;
			if (connect_to(c, c->sender_id, c->transport_id) != 0)
				return -1;
			if (c->fallback)
				return send_load(s);
			return send_offer(s);
		}
		return 0;
	}
	if (!strcmp(type, "LAUNCH_ERROR")) {
		if (c->phase == PH_LAUNCH && !c->fallback)
			return start_fallback(s);
		session_fail(s, "refused");
		return -1;
	}
	if (!strcmp(type, "LAUNCH_STATUS")) {
		cJSON *status = cJSON_GetObjectItem(msg, "status");

		/* Some receivers ask the user on the TV first. */
		if (cJSON_IsString(status) && !strcmp(status->valuestring, "USER_PENDING_AUTHORIZATION")) {
			c->waiting_user = true;
			emit("@info waiting=tv");
		}
		return 0;
	}
	if (!strcmp(type, "INVALID_REQUEST") && c->phase == PH_LAUNCH) {
		if (!c->fallback)
			return start_fallback(s);
		session_fail(s, "refused");
		return -1;
	}
	return 0;
}

static int handle_media(struct session *s, cJSON *msg, const char *type)
{
	cJSON *list, *item;

	if (strcmp(type, "MEDIA_STATUS"))
		return 0;
	list = cJSON_GetObjectItem(msg, "status");
	cJSON_ArrayForEach(item, list) {
		cJSON *state = cJSON_GetObjectItem(item, "playerState");
		cJSON *reason = cJSON_GetObjectItem(item, "idleReason");

		if (!cJSON_IsString(state) || strcmp(state->valuestring, "IDLE") || !cJSON_IsString(reason))
			continue;
		session_fail(s, !strcmp(reason->valuestring, "ERROR") ? "unsupported" : "stopped");
		return -1;
	}
	return 0;
}

static int handle_message(struct session *s, const struct castv2_message *m)
{
	struct cast *c = priv(s);
	cJSON *msg = cJSON_Parse(m->payload);
	cJSON *type_item;
	const char *type;
	int rc = 0;

	if (!msg)
		return 0;
	type_item = cJSON_GetObjectItem(msg, "type");
	if (!cJSON_IsString(type_item))
		type_item = cJSON_GetObjectItem(msg, "responseType");
	type = cJSON_IsString(type_item) ? type_item->valuestring : "";

	if (!strcmp(m->ns, NS_HEARTBEAT)) {
		if (!strcmp(type, "PING"))
			castv2_send(c->ch, m->destination, m->source, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
	} else if (!strcmp(m->ns, NS_CONNECTION)) {
		if (!strcmp(type, "CLOSE") && (!strcmp(m->source, c->transport_id) || !strcmp(m->source, RECEIVER_ID))) {
			if (c->phase != PH_LAUNCH) {
				session_fail(s, "stopped");
				rc = -1;
			}
		}
	} else if (!strcmp(m->ns, NS_RECEIVER)) {
		rc = handle_receiver(s, msg, type);
	} else if (!strcmp(m->ns, NS_WEBRTC)) {
		if (!strcmp(type, "ANSWER") && c->phase == PH_OFFER) {
			if (handle_answer(s, msg) != 0 && !s->error[0]) {
				/* Mirroring negotiation failed: the media receiver
				 * still shows the screen, with more delay. */
				char stop[256];

				snprintf(stop, sizeof(stop), "{\"type\":\"STOP\",\"requestId\":%d,\"sessionId\":\"%s\"}",
					 ++c->request_id, c->session_id);
				send_json(c, RECEIVER_ID, PLATFORM_SENDER, NS_RECEIVER, stop);
				rc = start_fallback(s);
			}
		}
	} else if (!strcmp(m->ns, NS_MEDIA)) {
		rc = handle_media(s, msg, type);
	}
	cJSON_Delete(msg);
	return rc;
}

static int read_channel(struct session *s)
{
	struct cast *c = priv(s);
	struct castv2_message m;

	if (castv2_read(c->ch) != 0) {
		session_fail(s, "network");
		return -1;
	}
	while (castv2_next(c->ch, &m))
		if (handle_message(s, &m) != 0) {
			if (!s->error[0])
				session_fail(s, "refused");
			return -1;
		}
	return 0;
}

/* ------------------------------------------------------------------ */
/* proto                                                               */
/* ------------------------------------------------------------------ */

static int cast_start(struct session *s)
{
	struct cast *c = calloc(1, sizeof(*c));
	int64_t deadline;
	uint32_t id;

	if (!c)
		return -1;
	s->priv = c;
	c->udp_fd = -1;
	random_bytes(&id, sizeof(id));
	snprintf(c->sender_id, sizeof(c->sender_id), "sender-nuubos-%u", id % 100000);
	fit_size(s->screen_w, s->screen_h, MAX_W, MAX_H, &c->width, &c->height);
	initial_bitrate(c);
	c->ch = castv2_connect(s->address, s->port > 0 ? s->port : 8009, 6000);
	if (!c->ch) {
		session_fail(s, "unreachable");
		return -1;
	}
	if (connect_to(c, PLATFORM_SENDER, RECEIVER_ID) != 0 || launch(c, MIRRORING_APP) != 0) {
		session_fail(s, "network");
		return -1;
	}
	c->next_ping_us = now_us() + PING_US;
	/* Launch and negotiation; the fallback's HTTP stream needs the main
	 * loop, so it returns as soon as LOAD went out. */
	deadline = now_ms() + START_TIMEOUT_MS;
	while (c->phase == PH_LAUNCH || c->phase == PH_OFFER) {
		struct pollfd pfd = { castv2_fd(c->ch), POLLIN, 0 };
		int64_t left = deadline - now_ms();

		if (session_stop_requested())
			return -1;
		if (left <= 0) {
			if (c->phase == PH_OFFER && !c->fallback) {
				log_msg("no ANSWER from the receiver");
				if (start_fallback(s) != 0)
					return -1;
				deadline = now_ms() + START_TIMEOUT_MS;
				continue;
			}
			session_fail(s, "refused");
			return -1;
		}
		if (!castv2_pending(c->ch) && poll(&pfd, 1, left > 200 ? 200 : (int)left) <= 0)
			continue;
		if (read_channel(s) != 0)
			return -1;
		/* Waiting for the user on the TV: give them time. */
		if (c->waiting_user) {
			c->waiting_user = false;
			deadline = now_ms() + AUTH_TIMEOUT_MS;
		}
	}
	return s->error[0] ? -1 : 0;
}

static int cast_pollfds(struct session *s, struct pollfd *pfd, int max)
{
	struct cast *c = priv(s);
	int n = 0;

	if (max < 2)
		return 0;
	pfd[n++] = (struct pollfd){ castv2_fd(c->ch), POLLIN, 0 };
	if (c->udp_fd >= 0)
		pfd[n++] = (struct pollfd){ c->udp_fd, POLLIN, 0 };
	if (c->http)
		n += http_live_pollfds(c->http, pfd + n, max - n);
	return n;
}

static void read_rtcp(struct cast *c)
{
	uint8_t buf[2048];
	ssize_t n;

	while ((n = recv(c->udp_fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		int64_t now = now_us();
		uint32_t ssrc;

		if (n < 8 || (buf[0] >> 6) != 2 || buf[1] < 200 || buf[1] > 207)
			continue;
		ssrc = get_be32(buf + 4);
		if (c->video && ssrc == cast_stream_receiver_ssrc(c->video))
			cast_stream_rtcp(c->video, buf, (size_t)n, now);
		else if (c->audio && ssrc == cast_stream_receiver_ssrc(c->audio))
			cast_stream_rtcp(c->audio, buf, (size_t)n, now);
	}
}

static int cast_dispatch(struct session *s, const struct pollfd *pfd, int count)
{
	struct cast *c = priv(s);
	int i = 0;

	if (count > 0 && (pfd[0].revents || castv2_pending(c->ch)) && read_channel(s) != 0)
		return -1;
	i = 1;
	if (c->udp_fd >= 0 && i < count) {
		if (pfd[i].revents)
			read_rtcp(c);
		i++;
	}
	if (c->http && i < count) {
		http_live_dispatch(c->http, pfd + i, count - i);
		if (http_live_take_new_client(c->http))
			restart_mux(s);
	}
	return s->error[0] ? -1 : 0;
}

static int64_t cast_deadline(struct session *s)
{
	struct cast *c = priv(s);
	int64_t d = c->next_ping_us, v;

	if (c->video && (v = cast_stream_next_deadline(c->video)) >= 0 && v < d)
		d = v;
	if (c->audio && (v = cast_stream_next_deadline(c->audio)) >= 0 && v < d)
		d = v;
	if (c->phase == PH_STREAMING && c->next_adapt_us < d)
		d = c->next_adapt_us;
	return d;
}

static void adapt_bitrate(struct cast *c)
{
	int retransmits, skips, kbps = c->kbps;

	cast_stream_take_counters(c->video, &retransmits, &skips);
	if (skips > 3 || retransmits > 30)
		kbps = kbps * 4 / 5;
	else if (skips == 0 && retransmits < 5)
		kbps = kbps * 11 / 10;
	if (kbps < 600)
		kbps = 600;
	if (kbps > c->max_kbps)
		kbps = c->max_kbps;
	if (kbps != c->kbps) {
		c->kbps = kbps;
		venc_set_bitrate(c->venc, kbps);
		log_msg("video %d kb/s (skips %d, retransmits %d, rtt %lld us)", kbps, skips, retransmits,
			(long long)cast_stream_rtt_us(c->video));
	}
}

static int cast_timers(struct session *s, int64_t now)
{
	struct cast *c = priv(s);

	if (now >= c->next_ping_us) {
		castv2_send(c->ch, PLATFORM_SENDER, RECEIVER_ID, NS_HEARTBEAT, "{\"type\":\"PING\"}");
		c->next_ping_us = now + PING_US;
	}
	if (c->phase == PH_STREAMING) {
		int64_t last = cast_stream_last_feedback_us(c->video);

		cast_stream_run(c->video, now);
		if (c->audio)
			cast_stream_run(c->audio, now);
		if (cast_stream_take_picture_loss(c->video))
			c->force_key = true;
		if (now - (last ? last : c->started_us) > FEEDBACK_TIMEOUT_US) {
			log_msg("no feedback from the receiver");
			session_fail(s, "network");
			return -1;
		}
		if (now >= c->next_adapt_us) {
			adapt_bitrate(c);
			c->next_adapt_us = now + ADAPT_US;
		}
	} else if (c->phase == PH_LOAD && c->http) {
		int64_t idle = http_live_idle_since(c->http);

		/* The receiver never fetched the stream, or left it. */
		if (!http_live_ever_streamed(c->http) && now - c->started_us > 20000000) {
			session_fail(s, "unsupported");
			return -1;
		}
		if (idle && now / 1000 - idle > 10000) {
			session_fail(s, "stopped");
			return -1;
		}
	}
	return 0;
}

static bool cast_want_picture(struct session *s, int64_t now)
{
	struct cast *c = priv(s);

	(void)now;
	if (c->phase == PH_STREAMING)
		return true;
	return c->phase == PH_LOAD && c->mux && http_live_streaming_clients(c->http) > 0;
}

static void cast_geometry(struct session *s, int *w, int *h, bool *letterbox, int *fps)
{
	struct cast *c = priv(s);

	*w = c->width;
	*h = c->height;
	*letterbox = true; /* keeps the aspect if the receiver shrank it */
	*fps = FPS;
}

static int cast_picture(struct session *s, const struct picture *pic)
{
	struct cast *c = priv(s);
	struct venc_packet pkt;
	bool key = c->force_key || !c->video_sent;
	int rc;

	if (c->phase == PH_STREAMING) {
		uint32_t rtp = (uint32_t)((pic->time_us - c->t0_us) * 90 / 1000);

		if (c->video_sent && (int32_t)(rtp - c->last_video_rtp) <= 0)
			rtp = c->last_video_rtp + 1;
		if (!cast_stream_can_enqueue(c->video, rtp)) {
			cast_stream_note_skip(c->video);
			return 0;
		}
		rc = venc_encode(c->venc, pic, key, &pkt);
		if (rc < 0)
			return -1;
		if (rc > 0)
			return 0;
		c->force_key = false;
		c->video_sent = true;
		c->last_video_rtp = rtp;
		return cast_stream_enqueue(c->video, pkt.data, pkt.len, pkt.key, rtp, pic->time_us);
	}
	if (c->phase == PH_LOAD && c->mux) {
		rc = venc_encode(c->venc, pic, key, &pkt);
		if (rc < 0)
			return -1;
		if (rc > 0)
			return 0;
		c->force_key = false;
		c->video_sent = true;
		if (mux_video(c->mux, &pkt) != 0)
			return -1;
		mux_flush(c->mux);
	}
	return 0;
}

static int cast_audio(struct session *s, const int16_t *pcm, size_t frames, int64_t time_us)
{
	struct cast *c = priv(s);
	struct aenc_packet pkt;

	if (!c->aenc || aenc_push(c->aenc, pcm, frames, time_us) != 0)
		return 0;
	while (aenc_pull(c->aenc, &pkt) == 0) {
		if (c->phase == PH_STREAMING && c->audio) {
			uint32_t expected = (uint32_t)((pkt.time_us - c->t0_us) * AUDIO_RATE / 1000000);

			/* Continuous sample clock, re-anchored if it drifts
			 * from the capture time by more than 40 ms. */
			if (!c->audio_rtp_set || abs((int32_t)(c->audio_rtp - expected)) > AUDIO_RATE / 25)
				c->audio_rtp = expected;
			c->audio_rtp_set = true;
			if (cast_stream_can_enqueue(c->audio, c->audio_rtp))
				cast_stream_enqueue(c->audio, pkt.data, pkt.len, true, c->audio_rtp, pkt.time_us);
			c->audio_rtp += (uint32_t)pkt.samples;
		} else if (c->phase == PH_LOAD && c->mux && c->video_sent) {
			if (mux_audio(c->mux, &pkt) != 0)
				return -1;
		}
	}
	return 0;
}

static int cast_repeat_ms(struct session *s)
{
	struct cast *c = priv(s);

	/* A WebM live stream must keep flowing; Cast Streaming receivers
	 * keep the last picture. */
	return c->phase == PH_LOAD ? 500 : 0;
}

static void cast_force_key(struct session *s)
{
	priv(s)->force_key = true;
}

static void cast_stop(struct session *s)
{
	struct cast *c = priv(s);
	char json[256];

	if (!c || !c->ch)
		return;
	if (c->session_id[0]) {
		snprintf(json, sizeof(json), "{\"type\":\"STOP\",\"requestId\":%d,\"sessionId\":\"%s\"}",
			 ++c->request_id, c->session_id);
		send_json(c, RECEIVER_ID, PLATFORM_SENDER, NS_RECEIVER, json);
	}
	if (c->app_connected)
		send_json(c, c->transport_id, c->sender_id, NS_CONNECTION, "{\"type\":\"CLOSE\"}");
	send_json(c, RECEIVER_ID, PLATFORM_SENDER, NS_CONNECTION, "{\"type\":\"CLOSE\"}");
}

static void cast_destroy(struct session *s)
{
	struct cast *c = priv(s);

	if (!c)
		return;
	cast_stream_free(c->video);
	cast_stream_free(c->audio);
	if (c->udp_fd >= 0)
		close(c->udp_fd);
	mux_close(c->mux);
	http_live_close(c->http);
	venc_close(c->venc);
	aenc_close(c->aenc);
	castv2_close(c->ch);
	free(c);
	s->priv = NULL;
}

const struct proto proto_cast = {
	.name = "cast",
	.start = cast_start,
	.pollfds = cast_pollfds,
	.dispatch = cast_dispatch,
	.deadline = cast_deadline,
	.timers = cast_timers,
	.want_picture = cast_want_picture,
	.geometry = cast_geometry,
	.picture = cast_picture,
	.audio = cast_audio,
	.repeat_ms = cast_repeat_ms,
	.force_key = cast_force_key,
	.stop = cast_stop,
	.destroy = cast_destroy,
};
