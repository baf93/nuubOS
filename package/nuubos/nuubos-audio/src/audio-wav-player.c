/* SPDX-License-Identifier: MIT */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUUBOS_ALSA_CONFIG "/usr/share/nuubos/audio/alsa.conf"

struct wav_info {
	unsigned int channels;
	unsigned int rate;
	unsigned int bits;
	long data_offset;
	uint32_t data_size;
};

static uint16_t le16(const unsigned char *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static int parse_wav(FILE *fp, struct wav_info *info)
{
	unsigned char h[12];
	int have_fmt = 0;

	if (fread(h, 1, sizeof(h), fp) != sizeof(h))
		return -1;
	if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
		return -1;

	while (!feof(fp)) {
		unsigned char ch[8];
		uint32_t size;
		long payload;

		if (fread(ch, 1, sizeof(ch), fp) != sizeof(ch))
			break;

		size = le32(ch + 4);
		payload = ftell(fp);

		if (memcmp(ch, "fmt ", 4) == 0) {
			unsigned char fmt[40];
			size_t need = size < sizeof(fmt) ? size : sizeof(fmt);

			memset(fmt, 0, sizeof(fmt));
			if (fread(fmt, 1, need, fp) != need)
				return -1;

			if (le16(fmt) != 1)
				return -1;

			info->channels = le16(fmt + 2);
			info->rate = le32(fmt + 4);
			info->bits = le16(fmt + 14);
			have_fmt = 1;
		} else if (memcmp(ch, "data", 4) == 0) {
			if (!have_fmt)
				return -1;
			info->data_offset = payload;
			info->data_size = size;
			return 0;
		}

		if (fseek(fp, payload + size + (size & 1U), SEEK_SET) != 0)
			return -1;
	}

	return -1;
}

static volatile sig_atomic_t anchor_running = 1;

static void handle_anchor_signal(int sig)
{
	(void)sig;
	anchor_running = 0;
}

static int run_anchor(const char *device)
{
	snd_pcm_t *pcm = NULL;
	int16_t silence[480 * 2];
	int rc = 1;

	memset(silence, 0, sizeof(silence));
	signal(SIGINT, handle_anchor_signal);
	signal(SIGTERM, handle_anchor_signal);

	if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		fprintf(stderr, "cannot open anchor PCM %s\n", device);
		goto out;
	}

	if (snd_pcm_set_params(
		    pcm,
		    SND_PCM_FORMAT_S16_LE,
		    SND_PCM_ACCESS_RW_INTERLEAVED,
		    2,
		    48000,
		    1,
		    100000) < 0) {
		fprintf(stderr, "cannot configure anchor PCM %s\n", device);
		goto out;
	}

	while (anchor_running) {
		snd_pcm_sframes_t frames = 480;
		int16_t *p = silence;

		while (frames > 0 && anchor_running) {
			snd_pcm_sframes_t written =
				snd_pcm_writei(pcm, p, (snd_pcm_uframes_t)frames);

			if (written == -EPIPE) {
				snd_pcm_prepare(pcm);
				continue;
			}

			if (written < 0)
				written = snd_pcm_recover(pcm, (int)written, 1);

			if (written < 0)
				goto out;

			p += (size_t)written * 2U;
			frames -= written;
		}
	}

	rc = 0;
out:
	if (pcm) {
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
	}
	return rc;
}

/*
 * Persistent system-sound player ("sfx-server").
 *
 * Opening a PCM on the PipeWire ALSA plugin creates a new PipeWire client
 * and stream node, which WirePlumber then has to set up and link: ~60 ms of
 * WirePlumber CPU per navigation cue when a player process was spawned per
 * cue. The server keeps one PCM (one stream node) open for its lifetime and
 * plays cues sent by nuubos-audiod on stdin, one per line:
 *
 *   PLAY <volume 1..100> <path to S16_LE mono/stereo WAV>
 *
 * Between cues the PCM is left prepared: the stream is inactive (no
 * processing, no wakeups) and the process sleeps in read(). A new cue
 * interrupts the one playing, as the previous kill-and-respawn did. EOF on
 * stdin (audiod closed the pipe) ends the server.
 */
static char line_buf[1024];
static size_t line_len;

