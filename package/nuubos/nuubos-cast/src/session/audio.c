/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio.h"
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/* One second of audio: the main loop drains it every 20 ms. */
#define RING_FRAMES AUDIO_RATE

struct audio_capture {
	struct pw_thread_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_proxy *sink;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	int event_fd;

	pthread_mutex_t lock;
	int16_t ring[RING_FRAMES * AUDIO_CHANNELS];
	size_t head; /* frames written */
	size_t tail; /* frames read */
	int64_t tail_time_us; /* capture time of the frame at `tail` */
	bool overrun_logged;
};

static void on_process(void *data)
{
	struct audio_capture *a = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(a->stream);
	struct spa_buffer *buf;
	const int16_t *samples;
	size_t frames;
	int64_t now;
	uint64_t one = 1;

	if (!b)
		return;
	buf = b->buffer;
	if (!buf->datas[0].data || !buf->datas[0].chunk) {
		pw_stream_queue_buffer(a->stream, b);
		return;
	}
	samples = SPA_PTROFF(buf->datas[0].data, buf->datas[0].chunk->offset, const int16_t);
	frames = buf->datas[0].chunk->size / (sizeof(int16_t) * AUDIO_CHANNELS);
	now = now_us();

	pthread_mutex_lock(&a->lock);
	if (a->head == a->tail)
		a->tail_time_us = now - (int64_t)frames * 1000000 / AUDIO_RATE;
	for (size_t i = 0; i < frames; i++) {
		if (a->head - a->tail >= RING_FRAMES) {
			/* The session fell behind: drop the oldest. */
			a->tail++;
			a->tail_time_us += 1000000 / AUDIO_RATE;
			if (!a->overrun_logged) {
				log_msg("audio ring overrun");
				a->overrun_logged = true;
			}
		}
		memcpy(&a->ring[(a->head % RING_FRAMES) * AUDIO_CHANNELS],
		       &samples[i * AUDIO_CHANNELS], sizeof(int16_t) * AUDIO_CHANNELS);
		a->head++;
	}
	pthread_mutex_unlock(&a->lock);
	pw_stream_queue_buffer(a->stream, b);
	if (write(a->event_fd, &one, sizeof(one)) < 0) {
		/* counter saturated: a wakeup is pending anyway */
	}
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
			     const char *error)
{
	(void)data; (void)old;
	log_msg("audio stream %s%s%s", pw_stream_state_as_string(state), error ? ": " : "",
		error ? error : "");
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.process = on_process,
};

struct audio_capture *audio_open(void)
{
	struct audio_capture *a = calloc(1, sizeof(*a));
	uint8_t pod_buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
	const struct spa_pod *params[1];
	struct spa_audio_info_raw info = { 0 };
	struct pw_properties *sink_props;

	if (!a)
		return NULL;
	pthread_mutex_init(&a->lock, NULL);
	a->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (a->event_fd < 0) {
		free(a);
		return NULL;
	}
	pw_init(NULL, NULL);
	a->loop = pw_thread_loop_new("cast-audio", NULL);
	if (!a->loop)
		goto fail;
	a->context = pw_context_new(pw_thread_loop_get_loop(a->loop), NULL, 0);
	if (!a->context)
		goto fail;
	if (pw_thread_loop_start(a->loop) != 0)
		goto fail;
	pw_thread_loop_lock(a->loop);
	a->core = pw_context_connect(a->context, NULL, 0);
	if (!a->core) {
		pw_thread_loop_unlock(a->loop);
		log_msg("PipeWire unavailable");
		goto fail;
	}

