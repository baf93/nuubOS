#!/usr/bin/env bash
#
# Screenshot of what is really on the target's screen(s) right now.
#
# Builds tools/drm-grab (dev-only, never in the image) with the Buildroot
# toolchain if needed, copies it to /tmp on the target, reads the scanout
# framebuffer of every active CRTC and writes output/grab/<name>-crtcN.png.
#
# Usage: tools/grab-target.sh [name] [-H root@host]

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="shot"
HOST="root@192.168.5.36"
while [ "$#" -gt 0 ]; do
    case "$1" in
        -H) HOST="$2"; shift 2 ;;
        *) NAME="$1"; shift ;;
    esac
done

BIN="$ROOT/output/drm-grab"
SYSROOT=output/h700/host/aarch64-buildroot-linux-gnu/sysroot
if [ ! -x "$BIN" ] || [ "$ROOT/tools/drm-grab/drm-grab.c" -nt "$BIN" ]; then
    docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/workspace" -w /workspace nuubos-dev:0.5 \
        sh -c "output/h700/host/bin/aarch64-buildroot-linux-gnu-gcc -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -I$SYSROOT/usr/include/libdrm tools/drm-grab/drm-grab.c -ldrm -o output/drm-grab" \
        || { echo "[FAIL] drm-grab build"; exit 1; }
fi

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST")
scp -q "$BIN" "$HOST:/tmp/drm-grab" || { echo "[FAIL] target unreachable"; exit 1; }
"${SSH[@]}" "rm -f /tmp/grab-*.ppm; /tmp/drm-grab /dev/dri/card0 /tmp/grab" || { echo "[FAIL] grab"; exit 1; }

mkdir -p "$ROOT/output/grab"
for REMOTE in $("${SSH[@]}" 'ls /tmp/grab-*.ppm'); do
    CRTC="$(basename "$REMOTE" .ppm | sed 's/^grab-//')"
    LOCAL="$ROOT/output/grab/$NAME-$CRTC"
    scp -q "$HOST:$REMOTE" "$LOCAL.ppm" || continue
    python3 - "$LOCAL.ppm" "$LOCAL.png" <<'EOF'
import struct, sys, zlib
data = open(sys.argv[1], 'rb').read()
magic, dims, maxval, pixels = data.split(b'\n', 3)
w, h = map(int, dims.split())
raw = b''.join(b'\x00' + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
with open(sys.argv[2], 'wb') as f:
    f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
EOF
    rm -f "$LOCAL.ppm"
    echo "[PASS] $LOCAL.png"
done