/* 1: a complete command line is buffered; 0: timeout; -1: EOF/error. */
static int wait_line(int timeout_ms)
{
	for (;;) {
		struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
		ssize_t n;
		int rc;

		if (memchr(line_buf, '\n', line_len) != NULL)
			return 1;
		if (line_len >= sizeof(line_buf))
			line_len = 0; /* overlong garbage: drop it */

		rc = poll(&pfd, 1, timeout_ms);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0)
			return -1;
		if (rc == 0)
			return 0;

		n = read(STDIN_FILENO, line_buf + line_len,
			 sizeof(line_buf) - line_len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		line_len += (size_t)n;
	}
}

static void take_line(char *out, size_t size)
{
	char *nl = memchr(line_buf, '\n', line_len);
	size_t n = nl != NULL ? (size_t)(nl - line_buf) : line_len;
	size_t copy = n < size - 1 ? n : size - 1;

	memcpy(out, line_buf, copy);
	out[copy] = '\0';
	if (nl != NULL)
		n++;
	memmove(line_buf, line_buf + n, line_len - n);
	line_len -= n;
}

/* Open (or reopen for a new format) the server PCM. */
static int server_pcm(snd_pcm_t **pcm, const char *device,
		      unsigned int *rate, unsigned int *channels,
		      const struct wav_info *info)
{
	if (*pcm != NULL && *rate == info->rate && *channels == info->channels)
		return 0;

	if (*pcm != NULL) {
		snd_pcm_close(*pcm);
		*pcm = NULL;
	}
	if (snd_pcm_open(pcm, device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		fprintf(stderr, "sfx-server: cannot open PCM %s\n", device);
		*pcm = NULL;
		return -1;
	}
	if (snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE,
			       SND_PCM_ACCESS_RW_INTERLEAVED,
			       info->channels, info->rate, 1, 15000U) < 0) {
		fprintf(stderr, "sfx-server: cannot configure PCM %s\n", device);
		snd_pcm_close(*pcm);
		*pcm = NULL;
		return -1;
	}
	*rate = info->rate;
	*channels = info->channels;
	return 0;
}

/* Play one cue. Returns early, with the PCM dropped, when a new command
 * arrives; -1 only when stdin is gone. */
