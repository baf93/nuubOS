/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-cast-session: one casting session (screen + sound to one
 * receiver) or one discovery, run by nuubos-castd as a child process.
 *
 * stdout: "@state connecting|casting", "@info key=value", "@error REASON",
 * "@rx ..." (discovery); stdin: "STOP", "KEYFRAME".
 * Error reasons: unreachable, refused, unsupported, encoder, network,
 * stopped (ended on the receiver), capture.
 */
#ifndef NUUBOS_CAST_SESSION_H
#define NUUBOS_CAST_SESSION_H

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#include "audio.h"
#include "capture.h"
#include "picture.h"

struct session;

struct proto {
	const char *name;
	/* Connect and negotiate what can be done before the main loop. */
	int (*start)(struct session *s);
	int (*pollfds)(struct session *s, struct pollfd *pfd, int max);
	/* -1: the session ended (s->error set). */
	int (*dispatch)(struct session *s, const struct pollfd *pfd, int count);
	int64_t (*deadline)(struct session *s);
	int (*timers)(struct session *s, int64_t now);
	/* Ready for the next picture, and its geometry. */
	bool (*want_picture)(struct session *s, int64_t now);
	void (*geometry)(struct session *s, int *width, int *height, bool *letterbox, int *fps);
	int (*picture)(struct session *s, const struct picture *pic);
	int (*audio)(struct session *s, const int16_t *pcm, size_t frames, int64_t time_us);
	/* Static screens: repeat the last picture after this long (0 = never). */
	int (*repeat_ms)(struct session *s);
	void (*force_key)(struct session *s);
	/* Local stop: tell the receiver. */
	void (*stop)(struct session *s);
	void (*destroy)(struct session *s);
};

struct session {
	const struct proto *proto;
	void *priv;

	/* arguments */
	char address[64];
	int port;
	char name[128];
	char location[512]; /* DLNA device description URL */
	char openh264[256];
	char console_name[64];
	int latency_ms;
	bool with_audio;

	char error[32];
	bool casting;

	/* capture pipeline */
	struct capture *capture;
	struct converter *converter;
	struct picture pic;
	bool have_pic;
	int64_t next_capture_us;
	int64_t capture_started_us;
	int64_t last_picture_us;
	int screen_w, screen_h;

	struct audio_capture *audio;
};

void session_fail(struct session *s, const char *reason);
/* Logical size of the captured screen (0 until the first frame). */
void session_screen_size(struct session *s, int *w, int *h);
/* Is the stop command waiting on stdin? Used by blocking negotiations. */
bool session_stop_requested(void);

extern const struct proto proto_cast;
extern const struct proto proto_dlna;
extern const struct proto proto_mice;

int discover_run(int seconds);

#endif
