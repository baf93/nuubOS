#!/bin/sh
# Regenerate the bundled nuubOS profile pictures (package/nuubos/nuubos-users/src/avatars).
# Source: DiceBear 10.x styles voxel-art, voxel-bot (all CC0 1.0; see avatars/README.txt).
# The SVG is rendered by the DiceBear HTTP API from a fixed seed on a retro-palette square
# background and rasterized locally to 256x256 PNG; the UI rounds the corners.
# Usage: generate.sh [output-dir]. Requires: curl, rsvg-convert. Development tool only; the build uses the committed PNGs.
out="${1:-$(cd "$(dirname "$0")/../.." && pwd)/package/nuubos/nuubos-users/src/avatars}"
api="https://api.dicebear.com/10.x"
bg="1d2b53,7e2553,008751,ab5236,29adff,ff77a8,ffa300,5f574f"
tmp="$(mktemp -d)" || { echo "[FAIL] mktemp"; exit 1; }
trap 'rm -rf "$tmp"' EXIT
n=0
for entry in \
	voxel-art:Felix voxel-art:Mimi voxel-art:Sasha voxel-art:Lilly voxel-art:Leo \
	voxel-art:Rocket voxel-art:Juno voxel-art:Otto voxel-art:Aneka voxel-art:Kiwi \
	voxel-bot:Felix voxel-bot:Aneka voxel-bot:Lilly voxel-bot:Nova voxel-bot:Pixel \
	voxel-bot:Mochi voxel-bot:Kiwi voxel-bot:Otto voxel-bot:Juno voxel-bot:Sasha; do
	n=$((n + 1))
	id=$(printf '%02d' "$n")
	style=${entry%%:*}
	seed=${entry#*:}
	if ! curl -sfL "$api/$style/svg?seed=$seed&backgroundColor=$bg" -o "$tmp/$id.svg"; then
		echo "[FAIL] download $style seed=$seed"; exit 1
	fi
	if ! rsvg-convert -w 256 -h 256 "$tmp/$id.svg" -o "$out/$id.png"; then
		echo "[FAIL] rasterize $style seed=$seed"; exit 1
	fi
	echo "[PASS] $id.png <- $style seed=$seed"
done