static int server_play(snd_pcm_t **pcm, const char *device,
		       unsigned int *rate, unsigned int *channels,
		       int volume, const char *path)
{
	struct wav_info info = {0};
	unsigned char buffer[4096];
	unsigned long long total_frames, fade_frames, played = 0;
	uint32_t remaining;
	size_t frame_bytes, chunk;
	FILE *fp = fopen(path, "rb");
	int rc = 0;

	if (fp == NULL)
		return 0;
	if (parse_wav(fp, &info) != 0 || info.bits != 16 ||
	    (info.channels != 1 && info.channels != 2) ||
	    info.rate < 8000 || info.rate > 192000 ||
	    fseek(fp, info.data_offset, SEEK_SET) != 0 ||
	    server_pcm(pcm, device, rate, channels, &info) != 0) {
		fclose(fp);
		return 0;
	}

	frame_bytes = info.channels * 2U;
	remaining = info.data_size;
	total_frames = remaining / frame_bytes;
	fade_frames = ((unsigned long long)info.rate * 4ULL) / 1000ULL;
	if (fade_frames * 2ULL > total_frames)
		fade_frames = total_frames / 2ULL;
	/* ~10 ms per write, so a new command is noticed quickly. */
	chunk = (info.rate / 100U) * frame_bytes;
	if (chunk == 0 || chunk > sizeof(buffer))
		chunk = sizeof(buffer) - sizeof(buffer) % frame_bytes;

	(void)snd_pcm_prepare(*pcm);

	while (remaining > 0) {
		size_t want = remaining < chunk ? remaining : chunk;
		size_t got = fread(buffer, 1, want, fp);
		size_t i;
		snd_pcm_sframes_t frames;
		unsigned char *p = buffer;
		int pending = wait_line(0);

		if (pending != 0) {
			snd_pcm_drop(*pcm);
			rc = pending < 0 ? -1 : 0;
			goto out;
		}
		if (got < frame_bytes)
			break;

		for (i = 0; i < got / 2; i++) {
			unsigned long long frame = played + i / info.channels;
			unsigned int envelope = 1000U;
			long long scaled;
			int16_t sample;

			if (fade_frames > 0 && frame < fade_frames)
				envelope = (unsigned int)((frame * 1000ULL) / fade_frames);
			if (fade_frames > 0 && frame + fade_frames >= total_frames) {
				unsigned long long left = total_frames > frame
					? total_frames - frame - 1ULL : 0ULL;
				unsigned int tail =
					(unsigned int)((left * 1000ULL) / fade_frames);
				if (tail < envelope)
					envelope = tail;
			}
			memcpy(&sample, buffer + i * 2, sizeof(sample));
			scaled = ((long long)sample * volume * envelope) / 100000LL;
			if (scaled > 32767)
				scaled = 32767;
			if (scaled < -32768)
				scaled = -32768;
			sample = (int16_t)scaled;
			memcpy(buffer + i * 2, &sample, sizeof(sample));
		}

		frames = (snd_pcm_sframes_t)(got / frame_bytes);
		while (frames > 0) {
			snd_pcm_sframes_t written =
				snd_pcm_writei(*pcm, p, (snd_pcm_uframes_t)frames);

			if (written < 0)
				written = snd_pcm_recover(*pcm, (int)written, 1);
			if (written < 0)
				goto out;
			p += (size_t)written * frame_bytes;
			frames -= written;
		}
		played += got / frame_bytes;
		remaining -= (uint32_t)got;
	}

	/* Let the buffered tail play out, then stop the stream. snd_pcm_drain
	 * on the PipeWire plugin keeps the stream (and this thread) waking for
	 * a long time, and its delay never reaches zero while running: read
	 * the delay once (time until the last written frame is heard), wait
	 * that long while still accepting a new cue, then drop, which leaves
	 * nothing unplayed and makes the stream inactive. */
	if (snd_pcm_state(*pcm) == SND_PCM_STATE_PREPARED)
		(void)snd_pcm_start(*pcm);
	{
		snd_pcm_sframes_t delay = 0;
		int pending;

		if (snd_pcm_delay(*pcm, &delay) < 0 || delay < 0)
			delay = 0;
		pending = wait_line((int)((delay * 1000) / (snd_pcm_sframes_t)info.rate) + 5);
		if (pending < 0)
			rc = -1;
	}
	snd_pcm_drop(*pcm);
out:
	fclose(fp);
	return rc;
}

