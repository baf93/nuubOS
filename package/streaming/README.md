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

FFmpeg 7.1.5 (Buildroot bump in `patches/buildroot/0002-ffmpeg-7.1.5-*.patch`:
version, hash, no `--disable-crystalhd`, Buildroot patches that are upstream
or MIPS/x86/MMAL-only dropped) with `patches/packages/ffmpeg/7.1.5/`:

- 0001-0014: Jonas Karlman's V4L2 request API hwaccel series, branch
  `v4l2-request-n7.1.3` of github.com/Kwiboo/FFmpeg, unchanged. This series
  has its own hwdevice type (`AV_HWDEVICE_TYPE_V4L2REQUEST`).
- 0015 (nuubOS): a client that passes a DRM device for DRM_PRIME output (the
  earlier series' interface) gets a V4L2 request device instead. Both
  moonlight-embedded and the Valve Steam Link application do that.
- 0016: the earlier series' "do not require drm device" hack (a DRM device
  created without a path), needed by the same clients.

FFmpeg 7.1 is required by Steam Link (libavcodec.so.61). Moonlight was
re-qualified on it with `tools/moonlight-vdec` (RG40XX-V, 900 frames, 0
errors: H.264 640x480 1.63 ms/frame, HEVC 1280x720 2.03 ms). The defconfig builds only the
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
| FFmpeg 7.1.5 + V4L2 request patches | LGPL-2.1+ | built without GPL components |
| Opus | BSD-3-Clause | |
| libcurl | curl (MIT-style) | |
| Moonlight application icon | MIT (nuubOS original artwork, `nuubos-streaming/src/app/moonlight.svg`) | the Moonlight project logo is not shipped |

GPL-3.0 redistribution: nuubOS ships the corresponding source through
Buildroot (`make legal-info`) and does not restrict modified software on the
device.

# Steam Link (EPIC-026)

Steam Link is Valve's proprietary remote play client. There is no open
implementation of Steam's remote play protocol, so nuubOS runs Valve's own
application, and never ships it.

## What is in the image, what is not

- **Not in the image:** the Steam Link application. On the user's request
  (Home → Applications → Steam Link → Download) `nuubos-steamlink-get`
  reads Valve's public build pointer
  (`https://media.steampowered.com/steamlink/rpi/trixie/arm64/public_build.txt`,
  the same one Valve's Raspberry Pi launcher uses) and downloads that arm64
  build over HTTPS (system CA bundle). The page tells the user that the
  download means accepting the Steam Subscriber Agreement (Valve's
  `LICENSE.txt`). It is extracted while it downloads into
  `/userdata/steamlink/app` (shared by all users; exFAT, so the archive's
  symbolic links become copies, ~110 MB), then swapped in atomically.
- **In the image (`steamlink-runtime`):** only open source pieces the
  application needs and does not bundle: FFmpeg 7.1 (above), libepoxy,
  double-conversion, md4c (`package/streaming/md4c`), zstd, the Kerberos
  GSSAPI library its Qt links, and the Qt 5.14.1 Wayland platform plugin.

## Display: Qt Wayland plugin for Valve's Qt

Valve bundles Qt 5.14.1 with the xcb, eglfs, linuxfb and vnc platforms only
(on Raspberry Pi OS its interface runs on XWayland). nuubOS has no X server,
so `steamlink-runtime` builds `libQt5WaylandClient` and the `wayland-egl`
platform, `wayland-egl` client buffer integration and `xdg-shell` plugins
from the same Qt release. qtbase 5.14.1 is configured like Valve's build
(OpenGL ES 2, EGL, Vulkan, D-Bus, GLib; the Vulkan feature changes
`QPlatformIntegration`'s vtable, so it must match) and used only as a build
SDK; nothing else from it is installed. At run time the plugins load
Valve's own Qt libraries (`QT_PLUGIN_PATH` lists ours first). qtbase needs
one GCC 15 fix (`steamlink-runtime/qtbase/0001`).

The stream itself is not drawn by Qt: Steam Link's SDL3 video path decodes
with libavcodec and shows NV12 dma-bufs on a Wayland subsurface
(`zwp_linux_dmabuf_v1` + `wp_viewporter`), the same model as Moonlight's
nuubOS platform. It asks libavcodec for a hw config with DRM_PRIME output and
creates a DRM device, which FFmpeg patch 0015 maps to the V4L2 request
hwaccel (Cedrus).

## Session

`nuubos-streamd` runs it like a Moonlight stream (`stream_client=steamlink`):
game gamepads on for its lifetime (SDL3 reads `nuubOS Gamepad` with the same
SDL mapping as Moonlight), Home hidden and Home Music stopped, Quick Menu
section STEAM LINK (Resume, Quit Steam Link), Sleep/Restart/Power Off end it
first. Valve's interface handles the PCs, pairing (PIN shown on the
handheld, entered in Steam on the PC) and streams; leaving it from its own
menu returns to nuubUI. Per-user data (pairing, settings):
`/userdata/users/<id>/appdata/steamlink` (`HOME`). Interface language from
the user's nuubOS language (`--locale`). Started with `--skip-update`:
updates go through nuubOS (Check for Updates / Update to x.y).

## Licences

| Component | Licence | Notes |
|---|---|---|
| Valve Steam Link application | Proprietary (Steam Subscriber Agreement) | **not distributed**: downloaded from Valve by the user on the device |
| Qt Wayland 5.14.1 (libQt5WaylandClient + 3 plugins) | LGPL-3.0 (or GPL-2.0+/GPL-3.0) | shared libraries, replaceable; source via `make legal-info` |
| qtbase 5.14.1 | LGPL-3.0 | build-time SDK only, nothing installed |
| Vulkan-Headers 1.2.131 | Apache-2.0 | build time only |
| md4c 0.5.2 | MIT | |
| libepoxy, double-conversion, zstd, libkrb5, ca-certificates | MIT / BSD-3-Clause / BSD-3-Clause or GPL-2.0 / MIT-style / MPL-2.0 | Buildroot packages |
| Steam Link application icon | MIT (nuubOS original artwork, `nuubos-streaming/src/app/steamlink.svg`) | Valve's Steam and Steam Link logos are not used |
