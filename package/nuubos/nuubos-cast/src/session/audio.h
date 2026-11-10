/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Audio for the receiver: a PipeWire null sink "nuubos_cast" owned by this
 * process (nuubos-audiod routes the console's sound to it while it exists,
 * like an HDMI output) and a capture stream on its monitor. 48 kHz,
 * stereo, S16.
 */
#ifndef NUUBOS_CAST_AUDIO_H
#define NUUBOS_CAST_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_RATE 48000
#define AUDIO_CHANNELS 2

struct audio_capture;

struct audio_capture *audio_open(void);
void audio_close(struct audio_capture *a);
/* eventfd, readable when samples are waiting. */
int audio_fd(struct audio_capture *a);
/* Clears the eventfd. */
void audio_ack(struct audio_capture *a);
size_t audio_available(struct audio_capture *a);
/* Reads exactly `frames` frames (interleaved) if available; returns the
 * capture time of the first one, or -1 when not enough are buffered. */
int64_t audio_read(struct audio_capture *a, int16_t *out, size_t frames);

#endif
