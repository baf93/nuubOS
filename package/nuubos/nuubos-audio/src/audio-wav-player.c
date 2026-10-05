/* SPDX-License-Identifier: MIT */
#include <alsa/asoundlib.h>
#include <errno.h>
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
