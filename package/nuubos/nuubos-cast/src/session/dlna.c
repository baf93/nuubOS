/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * DLNA / UPnP AV MediaRenderer (smart TVs, AV receivers): the console is
 * the control point and the media server. The TV is told to play
 * http://<console>/screen.ts (SetAVTransportURI + Play) and fetches a live
 * MPEG transport stream: MPEG-2 video + MPEG-1 Layer II audio, the DVB/DLNA
 * baseline every renderer decodes, both with expired patents.
 *
 * Renderers buffer a few seconds: this protocol is for watching, not for
 * playing (delay=high).
 */
#include "session.h"
#include "aenc.h"
#include "common.h"
#include "http.h"
#include "mux.h"
#include "upnp.h"
#include "venc.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FPS 30
#define MAX_W 1280
#define MAX_H 720
#define AUDIO_KBPS 192
#define PATH "/screen.ts"
#define MIME "video/mpeg"
#define FETCH_TIMEOUT_MS 25000
#define GONE_TIMEOUT_MS 8000
#define SOAP_TIMEOUT_MS 6000

struct dlna {
	struct url control;
	char service[160];
	char url[160];
	struct http_live *http;
	struct venc *venc;
	struct aenc *aenc;
	struct mux *mux;
	int width, height, kbps;
	bool force_key;
	bool video_sent;
	int64_t started_ms;

	pthread_t soap_thread;
	bool soap_running;
	int soap_pipe[2];
	char soap_error[32];
};

static struct dlna *priv(struct session *s)
{
	return s->priv;
}

static int soap(struct dlna *d, const char *action, const char *args)
{
	char *body = NULL, *envelope, soap_action[256];
	size_t size = strlen(args) + 1024;
	int status;

	envelope = malloc(size);
	if (!envelope)
		return -1;
	snprintf(envelope, size,
		 "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		 "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
		 "<u:%s xmlns:u=\"%s\"><InstanceID>0</InstanceID>%s</u:%s>"
		 "</s:Body></s:Envelope>",
		 action, d->service, args, action);
	snprintf(soap_action, sizeof(soap_action), "%s#%s", d->service, action);
	status = http_request(&d->control, soap_action, envelope, &body, SOAP_TIMEOUT_MS);
	if (status != 200)
		log_msg("DLNA %s: HTTP %d %.200s", action, status, body ? body : "");
	free(body);
	free(envelope);
	return status == 200 ? 0 : -1;
}

/* SetAVTransportURI + Play run on a thread: renderers fetch the stream
 * while they handle the request, so the main loop must keep serving. */
static void *soap_start(void *arg)
{
	struct session *s = arg;
	struct dlna *d = priv(s);
	char didl[1024], didl_escaped[2048], args[3072], title[96];
	const char *result = "ok";

	xml_escape(s->console_name, title, sizeof(title));
	snprintf(didl, sizeof(didl),
		 "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
		 "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
		 "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
		 "<item id=\"nuubos-screen\" parentID=\"0\" restricted=\"1\">"
		 "<dc:title>%s</dc:title><upnp:class>object.item.videoItem</upnp:class>"
		 "<res protocolInfo=\"http-get:*:" MIME ":DLNA.ORG_OP=00;DLNA.ORG_CI=0;"
		 "DLNA.ORG_FLAGS=01700000000000000000000000000000\">%s</res></item></DIDL-Lite>",
		 title, d->url);
	xml_escape(didl, didl_escaped, sizeof(didl_escaped));
	snprintf(args, sizeof(args), "<CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>",
		 d->url, didl_escaped);
	/* A renderer still playing something else may refuse a new URI. */
	(void)soap(d, "Stop", "");
	if (soap(d, "SetAVTransportURI", args) != 0)
		result = "refused";
	else if (soap(d, "Play", "<Speed>1</Speed>") != 0)
		result = "refused";
	if (write(d->soap_pipe[1], result, strlen(result)) < 0) {
		/* the main loop is gone */
	}
	return NULL;
}

static int dlna_start(struct session *s)
{
	struct dlna *d = calloc(1, sizeof(*d));
	struct url location;
	char *description = NULL, local[64], control[384];

	if (!d)
		return -1;
	s->priv = d;
	d->soap_pipe[0] = d->soap_pipe[1] = -1;
	if (!url_parse(s->location, &location) || http_request(&location, NULL, NULL, &description, 5000) != 200 ||
	    !description) {
		free(description);
		session_fail(s, "unreachable");
		return -1;
	}
	if (!upnp_control_url(description, "service:AVTransport:", control, sizeof(control), d->service,
			      sizeof(d->service))) {
		free(description);
		session_fail(s, "unsupported");
		return -1;
	}
	free(description);
	url_resolve(&location, control, &d->control);
	if (!local_address_for(d->control.host, local, sizeof(local))) {
		session_fail(s, "network");
		return -1;
	}
	d->http = http_live_open(PATH, MIME, true);
	if (!d->http) {
		session_fail(s, "network");
		return -1;
	}
	snprintf(d->url, sizeof(d->url), "http://%s:%d" PATH, local, http_live_port(d->http));
	fit_size(s->screen_w, s->screen_h, MAX_W, MAX_H, &d->width, &d->height);
	/* MPEG-2 needs more bits than VP8/H.264 for the same picture; the LAN
	 * has them. */
	d->kbps = d->width * d->height > 600000 ? 9000 : 6000;
	d->venc = venc_open(VENC_MPEG2, d->width, d->height, FPS, d->kbps, NULL);
	if (s->audio)
		d->aenc = aenc_open(AENC_MP2, AUDIO_KBPS);
	if (!d->venc || (s->audio && !d->aenc)) {
		session_fail(s, "encoder");
		return -1;
	}
	if (pipe2(d->soap_pipe, O_CLOEXEC) != 0 ||
	    pthread_create(&d->soap_thread, NULL, soap_start, s) != 0) {
		session_fail(s, "network");
		return -1;
	}
	d->soap_running = true;
	d->started_ms = now_ms();
	emit("@info delay=high");
	emit("@info size=%dx%d", d->width, d->height);
	log_msg("DLNA renderer %s:%d%s, stream %s", d->control.host, d->control.port, d->control.path, d->url);
	return 0;
}

