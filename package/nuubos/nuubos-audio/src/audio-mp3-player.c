/* SPDX-License-Identifier: MIT */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <mpg123.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int clamp_sample(int value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return value;
}

/*
 * Live volume control: audiod may connect stdin to a pipe and write one
 * decimal volume (0-100) per line. The newest complete value wins. EOF or
 * a non-pipe stdin simply disables live updates.
 */
static int poll_volume(int *live, int current)
{
    static char pending[32];
    static size_t used;
    char chunk[64];
    ssize_t n;
    int value = current;

    if (!*live)
        return current;

    for (;;) {
        n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            *live = 0;
            break;
        }
        if (n < 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                pending[used] = '\0';
                if (used > 0) {
                    value = atoi(pending);
                    if (value < 0) value = 0;
                    if (value > 100) value = 100;
                }
                used = 0;
            } else if (used + 1 < sizeof(pending)) {
                pending[used++] = chunk[i];
            }
        }
    }
    return value;
}

int main(int argc, char **argv)
{
    mpg123_handle *mh = NULL;
    snd_pcm_t *pcm = NULL;
    unsigned char *buffer = NULL;
    size_t buffer_size, done = 0;
    long rate = 0;
    int channels = 0, encoding = 0, err = MPG123_OK;
    int volume, target, live, rc = 1;
    int stdin_flags;
    unsigned long long skip_ms;

    if (argc != 6) {
        fprintf(stderr, "usage: %s <alsa-device> <volume> <skip-ms> <profile> <file>\n", argv[0]);
        return 2;
    }
    volume = atoi(argv[2]);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    target = volume;
    /* Only a private control pipe is switched to non-blocking; never touch
     * an inherited terminal or file description. */
    struct stat st;
    stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    live = fstat(STDIN_FILENO, &st) == 0 && S_ISFIFO(st.st_mode) &&
           stdin_flags >= 0 &&
           fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK) == 0;
    skip_ms = strtoull(argv[3], NULL, 10);
    (void)argv[4];

    if (mpg123_init() != MPG123_OK) return 1;
    mh = mpg123_new(NULL, &err);
    if (!mh) goto out;
    if (mpg123_open(mh, argv[5]) != MPG123_OK) goto out;
    if (mpg123_getformat(mh, &rate, &channels, &encoding) != MPG123_OK) goto out;
    if (rate <= 0 || (channels != 1 && channels != 2)) goto out;

    mpg123_format_none(mh);
    if (mpg123_format(mh, rate, channels, MPG123_ENC_SIGNED_16) != MPG123_OK) goto out;
    if (skip_ms > 0) {
        off_t target = (off_t)((skip_ms * (unsigned long long)rate) / 1000ULL);
        if (mpg123_seek(mh, target, SEEK_SET) < 0) goto out;
    }

    if (snd_pcm_open(&pcm, argv[1], SND_PCM_STREAM_PLAYBACK, 0) < 0) goto out;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           (unsigned int)channels, (unsigned int)rate,
                           1, 80000) < 0) goto out;

    buffer_size = mpg123_outblock(mh);
    if (buffer_size < 4096) buffer_size = 4096;
    buffer = malloc(buffer_size);
    if (!buffer) goto out;

    while ((err = mpg123_read(mh, buffer, buffer_size, &done)) == MPG123_OK || err == MPG123_DONE) {
        if (done > 0) {
            int16_t *samples = (int16_t *)buffer;
            size_t count = done / sizeof(*samples);
            size_t frames = count / (size_t)channels;
            size_t i;
            snd_pcm_sframes_t written;

            target = poll_volume(&live, target);
            if (target != volume && frames > 0) {
                /* Linear gain ramp across this block: no zipper noise or
                 * click on volume steps, no stream restart. */
                for (i = 0; i < frames; i++) {
                    int g = volume * 1024 + (int)(((long)(target - volume) * 1024 * (long)(i + 1)) / (long)frames);
                    for (int c = 0; c < channels; c++) {
                        size_t k = i * (size_t)channels + (size_t)c;
                        samples[k] = (int16_t)clamp_sample(((int)samples[k] * g) / (100 * 1024));
                    }
                }
                volume = target;
            } else if (volume != 100) {
                for (i = 0; i < count; i++)
                    samples[i] = (int16_t)clamp_sample(((int)samples[i] * volume) / 100);
            }

            while (frames > 0) {
                written = snd_pcm_writei(pcm, samples, frames);
                if (written == -EPIPE) {
                    if (snd_pcm_prepare(pcm) < 0) goto out;
                    continue;
                }
                if (written < 0) {
                    written = snd_pcm_recover(pcm, (int)written, 1);
                    if (written < 0) goto out;
                    continue;
                }
                samples += (size_t)written * (size_t)channels;
                frames -= (size_t)written;
            }
        }
        if (err == MPG123_DONE) break;
    }
    if (err != MPG123_DONE) goto out;
    snd_pcm_drain(pcm);
    rc = 0;
out:
    free(buffer);
    if (pcm) snd_pcm_close(pcm);
    if (mh) { mpg123_close(mh); mpg123_delete(mh); }
    mpg123_exit();
    return rc;
}
