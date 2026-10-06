# PC game streaming packages (EPIC-025)

Moonlight is the nuubOS PC streaming client (NVIDIA GameStream / Sunshine
hosts). `nuubos-streamd` (`package/nuubos/nuubos-streaming`) owns the user's
PCs, pairing, host applications, stream settings and the running stream; the
nuubUI Moonlight application (Home → Applications) and the Quick Menu STREAM
section are its clients.

## Why moonlight-embedded

moonlight-embedded is the small C client (no Qt, no SDL). Its event loop
sleeps in `poll()`, and its platform layer is a set of decoder/renderer
callbacks, so an H700 platform is one file. moonlight-qt would bring Qt 6
Quick and its own non-nuubOS interface for the same protocol library
(moonlight-common-c).

## H700 video path (patch 0002)

```
Sunshine ─RTP─▶ moonlight-common-c ─Annex B AU─▶ FFmpeg h264/hevc decoder
   ─V4L2 request API (FFmpeg patches/packages/ffmpeg)─▶ Cedrus VPU
   ─NV12 capture dma-buf (AV_PIX_FMT_DRM_PRIME)─▶ zwp_linux_dmabuf_v1 wl_buffer
   ─desync subsurface + wp_viewporter─▶ labwc (Panfrost samples the dma-buf)
```

- Zero CPU copies or colour conversions; no GL context in the client. Each
  capture buffer is wrapped once in a `wl_buffer` (cache keyed by dma-buf
  inode); a decoded frame stays referenced until the compositor releases it.
- Frames are shown as soon as they are decoded; a frame is not presented when
  newer ones are already queued (catch-up after a network burst).
- Letterboxing with a one-pixel black parent surface stretched by
  `wp_viewporter`; the stream is fitted with `wp_viewporter`, so HDMI
  fractional scaling maps the decoded frame 1:1 to physical pixels.
- Resolution "Automatic" streams at the active screen's mode (internal panel,
  or the HDMI preferred mode when connected), capped at 1080p.

Measured on RG40XX-V (Cedrus at 600 MHz, `tools/moonlight-vdec`, 900 frames
at 60 fps, all presented, no errors): H.264 640x480 1.6 ms/frame (3.1 ms with
multiple slices), H.264 1080p 5.9 ms, HEVC 640x480 1.1 ms, HEVC 1080p 4.7 ms;
whole system 6-9 % of four cores including labwc composition.

## Other patches

- 0001: avahi optional (streamd discovers hosts with a one-shot mDNS query,
  no avahi daemon).
- 0003: ALSA 10 ms periods / 40 ms buffer (audio delay bounded), `drop`
  instead of `drain` at the end (PipeWire's ALSA plugin).
- 0004: `-gamepad <name>`: only the nuubOS game gamepads of nuubos-inputd
  are read, each as the player given by its product id; `-noquitcombo`.
- 0005: `-control`: `@event` lines on stdout and `STATS`/`QUIT` on stdin
  for streamd; `status` action; a failed connection ends the process.

## FFmpeg

`patches/packages/ffmpeg/6.1.5/` is Jonas Karlman's V4L2 request API hwaccel
series (branch `v4l2-request-n6.1.1` of github.com/Kwiboo/FFmpeg, as used by
LibreELEC), applied unchanged to FFmpeg 6.1.5. The defconfig builds only the
h264/hevc decoders and parsers with the h264/hevc V4L2 request hwaccels (no
programs, formats, filters, encoders or swscale; ~2 MB libavcodec).

## Licences

| Component | Licence | Notes |
|---|---|---|
| moonlight-embedded 2.7.1 + nuubOS patches | GPL-3.0+ | runs as a separate process; nuubos-streamd (MIT) never links it |
| moonlight-common-c (bundled) | GPL-3.0+ | |
| enet (bundled in moonlight-common-c) | MIT | |
| h264bitstream (bundled) | LGPL-2.1+ | |
| SDL_GameControllerDB (bundled, installed) | Zlib | |
| FFmpeg 6.1.5 + V4L2 request patches | LGPL-2.1+ | built without GPL components |
| Opus | BSD-3-Clause | |
| libcurl | curl (MIT-style) | |
| Moonlight application icon | MIT (nuubOS original artwork, `nuubos-streaming/src/app/moonlight.svg`) | the Moonlight project logo is not shipped |

GPL-3.0 redistribution: nuubOS ships the corresponding source through
Buildroot (`make legal-info`) and does not restrict modified software on the
device.
