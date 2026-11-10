/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Miracast over Infrastructure ([MS-MICE], Microsoft Open Specifications):
 * the Wi-Fi Display session runs over the existing network instead of
 * Wi-Fi Direct (the RTL8821CS rtw88 driver has no P2P modes).
 *
 *   1. TCP to the sink, port 7250: SOURCE_READY (our RTSP port).
 *   2. The sink connects to our RTSP server; the Wi-Fi Display capability
 *      negotiation follows (M1-M7 as source: OPTIONS, GET_PARAMETER,
 *      SET_PARAMETER, trigger SETUP; the sink's SETUP and PLAY).
 *   3. RTP (payload type 33) carrying an MPEG transport stream: H.264
 *      Constrained Baseline (Cisco's OpenH264) + AAC-LC 48 kHz stereo.
 *   4. GET_PARAMETER keep-alive every 25 s; stop = trigger TEARDOWN and
 *      STOP_PROJECTION.
 *
 * Content protection (HDCP) and the optional PIN security handshake are
 * not implemented: sinks that require them refuse the session.
 */
#include "session.h"
#include "aenc.h"
#include "common.h"
#include "mux.h"
#include "venc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MICE_PORT 7250
#define RTSP_PORT 7236
#define MICE_SOURCE_READY 0x01
#define MICE_STOP_PROJECTION 0x02
#define TLV_FRIENDLY_NAME 0x00
#define TLV_RTSP_PORT 0x02
#define TLV_SOURCE_ID 0x03
#define KEEPALIVE_US 25000000
#define CONNECT_TIMEOUT_MS 20000
#define AUDIO_KBPS 128
#define TS_PER_RTP 7

enum phase {
	PH_WAIT_SINK,   /* SOURCE_READY sent, waiting for the RTSP connection */
	PH_M1, PH_M2, PH_M3, PH_M4, PH_M5, /* M2: the sink's OPTIONS; others: our request */
	PH_SETUP,       /* waiting for the sink's SETUP/PLAY */
	PH_PLAYING,
};

struct mice {
	int mice_fd;
	int listen_fd;
	int rtsp_fd;
	int rtsp_port;
	uint8_t source_id[16];
	enum phase phase;
	int cseq;
	int expect_cseq;
	struct bytes in;
	char session_id[16];
	char local_ip[64];
	int sink_rtp_port;
	bool sink_aac;
	int width, height, fps;
	uint32_t cea_bit;
	int64_t started_ms;
	int64_t m2_deadline_ms;
	int64_t next_keepalive_us;

	int rtp_fd;
	uint16_t rtp_seq;
	uint32_t rtp_ssrc;
	struct bytes ts;
	struct venc *venc;
	struct aenc *aenc;
	struct mux *mux;
	bool force_key;
	bool video_sent;
};

static struct mice *priv(struct session *s)
{
	return s->priv;
}

/* ------------------------------------------------------------------ */
/* MS-MICE messages                                                    */
/* ------------------------------------------------------------------ */

static int send_mice(struct session *s, int command)
{
	struct mice *m = priv(s);
	uint8_t msg[512];
	size_t n = 4, name_len = 0;
	const char *name = s->console_name;

	msg[2] = 0x01; /* version */
	msg[3] = (uint8_t)command;
	/* Friendly Name: UTF-16LE (Basic Multilingual Plane; others '?'). */
	uint8_t utf16[240];
	const unsigned char *p = (const unsigned char *)name;

	while (*p && name_len + 2 <= sizeof(utf16)) {
		uint32_t cp = '?';

		if (*p < 0x80) {
			cp = *p++;
		} else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
			cp = (uint32_t)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
			p += 2;
		} else if ((*p & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
			cp = (uint32_t)(p[0] & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
			p += 3;
		} else {
			p++;
			while ((*p & 0xc0) == 0x80)
				p++;
		}
		utf16[name_len++] = (uint8_t)cp;
		utf16[name_len++] = (uint8_t)(cp >> 8);
	}
	msg[n++] = TLV_FRIENDLY_NAME;
	put_be16(msg + n, (uint16_t)name_len);
	n += 2;
	memcpy(msg + n, utf16, name_len);
	n += name_len;
	if (command == MICE_SOURCE_READY) {
		msg[n++] = TLV_RTSP_PORT;
		put_be16(msg + n, 2);
		n += 2;
		put_be16(msg + n, (uint16_t)m->rtsp_port);
		n += 2;
	}
	msg[n++] = TLV_SOURCE_ID;
	put_be16(msg + n, 16);
	n += 2;
	memcpy(msg + n, m->source_id, 16);
	n += 16;
	put_be16(msg, (uint16_t)n);
	return write_all(m->mice_fd, msg, n);
}

/* ------------------------------------------------------------------ */
/* RTSP                                                                */
/* ------------------------------------------------------------------ */

struct rtsp_msg {
	bool response;
	int status;
	char method[24];
	char uri[160];
	int cseq;
	char session[64];
	char transport[160];
	char *body; /* NUL terminated, inside the input buffer copy */
	char raw[4096];
};

static const char *header(const char *headers, const char *name, char *out, size_t size)
{
	size_t len = strlen(name);
	const char *p = headers;

	out[0] = '\0';
	while ((p = strchr(p, '\n')) != NULL) {
		p++;
		if (!strncasecmp(p, name, len) && p[len] == ':') {
			const char *v = p + len + 1;
			size_t l;

			while (*v == ' ')
				v++;
			l = strcspn(v, "\r\n");
			snprintf(out, size, "%.*s", (int)l, v);
			return out;
		}
	}
	return NULL;
}

/* Takes the next complete message out of the input buffer. */
static bool rtsp_next(struct mice *m, struct rtsp_msg *msg)
{
	char *end, value[32];
	size_t head_len, body_len = 0, total;

	if (!m->in.len || !memchr(m->in.data, '\n', m->in.len))
		return false;
	if (bytes_append(&m->in, "", 1) != 0)
		return false;
	m->in.len--;
	end = strstr((char *)m->in.data, "\r\n\r\n");
	if (!end)
		return false;
	head_len = (size_t)(end - (char *)m->in.data) + 4;
	memset(msg, 0, sizeof(*msg));
	snprintf(msg->raw, sizeof(msg->raw), "%.*s", (int)head_len, (char *)m->in.data);
	if (header(msg->raw, "Content-Length", value, sizeof(value)))
		body_len = (size_t)atol(value);
	total = head_len + body_len;
	if (m->in.len < total || total >= sizeof(msg->raw))
		return m->in.len >= sizeof(msg->raw) ? (m->in.len = 0, false) : false;
	memcpy(msg->raw, m->in.data, total);
	msg->raw[total] = '\0';
	msg->body = msg->raw + head_len;
	bytes_consume(&m->in, total);
	if (!strncmp(msg->raw, "RTSP/", 5)) {
		msg->response = true;
		sscanf(msg->raw, "RTSP/%*s %d", &msg->status);
	} else {
		sscanf(msg->raw, "%23s %159s", msg->method, msg->uri);
	}
	if (header(msg->raw, "CSeq", value, sizeof(value)))
		msg->cseq = atoi(value);
	header(msg->raw, "Session", msg->session, sizeof(msg->session));
	header(msg->raw, "Transport", msg->transport, sizeof(msg->transport));
	return true;
}

static int rtsp_request(struct mice *m, const char *method, const char *uri, const char *body)
{
	char msg[2048];
	int n;

	m->expect_cseq = ++m->cseq;
	if (body && body[0])
		n = snprintf(msg, sizeof(msg),
			     "%s %s RTSP/1.0\r\nCSeq: %d\r\nContent-Type: text/parameters\r\n"
			     "Content-Length: %zu\r\n%s%s%s\r\n%s",
			     method, uri, m->cseq, strlen(body),
			     m->session_id[0] && strcmp(method, "OPTIONS") ? "Session: " : "",
			     m->session_id[0] && strcmp(method, "OPTIONS") ? m->session_id : "",
			     m->session_id[0] && strcmp(method, "OPTIONS") ? "\r\n" : "", body);
	else
		n = snprintf(msg, sizeof(msg), "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s%s%s%s\r\n", method, uri,
			     m->cseq, !strcmp(method, "OPTIONS") ? "Require: org.wfa.wfd1.0\r\n" : "",
			     m->session_id[0] && strcmp(method, "OPTIONS") ? "Session: " : "",
			     m->session_id[0] && strcmp(method, "OPTIONS") ? m->session_id : "",
			     m->session_id[0] && strcmp(method, "OPTIONS") ? "\r\n" : "");
	log_msg("rtsp > %s %s", method, body && body[0] ? body : "");
	return write_all(m->rtsp_fd, msg, (size_t)n);
}

static int rtsp_reply(struct mice *m, int cseq, const char *extra)
{
	char msg[1024];
	int n = snprintf(msg, sizeof(msg), "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s\r\n", cseq, extra ? extra : "");

	return write_all(m->rtsp_fd, msg, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Capability negotiation                                              */
/* ------------------------------------------------------------------ */

static const char *param(const char *body, const char *name, char *out, size_t size)
{
	size_t len = strlen(name);
	const char *p = body;

	out[0] = '\0';
	while (p && *p) {
		if (!strncmp(p, name, len) && p[len] == ':') {
			const char *v = p + len + 1;

			while (*v == ' ')
				v++;
			snprintf(out, size, "%.*s", (int)strcspn(v, "\r\n"), v);
			return out;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

/* Picks the mode: H.264 Constrained Baseline (mandatory), 640x480p60 for
 * small screens (mandatory for every sink), 1280x720p30 for taller ones
 * when the sink supports it. */
static int choose_formats(struct session *s, const char *body)
{
	struct mice *m = priv(s);
	char video[512], audio[256], ports[128];
	unsigned native, pref, profile, level, cea, vesa, hh;
	bool cbp = false;
	const char *p;

	if (!param(body, "wfd_video_formats", video, sizeof(video)) || !strcmp(video, "none"))
		return -1;
	m->cea_bit = 0;
	/* "native pref profile level CEA VESA HH ...", codec entries after
	 * the first two fields are separated by ", ". */
	if (sscanf(video, "%x %x", &native, &pref) != 2)
		return -1;
	p = video;
	for (int i = 0; i < 2 && p; i++) {
		p = strchr(p, ' ');
		if (p)
			p++;
	}
	while (p && *p) {
		if (sscanf(p, "%x %x %x %x %x", &profile, &level, &cea, &vesa, &hh) == 5 && (profile & 0x01)) {
			cbp = true;
			if (s->screen_h > 480 && (cea & 0x20))
				m->cea_bit = 0x20;
			else if (!m->cea_bit)
				m->cea_bit = 0x01;
		}
		p = strchr(p, ',');
		if (p)
			p += strspn(p, ", ");
	}
	if (!cbp)
		return -1;
	if (m->cea_bit == 0x20) {
		m->width = 1280;
		m->height = 720;
		m->fps = 30;
	} else {
		m->width = 640;
		m->height = 480;
		m->fps = 30; /* 640x480p60 mode; frames are repeated when needed */
	}
	m->sink_aac = param(body, "wfd_audio_codecs", audio, sizeof(audio)) && strstr(audio, "AAC");
	if (!param(body, "wfd_client_rtp_ports", ports, sizeof(ports)) ||
	    sscanf(ports, "RTP/AVP/UDP;unicast %d", &m->sink_rtp_port) != 1 || m->sink_rtp_port <= 0)
		return -1;
	log_msg("WFD sink: video \"%s\", audio \"%s\", rtp port %d -> %dx%d", video, audio,
		m->sink_rtp_port, m->width, m->height);
	return 0;
}

/* M3: what the sink can do. */
static int send_m3(struct mice *m)
{
	m->phase = PH_M3;
	return rtsp_request(m, "GET_PARAMETER", "rtsp://localhost/wfd1.0",
			    "wfd_video_formats\r\nwfd_audio_codecs\r\nwfd_client_rtp_ports\r\n");
}

static int send_m4(struct session *s)
{
	struct mice *m = priv(s);
	char body[1024];

	snprintf(body, sizeof(body),
		 "wfd_video_formats: 00 00 01 01 %08X 00000000 00000000 00 0000 0000 00 none none\r\n"
		 "%s"
		 "wfd_presentation_URL: rtsp://%s/wfd1.0/streamid=0 none\r\n"
		 "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d 0 mode=play\r\n",
		 m->cea_bit, m->sink_aac && s->audio ? "wfd_audio_codecs: AAC 00000001 00\r\n" : "",
		 m->local_ip, m->sink_rtp_port);
	m->phase = PH_M4;
	return rtsp_request(m, "SET_PARAMETER", "rtsp://localhost/wfd1.0", body);
}

static void ts_write(void *user, const uint8_t *data, size_t len);

static int start_media(struct session *s, const char *peer)
{
	struct mice *m = priv(s);
	struct sockaddr_in to = { 0 };

	m->rtp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	to.sin_family = AF_INET;
	to.sin_port = htons((uint16_t)m->sink_rtp_port);
	if (m->rtp_fd < 0 || inet_pton(AF_INET, peer, &to.sin_addr) != 1 ||
	    connect(m->rtp_fd, (struct sockaddr *)&to, sizeof(to)) != 0)
		return -1;
	m->venc = venc_open(VENC_H264, m->width, m->height, m->fps, m->width > 640 ? 4000 : 2500, s->openh264);
	if (!m->venc) {
		session_fail(s, "encoder");
		return -1;
	}
	if (m->sink_aac && s->audio)
		m->aenc = aenc_open(AENC_AAC, AUDIO_KBPS);
	m->mux = mux_open(MUX_MPEGTS, VENC_H264, m->width, m->height, m->fps, m->aenc, AENC_AAC, ts_write, s);
	if (!m->mux) {
		session_fail(s, "encoder");
		return -1;
	}
	random_bytes(&m->rtp_ssrc, sizeof(m->rtp_ssrc));
	m->force_key = true;
	m->phase = PH_PLAYING;
	s->casting = true;
	emit("@info delay=low");
	emit("@info size=%dx%d", m->width, m->height);
	emit("@state casting");
	return 0;
}

static int handle_rtsp(struct session *s, struct rtsp_msg *msg)
{
	struct mice *m = priv(s);
	char extra[512], peer[64];
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);

	if (msg->response) {
		log_msg("rtsp < %d (CSeq %d) %s", msg->status, msg->cseq, msg->body);
		if (msg->cseq != m->expect_cseq)
			return 0;
		if (msg->status != 200) {
			session_fail(s, m->phase == PH_PLAYING ? "network" : "refused");
			return -1;
		}
		switch (m->phase) {
		case PH_M1:
			/* The sink's own OPTIONS (M2) comes next. */
			m->phase = PH_M2;
			m->m2_deadline_ms = now_ms() + 2000;
			return 0;
		case PH_M3:
			if (choose_formats(s, msg->body) != 0) {
				session_fail(s, "unsupported");
				return -1;
			}
			return send_m4(s);
		case PH_M4:
			m->phase = PH_M5;
			return rtsp_request(m, "SET_PARAMETER", "rtsp://localhost/wfd1.0",
					    "wfd_trigger_method: SETUP\r\n");
		case PH_M5:
			m->phase = PH_SETUP;
			return 0;
		default:
			return 0; /* keep-alive answers */
		}
	}

	log_msg("rtsp < %s %s", msg->method, msg->body);
	if (!strcmp(msg->method, "OPTIONS")) {
		if (rtsp_reply(m, msg->cseq,
			       "Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER\r\n") != 0)
			return -1;
		return m->phase == PH_M2 ? send_m3(m) : 0;
	}
	if (!strcmp(msg->method, "SETUP")) {
		int client_port = m->sink_rtp_port;
		const char *cp = strstr(msg->transport, "client_port=");

		if (cp)
			client_port = atoi(cp + 12);
		if (client_port > 0)
			m->sink_rtp_port = client_port;
		snprintf(m->session_id, sizeof(m->session_id), "%08X", (unsigned)(now_us() & 0x7fffffff));
		snprintf(extra, sizeof(extra),
			 "Session: %s;timeout=30\r\nTransport: RTP/AVP/UDP;unicast;client_port=%d;server_port=%d-%d\r\n",
			 m->session_id, m->sink_rtp_port, 19000, 19001);
		return rtsp_reply(m, msg->cseq, extra);
	}
	if (!strcmp(msg->method, "PLAY")) {
		snprintf(extra, sizeof(extra), "Session: %s\r\nRange: npt=now-\r\n", m->session_id);
		if (rtsp_reply(m, msg->cseq, extra) != 0)
			return -1;
		if (m->phase != PH_PLAYING) {
			if (getpeername(m->rtsp_fd, (struct sockaddr *)&addr, &len) != 0 ||
			    !inet_ntop(AF_INET, &addr.sin_addr, peer, sizeof(peer)) || start_media(s, peer) != 0) {
				if (!s->error[0])
					session_fail(s, "network");
				return -1;
			}
		}
		return 0;
	}
	if (!strcmp(msg->method, "SET_PARAMETER")) {
		if (strstr(msg->body, "wfd_idr_request"))
			m->force_key = true;
		return rtsp_reply(m, msg->cseq, NULL);
	}
	if (!strcmp(msg->method, "GET_PARAMETER") || !strcmp(msg->method, "PAUSE"))
		return rtsp_reply(m, msg->cseq, NULL);
	if (!strcmp(msg->method, "TEARDOWN")) {
		rtsp_reply(m, msg->cseq, NULL);
		session_fail(s, "stopped");
		return -1;
	}
	return rtsp_reply(m, msg->cseq, NULL);
}

static int read_rtsp(struct session *s)
{
	struct mice *m = priv(s);
	struct rtsp_msg msg;
	char buf[4096];
	ssize_t n = recv(m->rtsp_fd, buf, sizeof(buf), MSG_DONTWAIT);

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
		session_fail(s, m->phase == PH_PLAYING ? "stopped" : "refused");
		return -1;
	}
	if (n > 0 && bytes_append(&m->in, buf, (size_t)n) != 0)
		return -1;
	while (rtsp_next(m, &msg))
		if (handle_rtsp(s, &msg) != 0)
			return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* RTP: MPEG-TS packets, 7 per datagram                                */
/* ------------------------------------------------------------------ */

static void send_rtp(struct mice *m, const uint8_t *ts, size_t len)
{
	uint8_t pkt[12 + 188 * TS_PER_RTP];

	pkt[0] = 0x80;
	pkt[1] = 33; /* MP2T */
	put_be16(pkt + 2, m->rtp_seq++);
	put_be32(pkt + 4, (uint32_t)(now_us() * 9 / 100));
	put_be32(pkt + 8, m->rtp_ssrc);
	memcpy(pkt + 12, ts, len);
	if (send(m->rtp_fd, pkt, 12 + len, MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != ECONNREFUSED)
		log_msg("rtp send: %s", strerror(errno));
}

static void ts_write(void *user, const uint8_t *data, size_t len)
{
	struct session *s = user;
	struct mice *m = priv(s);

	if (bytes_append(&m->ts, data, len) != 0)
		return;
	while (m->ts.len >= 188 * TS_PER_RTP) {
		send_rtp(m, m->ts.data, 188 * TS_PER_RTP);
		bytes_consume(&m->ts, 188 * TS_PER_RTP);
	}
}

/* After each frame: the rest goes out now (latency). */
static void ts_flush(struct mice *m)
{
	size_t whole = m->ts.len / 188 * 188;

	if (whole) {
		send_rtp(m, m->ts.data, whole);
		bytes_consume(&m->ts, whole);
	}
}

/* ------------------------------------------------------------------ */
/* proto                                                               */
/* ------------------------------------------------------------------ */

static int open_rtsp_listener(struct mice *m)
{
	struct sockaddr_in addr = { 0 };
	socklen_t len = sizeof(addr);
	int one = 1;

	m->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (m->listen_fd < 0)
		return -1;
	setsockopt(m->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(RTSP_PORT);
	if (bind(m->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		/* 7236 taken: any port, announced in SOURCE_READY. */
		addr.sin_port = 0;
		if (bind(m->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
			return -1;
	}
	if (listen(m->listen_fd, 2) != 0 || getsockname(m->listen_fd, (struct sockaddr *)&addr, &len) != 0)
		return -1;
	m->rtsp_port = ntohs(addr.sin_port);
	return 0;
}

static int mice_start(struct session *s)
{
	struct mice *m = calloc(1, sizeof(*m));

	if (!m)
		return -1;
	s->priv = m;
	m->mice_fd = m->listen_fd = m->rtsp_fd = m->rtp_fd = -1;
	if (!s->openh264[0] || access(s->openh264, R_OK) != 0) {
		session_fail(s, "encoder");
		return -1;
	}
	random_bytes(m->source_id, sizeof(m->source_id));
	if (!local_address_for(s->address, m->local_ip, sizeof(m->local_ip)) || open_rtsp_listener(m) != 0) {
		session_fail(s, "network");
		return -1;
	}
	m->mice_fd = tcp_connect(s->address, MICE_PORT, 6000);
	if (m->mice_fd < 0) {
		session_fail(s, "unreachable");
		return -1;
	}
	if (send_mice(s, MICE_SOURCE_READY) != 0) {
		session_fail(s, "network");
		return -1;
	}
	m->phase = PH_WAIT_SINK;
	m->started_ms = now_ms();
	log_msg("MS-MICE SOURCE_READY sent to %s, RTSP on %d", s->address, m->rtsp_port);
	return 0;
}

static int mice_pollfds(struct session *s, struct pollfd *pfd, int max)
{
	struct mice *m = priv(s);
	int n = 0;

	if (m->rtsp_fd < 0 && m->listen_fd >= 0 && n < max)
		pfd[n++] = (struct pollfd){ m->listen_fd, POLLIN, 0 };
	if (m->rtsp_fd >= 0 && n < max)
		pfd[n++] = (struct pollfd){ m->rtsp_fd, POLLIN, 0 };
	if (m->mice_fd >= 0 && n < max)
		pfd[n++] = (struct pollfd){ m->mice_fd, POLLIN, 0 };
	return n;
}

static int mice_dispatch(struct session *s, const struct pollfd *pfd, int count)
{
	struct mice *m = priv(s);

	for (int i = 0; i < count; i++) {
		if (!pfd[i].revents)
			continue;
		if (pfd[i].fd == m->listen_fd && m->rtsp_fd < 0) {
			m->rtsp_fd = accept4(m->listen_fd, NULL, NULL, SOCK_CLOEXEC);
			if (m->rtsp_fd < 0)
				continue;
			log_msg("WFD sink connected");
			/* M1 */
			m->phase = PH_M1;
			if (rtsp_request(m, "OPTIONS", "*", NULL) != 0) {
				session_fail(s, "network");
				return -1;
			}
		} else if (pfd[i].fd == m->rtsp_fd) {
			if (read_rtsp(s) != 0)
				return -1;
		} else if (pfd[i].fd == m->mice_fd) {
			char buf[256];
			ssize_t n = recv(m->mice_fd, buf, sizeof(buf), MSG_DONTWAIT);

			/* The sink may drop the MICE connection once RTSP runs. */
			if (n == 0 || (n < 0 && errno != EAGAIN)) {
				close(m->mice_fd);
				m->mice_fd = -1;
				if (m->phase == PH_WAIT_SINK) {
					session_fail(s, "refused");
					return -1;
				}
			} else if (n >= 4 && buf[3] == MICE_STOP_PROJECTION) {
				session_fail(s, "stopped");
				return -1;
			}
		}
	}
	return 0;
}

static int64_t mice_deadline(struct session *s)
{
	struct mice *m = priv(s);

	if (m->phase == PH_PLAYING)
		return m->next_keepalive_us;
	return now_us() + 1000000;
}

static int mice_timers(struct session *s, int64_t now)
{
	struct mice *m = priv(s);

	if (m->phase != PH_PLAYING) {
		if (m->phase == PH_M2 && now_ms() >= m->m2_deadline_ms && send_m3(m) != 0) {
			session_fail(s, "network");
			return -1;
		}
		if (now_ms() - m->started_ms > CONNECT_TIMEOUT_MS) {
			session_fail(s, m->rtsp_fd < 0 ? "refused" : "unsupported");
			return -1;
		}
		return 0;
	}
	if (!m->next_keepalive_us)
		m->next_keepalive_us = now + KEEPALIVE_US;
	if (now >= m->next_keepalive_us) {
		/* M16 */
		if (rtsp_request(m, "GET_PARAMETER", "rtsp://localhost/wfd1.0", NULL) != 0) {
			session_fail(s, "network");
			return -1;
		}
		m->next_keepalive_us = now + KEEPALIVE_US;
	}
	return 0;
}

static bool mice_want_picture(struct session *s, int64_t now)
{
	(void)now;
	return priv(s)->phase == PH_PLAYING;
}

static void mice_geometry(struct session *s, int *w, int *h, bool *letterbox, int *fps)
{
	struct mice *m = priv(s);

	*w = m->width ? m->width : 640;
	*h = m->height ? m->height : 480;
	*letterbox = true;
	*fps = m->fps ? m->fps : 30;
}

static int mice_picture(struct session *s, const struct picture *pic)
{
	struct mice *m = priv(s);
	struct venc_packet pkt;
	int rc;

	if (m->phase != PH_PLAYING || !m->mux)
		return 0;
	rc = venc_encode(m->venc, pic, m->force_key || !m->video_sent, &pkt);
	if (rc != 0)
		return rc < 0 ? -1 : 0;
	m->force_key = false;
	m->video_sent = true;
	if (mux_video(m->mux, &pkt) != 0)
		return -1;
	mux_flush(m->mux);
	ts_flush(m);
	return 0;
}

static int mice_audio(struct session *s, const int16_t *pcm, size_t frames, int64_t time_us)
{
	struct mice *m = priv(s);
	struct aenc_packet pkt;

	if (!m->aenc || aenc_push(m->aenc, pcm, frames, time_us) != 0)
		return 0;
	while (aenc_pull(m->aenc, &pkt) == 0)
		if (m->mux && m->video_sent && mux_audio(m->mux, &pkt) != 0)
			return -1;
	return 0;
}

static int mice_repeat_ms(struct session *s)
{
	(void)s;
	return 200; /* sinks expect a continuous stream */
}

static void mice_force_key(struct session *s)
{
	priv(s)->force_key = true;
}

static void mice_stop(struct session *s)
{
	struct mice *m = priv(s);

	if (!m)
		return;
	if (m->rtsp_fd >= 0)
		rtsp_request(m, "SET_PARAMETER", "rtsp://localhost/wfd1.0", "wfd_trigger_method: TEARDOWN\r\n");
	if (m->mice_fd < 0)
		m->mice_fd = tcp_connect(s->address, MICE_PORT, 2000);
	if (m->mice_fd >= 0)
		send_mice(s, MICE_STOP_PROJECTION);
}

static void mice_destroy(struct session *s)
{
	struct mice *m = priv(s);

	if (!m)
		return;
	mux_close(m->mux);
	venc_close(m->venc);
	aenc_close(m->aenc);
	if (m->rtp_fd >= 0)
		close(m->rtp_fd);
	if (m->rtsp_fd >= 0)
		close(m->rtsp_fd);
	if (m->listen_fd >= 0)
		close(m->listen_fd);
	if (m->mice_fd >= 0)
		close(m->mice_fd);
	bytes_free(&m->in);
	bytes_free(&m->ts);
	free(m);
	s->priv = NULL;
}

const struct proto proto_mice = {
	.name = "mice",
	.start = mice_start,
	.pollfds = mice_pollfds,
	.dispatch = mice_dispatch,
	.deadline = mice_deadline,
	.timers = mice_timers,
	.want_picture = mice_want_picture,
	.geometry = mice_geometry,
	.picture = mice_picture,
	.audio = mice_audio,
	.repeat_ms = mice_repeat_ms,
	.force_key = mice_force_key,
	.stop = mice_stop,
	.destroy = mice_destroy,
};