static void http_write(void *user, const uint8_t *data, size_t len)
{
	http_live_send(user, data, len);
}

static int dlna_pollfds(struct session *s, struct pollfd *pfd, int max)
{
	struct dlna *d = priv(s);
	int n = 0;

	if (d->soap_running && max > 0)
		pfd[n++] = (struct pollfd){ d->soap_pipe[0], POLLIN, 0 };
	return n + http_live_pollfds(d->http, pfd + n, max - n);
}

static int dlna_dispatch(struct session *s, const struct pollfd *pfd, int count)
{
	struct dlna *d = priv(s);
	int i = 0;

	if (d->soap_running) {
		if (pfd[0].revents) {
			char result[32] = "";
			ssize_t n = read(d->soap_pipe[0], result, sizeof(result) - 1);

			pthread_join(d->soap_thread, NULL);
			d->soap_running = false;
			if (n > 0)
				result[n] = '\0';
			if (strcmp(result, "ok")) {
				session_fail(s, "refused");
				return -1;
			}
		}
		i = 1;
	}
	http_live_dispatch(d->http, pfd + i, count - i);
	if (http_live_take_new_client(d->http)) {
		/* A (re)connecting renderer gets a fresh stream: tables and a
		 * key frame first. */
		mux_close(d->mux);
		d->mux = mux_open(MUX_MPEGTS, VENC_MPEG2, d->width, d->height, FPS, d->aenc, AENC_MP2,
				  http_write, d->http);
		if (!d->mux) {
			session_fail(s, "encoder");
			return -1;
		}
		d->force_key = true;
		d->video_sent = false;
		if (!s->casting) {
			s->casting = true;
			emit("@state casting");
		}
	}
	return 0;
}

static int64_t dlna_deadline(struct session *s)
{
	(void)s;
	return now_us() + 1000000;
}

static int dlna_timers(struct session *s, int64_t now)
{
	struct dlna *d = priv(s);
	int64_t idle = http_live_idle_since(d->http);

	(void)now;
	if (!http_live_ever_streamed(d->http) && now_ms() - d->started_ms > FETCH_TIMEOUT_MS) {
		session_fail(s, "unsupported");
		return -1;
	}
	if (idle && now_ms() - idle > GONE_TIMEOUT_MS) {
		session_fail(s, "stopped");
		return -1;
	}
	return 0;
}

static bool dlna_want_picture(struct session *s, int64_t now)
{
	struct dlna *d = priv(s);

	(void)now;
	return d->mux && http_live_streaming_clients(d->http) > 0;
}

static void dlna_geometry(struct session *s, int *w, int *h, bool *letterbox, int *fps)
{
	struct dlna *d = priv(s);

	*w = d->width;
	*h = d->height;
	*letterbox = true;
	*fps = FPS;
}

static int dlna_picture(struct session *s, const struct picture *pic)
{
	struct dlna *d = priv(s);
	struct venc_packet pkt;
	int rc;

	if (!d->mux)
		return 0;
	rc = venc_encode(d->venc, pic, d->force_key || !d->video_sent, &pkt);
	if (rc != 0)
		return rc < 0 ? -1 : 0;
	d->force_key = false;
	d->video_sent = true;
	if (mux_video(d->mux, &pkt) != 0)
		return -1;
	mux_flush(d->mux);
	return 0;
}

static int dlna_audio(struct session *s, const int16_t *pcm, size_t frames, int64_t time_us)
{
	struct dlna *d = priv(s);
	struct aenc_packet pkt;

	if (!d->aenc || aenc_push(d->aenc, pcm, frames, time_us) != 0)
		return 0;
	while (aenc_pull(d->aenc, &pkt) == 0)
		if (d->mux && d->video_sent && mux_audio(d->mux, &pkt) != 0)
			return -1;
	return 0;
}

static int dlna_repeat_ms(struct session *s)
{
	(void)s;
	return 250; /* a transport stream must keep flowing */
}

static void dlna_force_key(struct session *s)
{
	priv(s)->force_key = true;
}

static void dlna_stop(struct session *s)
{
	struct dlna *d = priv(s);

	if (d && d->control.host[0])
		(void)soap(d, "Stop", "");
}

static void dlna_destroy(struct session *s)
{
	struct dlna *d = priv(s);

	if (!d)
		return;
	if (d->soap_running)
		pthread_join(d->soap_thread, NULL);
	if (d->soap_pipe[0] >= 0)
		close(d->soap_pipe[0]);
	if (d->soap_pipe[1] >= 0)
		close(d->soap_pipe[1]);
	mux_close(d->mux);
	http_live_close(d->http);
	venc_close(d->venc);
	aenc_close(d->aenc);
	free(d);
	s->priv = NULL;
}

const struct proto proto_dlna = {
	.name = "dlna",
	.start = dlna_start,
	.pollfds = dlna_pollfds,
	.dispatch = dlna_dispatch,
	.deadline = dlna_deadline,
	.timers = dlna_timers,
	.want_picture = dlna_want_picture,
	.geometry = dlna_geometry,
	.picture = dlna_picture,
	.audio = dlna_audio,
	.repeat_ms = dlna_repeat_ms,
	.force_key = dlna_force_key,
	.stop = dlna_stop,
	.destroy = dlna_destroy,
};