static int run_sfx_server(const char *device)
{
	snd_pcm_t *pcm = NULL;
	unsigned int rate = 0, channels = 0;
	char line[sizeof(line_buf)];

	for (;;) {
		char path[512];
		int volume;

		if (wait_line(-1) < 0)
			break;
		take_line(line, sizeof(line));
		if (sscanf(line, "PLAY %d %511s", &volume, path) != 2 ||
		    volume <= 0 || volume > 100)
			continue;
		if (server_play(&pcm, device, &rate, &channels, volume, path) < 0)
			break;
	}

	if (pcm != NULL) {
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *device;
	const char *path;
	const char *profile;
	bool sfx_profile;
	unsigned int latency_us;
	struct wav_info info = {0};
	snd_pcm_t *pcm = NULL;
	FILE *fp = NULL;
	unsigned char buffer[16384];
	unsigned long skip_ms;
	unsigned long long skip_frames;
	unsigned long long skip_bytes;
	uint32_t remaining;
	unsigned long long played_frames = 0;
	unsigned long long total_frames = 0;
	unsigned long long fade_frames = 0;
	int volume;
	int rc = 1;

	if (argc != 5 && argc != 6) {
		fprintf(stderr,
			"Usage: %s PCM VOLUME SKIP_MS [PROFILE] FILE\n",
			argv[0]);
		return 2;
	}

	if (access(NUUBOS_ALSA_CONFIG, R_OK) == 0)
		(void)setenv("ALSA_CONFIG_PATH", NUUBOS_ALSA_CONFIG, 1);

	device = argv[1];
	volume = atoi(argv[2]);
	skip_ms = strtoul(argv[3], NULL, 10);
	profile = argc == 6 ? argv[4] : "stream";
	path = argc == 6 ? argv[5] : argv[4];
	sfx_profile = strcmp(profile, "sfx") == 0;
	latency_us = sfx_profile ? 15000U : 50000U;

	if (strcmp(profile, "anchor") == 0)
		return run_anchor(device);
	if (strcmp(profile, "sfx-server") == 0)
		return run_sfx_server(device);

	if (volume < 0 || volume > 100)
		return 2;

	fp = fopen(path, "rb");
	if (!fp) {
		perror("fopen");
		goto out;
	}

	if (parse_wav(fp, &info) != 0 ||
	    info.bits != 16 ||
	    (info.channels != 1 && info.channels != 2) ||
	    info.rate < 8000 || info.rate > 192000) {
		fprintf(stderr, "unsupported WAV (need PCM S16_LE mono/stereo)\n");
		goto out;
	}

	skip_frames = ((unsigned long long)info.rate * skip_ms) / 1000ULL;
	skip_bytes = skip_frames * info.channels * 2ULL;
	if (skip_bytes >= info.data_size)
		goto out_ok;

	if (fseek(fp, info.data_offset + (long)skip_bytes, SEEK_SET) != 0)
		goto out;

	remaining = info.data_size - (uint32_t)skip_bytes;
	total_frames = remaining / (info.channels * 2U);
	fade_frames = sfx_profile ? ((unsigned long long)info.rate * 4ULL) / 1000ULL : 0;
	if (fade_frames * 2ULL > total_frames)
		fade_frames = total_frames / 2ULL;

	if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		fprintf(stderr, "cannot open PCM %s\n", device);
		goto out;
	}

	if (snd_pcm_set_params(
		    pcm,
		    SND_PCM_FORMAT_S16_LE,
		    SND_PCM_ACCESS_RW_INTERLEAVED,
		    info.channels,
		    info.rate,
		    1,
		    latency_us) < 0) {
		fprintf(stderr, "cannot configure PCM %s\n", device);
		goto out;
	}

	while (remaining > 0) {
		size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		size_t got = fread(buffer, 1, want, fp);
		size_t samples;
		size_t i;
		size_t frame_bytes;
		snd_pcm_sframes_t frames;
		unsigned char *p;

		if (got == 0)
			break;

		samples = got / 2;
		for (i = 0; i < samples; i++) {
			unsigned long long frame =
				played_frames + i / info.channels;
			unsigned int envelope = 1000U;
			int16_t sample;
			long long scaled;

			if (fade_frames > 0 && frame < fade_frames)
				envelope = (unsigned int)((frame * 1000ULL) / fade_frames);
			if (fade_frames > 0 &&
			    frame + fade_frames >= total_frames) {
				unsigned long long left = total_frames > frame
					? total_frames - frame - 1ULL : 0ULL;
				unsigned int tail =
					(unsigned int)((left * 1000ULL) / fade_frames);
				if (tail < envelope)
					envelope = tail;
			}

			memcpy(&sample, buffer + i * 2, sizeof(sample));
			scaled = ((long long)sample * volume * envelope) / 100000LL;
			if (scaled > 32767)
				scaled = 32767;
			if (scaled < -32768)
				scaled = -32768;
			sample = (int16_t)scaled;
			memcpy(buffer + i * 2, &sample, sizeof(sample));
		}

		frame_bytes = info.channels * 2U;
		p = buffer;
		frames = (snd_pcm_sframes_t)(got / frame_bytes);

		while (frames > 0) {
			snd_pcm_sframes_t written =
				snd_pcm_writei(pcm, p, (snd_pcm_uframes_t)frames);

			if (written == -EPIPE) {
				snd_pcm_prepare(pcm);
				continue;
			}

			if (written < 0)
				written = snd_pcm_recover(pcm, (int)written, 1);

			if (written < 0)
				goto out;

			p += (size_t)written * frame_bytes;
			frames -= written;
		}

		played_frames += got / frame_bytes;
		remaining -= (uint32_t)got;
	}

	snd_pcm_drain(pcm);

out_ok:
	rc = 0;
out:
	if (pcm)
		snd_pcm_close(pcm);
	if (fp)
		fclose(fp);
	return rc;
}
