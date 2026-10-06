#!/usr/bin/env bash
#
# Qualifies the Moonlight video path on the development device without a
# streaming host: the real moonlight-embedded Wayland/V4L2 request renderer
# (output/h700/build/moonlight-embedded-*/src/video/wayland.c) decodes an
# Annex B file split on access unit delimiters, paced at a fixed rate, and
# presents it fullscreen; decode statistics are printed at the end.
# Development aid only, never shipped.
#
# Usage: tools/moonlight-vdec/run.sh [-H root@host] WIDTHxHEIGHT [fps] [h264|hevc]
#   Builds the harness, encodes a 15 s testsrc2 clip on the host (ffmpeg with
#   libx264/libx265, no B-frames, AUD on, like Sunshine), copies both to
#   /tmp/vtest on the target and runs it there (the screen shows the clip).
#   Grab the screen meanwhile with tools/grab-target.sh to check the image.
#   Never run it while the user is playing or streaming.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST=root@192.168.5.36
if [ "${1:-}" = "-H" ]; then HOST="$2"; shift 2; fi
SIZE="${1:?usage: run.sh [-H host] WIDTHxHEIGHT [fps] [h264|hevc]}"
FPS="${2:-60}"
CODEC="${3:-h264}"
W="${SIZE%x*}"
H="${SIZE#*x}"
OUT="$ROOT/output/moonlight-vdec"
B="$(ls -d "$ROOT"/output/h700/build/moonlight-embedded-* | head -n 1)"
CC="$ROOT/output/h700/host/bin/aarch64-buildroot-linux-gnu-gcc"
mkdir -p "$OUT"

if [ ! -f "$B/src/video/wayland.c" ]; then
    echo "[FAIL] build moonlight-embedded first (compile.sh moonlight-embedded)"
    exit 1
fi
"$CC" -O2 -Wall -DHAVE_WAYLAND -I"$B/src" -I"$B" -I"$B/third_party/moonlight-common-c/src" \
    "$ROOT/tools/moonlight-vdec/vdec-test.c" "$B/src/video/wayland.c" \
    "$B/xdg-shell-protocol.c" "$B/viewporter-protocol.c" "$B/linux-dmabuf-unstable-v1-protocol.c" \
    -o "$OUT/vdec-test" -lavcodec -lavutil -lwayland-client -lpthread || exit 1

CLIP="$OUT/$CODEC-${W}x$H-$FPS.bin"
if [ ! -f "$CLIP" ]; then
    if [ "$CODEC" = hevc ]; then
        ENC=(-c:v libx265 -x265-params aud=1:repeat-headers=1:bframes=0:log-level=error)
    else
        ENC=(-c:v libx264 -profile:v high -bf 0 -x264-params aud=1:repeat-headers=1:sliced-threads=0:slices=1)
    fi
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=$FPS" -t 15 \
        -pix_fmt yuv420p -preset veryfast -tune zerolatency -g 9999 -b:v 10M "${ENC[@]}" \
        -f "$([ "$CODEC" = hevc ] && echo hevc || echo h264)" "$CLIP" || exit 1
fi

ssh "$HOST" 'mkdir -p /tmp/vtest' || exit 1
scp -q "$OUT/vdec-test" "$CLIP" "$HOST:/tmp/vtest/" || exit 1
ssh "$HOST" "cd /tmp/vtest && XDG_RUNTIME_DIR=/run/nuubos/wayland-runtime WAYLAND_DISPLAY=wayland-0 \
    ./vdec-test $(basename "$CLIP") $W $H $FPS $([ "$CODEC" = hevc ] && echo hevc); rm -f $(basename "$CLIP")"
