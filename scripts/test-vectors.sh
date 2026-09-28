#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${NUBOS_TEST_VECTOR_DIR:-$ROOT/output/test-vectors}"

mkdir -p "$OUT"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg is required on the host." >&2
    exit 1
fi

make_h264() {
    local name="$1" size="$2" rate="$3" frames="$4" profile="$5" level="$6" bf="$7" gop="$8"
    local dst="$OUT/$name"
    [[ -f "$dst" ]] && return 0

    echo "Generating $name"
    ffmpeg -hide_banner -loglevel error -y \
      -f lavfi -i "testsrc2=size=$size:rate=$rate" \
      -frames:v "$frames" -an \
      -c:v libx264 -threads 1 -preset veryfast \
      -profile:v "$profile" -level:v "$level" \
      -pix_fmt yuv420p -bf "$bf" -g "$gop" \
      -keyint_min "$gop" -sc_threshold 0 \
      -x264-params 'repeat-headers=1:annexb=1' \
      -f h264 "$dst"
}

make_hevc() {
    local name="$1" size="$2" rate="$3" frames="$4" profile="$5" pixfmt="$6" gop="$7"
    local dst="$OUT/$name"
    [[ -f "$dst" ]] && return 0

    echo "Generating $name"
    ffmpeg -hide_banner -loglevel error -y \
      -f lavfi -i "testsrc2=size=$size:rate=$rate" \
      -frames:v "$frames" -an \
      -c:v libx265 -threads 1 -preset veryfast \
      -profile:v "$profile" -pix_fmt "$pixfmt" \
      -x265-params "log-level=error:repeat-headers=1:keyint=$gop:min-keyint=$gop:scenecut=0" \
      -f hevc "$dst"
}

generate_one() {
    case "$1" in
        h264-baseline-320x240-30-2s.h264)
            make_h264 "$1" 320x240 30 60 baseline 3.0 0 30 ;;
        h264-high-720p30-10s.h264)
            make_h264 "$1" 1280x720 30 300 high 3.1 3 60 ;;
        h264-high-1080p30-10s.h264)
            make_h264 "$1" 1920x1080 30 300 high 4.0 3 60 ;;
        h264-high-1080p60-10s.h264)
            make_h264 "$1" 1920x1080 60 600 high 4.2 3 120 ;;
        hevc-main-720p30-10s.hevc)
            make_hevc "$1" 1280x720 30 300 main yuv420p 60 ;;
        hevc-main10-720p30-10s.hevc)
            make_hevc "$1" 1280x720 30 300 main10 yuv420p10le 60 ;;
        hevc-main-1080p30-10s.hevc)
            make_hevc "$1" 1920x1080 30 300 main yuv420p 60 ;;
        hevc-main10-1080p30-10s.hevc)
            make_hevc "$1" 1920x1080 30 300 main10 yuv420p10le 60 ;;
        hevc-main-1080p60-10s.hevc)
            make_hevc "$1" 1920x1080 60 600 main yuv420p 120 ;;
        hevc-main10-1080p60-10s.hevc)
            make_hevc "$1" 1920x1080 60 600 main10 yuv420p10le 120 ;;
        *)
            echo "Unknown test vector: $1" >&2
            return 2 ;;
    esac
}

ALL=(
    h264-baseline-320x240-30-2s.h264
    h264-high-720p30-10s.h264
    h264-high-1080p30-10s.h264
    h264-high-1080p60-10s.h264
    hevc-main-720p30-10s.hevc
    hevc-main10-720p30-10s.hevc
    hevc-main-1080p30-10s.hevc
    hevc-main10-1080p30-10s.hevc
    hevc-main-1080p60-10s.hevc
    hevc-main10-1080p60-10s.hevc
)

if (($#)); then
    SELECTED=("$@")
else
    SELECTED=("${ALL[@]}")
fi

for name in "${SELECTED[@]}"; do
    generate_one "$name"
done

{
    echo "# nuubOS generated regression vectors"
    echo "# ffmpeg: $(ffmpeg -version | head -1)"
    echo
    find "$OUT" -maxdepth 1 \( -name '*.h264' -o -name '*.hevc' \) -type f -print0 \
      | sort -z \
      | xargs -0 -r sha256sum
} > "$OUT/SHA256SUMS"

echo
echo "Available vectors:"
ls -lh "$OUT"
