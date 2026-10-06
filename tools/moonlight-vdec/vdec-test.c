/* SPDX-License-Identifier: MIT */
/* Dev harness (not shipped): drives moonlight's Wayland/V4L2 request
 * renderer with an Annex B file split on access unit delimiters, paced at
 * a fixed rate, and prints the renderer statistics. */
#define _GNU_SOURCE
#include <Limelight.h>
#include "video/wayland.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

pthread_t main_thread_id;
int LiGetPendingVideoFrames(void) { return 0; }

static int nal_type(const unsigned char *p, int hevc) {
  return hevc ? (p[0] >> 1) & 0x3f : p[0] & 0x1f;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s file.h264|h265 width height fps [hevc]\n", argv[0]);
    return 2;
  }
  int hevc = argc > 5;
  int width = atoi(argv[2]), height = atoi(argv[3]), fps = atoi(argv[4]);
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror("open"); return 1; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *data = malloc(size);
  if (fread(data, 1, size, f) != (size_t) size) { perror("read"); return 1; }
  fclose(f);

  main_thread_id = pthread_self();
  if (decoder_callbacks_wayland.setup(hevc ? VIDEO_FORMAT_H265 : VIDEO_FORMAT_H264, width, height, fps, NULL, 0) != 0) {
    fprintf(stderr, "setup failed\n");
    return 1;
  }

  /* Access units start at each AUD (H.264 type 9, HEVC type 35). */
  long starts[100000];
  int count = 0;
  for (long i = 0; i + 4 < size && count < 100000; i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      int t = nal_type(&data[i + 3], hevc);
      if (t == (hevc ? 35 : 9))
        starts[count++] = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
    }
  }
  printf("%d access units\n", count);

  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  int needIdr = 0;
  for (int n = 0; n < count; n++) {
    long end = n + 1 < count ? starts[n + 1] : size;
    LENTRY entry = { .next = NULL, .data = (char *) data + starts[n], .length = (int) (end - starts[n]), .bufferType = BUFFER_TYPE_PICDATA };
    DECODE_UNIT du = { 0 };
    du.frameNumber = n;
    du.frameType = n == 0 ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME;
    du.fullLength = entry.length;
    du.bufferList = &entry;
    if (decoder_callbacks_wayland.submitDecodeUnit(&du) != DR_OK)
      needIdr++;
    next.tv_nsec += 1000000000L / fps;
    if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
  }

  struct wayland_video_stats s;
  wayland_video_stats(&s);
  printf("decoded=%llu presented=%llu skipped=%llu errors=%llu need_idr=%d avg_decode=%.2f ms\n",
         (unsigned long long) s.decoded, (unsigned long long) s.presented, (unsigned long long) s.skipped,
         (unsigned long long) s.decode_errors, needIdr, s.decoded ? s.decode_us_total / 1000.0 / s.decoded : 0.0);
  decoder_callbacks_wayland.cleanup();
  return 0;
}