	/* The sink lives as long as this client: a crashed session never
	 * leaves the console's sound routed to nowhere. */
	sink_props = pw_properties_new(
		"factory.name", "support.null-audio-sink",
		PW_KEY_NODE_NAME, "nuubos_cast",
		PW_KEY_NODE_DESCRIPTION, "nuubOS Cast",
		PW_KEY_MEDIA_CLASS, "Audio/Sink",
		"audio.position", "FL,FR",
		"audio.rate", "48000",
		"monitor.channel-volumes", "false",
		PW_KEY_OBJECT_LINGER, "false",
		NULL);
	a->sink = pw_core_create_object(a->core, "adapter", PW_TYPE_INTERFACE_Node,
					PW_VERSION_NODE, &sink_props->dict, 0);
	pw_properties_free(sink_props);
	if (!a->sink) {
		pw_thread_loop_unlock(a->loop);
		log_msg("cannot create the cast sink");
		goto fail;
	}

	a->stream = pw_stream_new(a->core, "nuubOS Cast",
				  pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
						    PW_KEY_MEDIA_CATEGORY, "Capture",
						    PW_KEY_MEDIA_ROLE, "Screen",
						    PW_KEY_TARGET_OBJECT, "nuubos_cast",
						    PW_KEY_STREAM_CAPTURE_SINK, "true",
						    PW_KEY_NODE_LATENCY, "480/48000",
						    NULL));
	if (!a->stream) {
		pw_thread_loop_unlock(a->loop);
		goto fail;
	}
	pw_stream_add_listener(a->stream, &a->stream_listener, &stream_events, a);
	info.format = SPA_AUDIO_FORMAT_S16_LE;
	info.rate = AUDIO_RATE;
	info.channels = AUDIO_CHANNELS;
	info.position[0] = SPA_AUDIO_CHANNEL_FL;
	info.position[1] = SPA_AUDIO_CHANNEL_FR;
	params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
	if (pw_stream_connect(a->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
			      PW_STREAM_FLAG_RT_PROCESS,
			      params, 1) != 0) {
		pw_thread_loop_unlock(a->loop);
		log_msg("cannot connect the cast capture stream");
		goto fail;
	}
	pw_thread_loop_unlock(a->loop);
	return a;
fail:
	audio_close(a);
	return NULL;
}

void audio_close(struct audio_capture *a)
{
	if (!a)
		return;
	if (a->loop)
		pw_thread_loop_lock(a->loop);
	if (a->stream)
		pw_stream_destroy(a->stream);
	if (a->sink)
		pw_proxy_destroy(a->sink);
	if (a->core)
		pw_core_disconnect(a->core);
	if (a->loop) {
		pw_thread_loop_unlock(a->loop);
		pw_thread_loop_stop(a->loop);
	}
	if (a->context)
		pw_context_destroy(a->context);
	if (a->loop)
		pw_thread_loop_destroy(a->loop);
	if (a->event_fd >= 0)
		close(a->event_fd);
	pthread_mutex_destroy(&a->lock);
	free(a);
}

int audio_fd(struct audio_capture *a)
{
	return a->event_fd;
}

void audio_ack(struct audio_capture *a)
{
	uint64_t value;

	if (read(a->event_fd, &value, sizeof(value)) < 0) {
		/* EAGAIN: nothing pending */
	}
}

size_t audio_available(struct audio_capture *a)
{
	size_t n;

	pthread_mutex_lock(&a->lock);
	n = a->head - a->tail;
	pthread_mutex_unlock(&a->lock);
	return n;
}

int64_t audio_read(struct audio_capture *a, int16_t *out, size_t frames)
{
	int64_t t;

	pthread_mutex_lock(&a->lock);
	if (a->head - a->tail < frames) {
		pthread_mutex_unlock(&a->lock);
		return -1;
	}
	t = a->tail_time_us;
	for (size_t i = 0; i < frames; i++) {
		memcpy(&out[i * AUDIO_CHANNELS], &a->ring[(a->tail % RING_FRAMES) * AUDIO_CHANNELS],
		       sizeof(int16_t) * AUDIO_CHANNELS);
		a->tail++;
	}
	a->tail_time_us += (int64_t)frames * 1000000 / AUDIO_RATE;
	pthread_mutex_unlock(&a->lock);
	return t;
}
