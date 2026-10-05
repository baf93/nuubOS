/* SPDX-License-Identifier: MIT */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long ns_now(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	const char *device = "hw:Codec,0";
	const unsigned int rate = 48000;
	const snd_pcm_uframes_t frames_total = 48000;
	const snd_pcm_uframes_t chunk = 1024;
	snd_pcm_t *pcm = NULL;
	short *buffer = NULL;
	snd_pcm_uframes_t written = 0;
	long long start_ns;
	long long end_ns;
	double elapsed;
	int rc;

	if (argc == 2)
		device = argv[1];
	else if (argc > 2) {
		fprintf(stderr, "Usage: %s [alsa-device]\n", argv[0]);
		return 2;
	}

	rc = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0) {
		fprintf(stderr, "open %s: %s\n", device, snd_strerror(rc));
		return 1;
	}

	rc = snd_pcm_set_params(
		pcm,
		SND_PCM_FORMAT_S16_LE,
		SND_PCM_ACCESS_RW_INTERLEAVED,
		2,
		rate,
		1,
		100000);
	if (rc < 0) {
		fprintf(stderr, "params: %s\n", snd_strerror(rc));
		snd_pcm_close(pcm);
		return 1;
	}

	buffer = calloc((size_t)chunk * 2U, sizeof(short));
	if (!buffer) {
		snd_pcm_close(pcm);
		return 1;
	}

	start_ns = ns_now();
	if (start_ns < 0) {
		free(buffer);
		snd_pcm_close(pcm);
		return 1;
	}

	while (written < frames_total) {
		snd_pcm_uframes_t remaining = frames_total - written;
		snd_pcm_uframes_t request = remaining < chunk ? remaining : chunk;
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, buffer, request);

		if (n == -EPIPE) {
			snd_pcm_prepare(pcm);
			continue;
		}
		if (n < 0) {
			n = snd_pcm_recover(pcm, (int)n, 1);
			if (n < 0) {
				fprintf(stderr, "write: %s\n", snd_strerror((int)n));
				free(buffer);
				snd_pcm_close(pcm);
				return 1;
			}
			continue;
		}
		written += (snd_pcm_uframes_t)n;
	}

	rc = snd_pcm_drain(pcm);
	end_ns = ns_now();

	free(buffer);
	snd_pcm_close(pcm);

	if (rc < 0 || end_ns < start_ns) {
		fprintf(stderr, "drain/timing failed\n");
		return 1;
	}

	elapsed = (double)(end_ns - start_ns) / 1000000000.0;
	printf("device=%s\nrate=%u\nframes=%lu\nelapsed=%.6f\n",
	       device, rate, (unsigned long)frames_total, elapsed);

	/*
	 * One second of PCM plus ALSA startup/drain overhead should remain close
	 * to one second. The historical H700 codec-clock regression was ~2x.
	 */
	if (elapsed < 0.75 || elapsed > 1.35) {
		printf("result=FAIL\n");
		return 3;
	}

	printf("result=PASS\n");
	return 0;
}
