#!/bin/sh
# Regenerate the bundled nuubOS profile pictures (package/nuubos/nuubos-users/src/avatars).
# Source: DiceBear 9.x styles big-smile, fun-emoji, open-peeps (see avatars/README.txt for licenses).
# The SVG is rendered by the DiceBear HTTP API from a fixed seed (round, radius=50) and rasterized locally to 192x192 PNG.
# Usage: generate.sh [output-dir]. Requires: curl, rsvg-convert. Development tool only; the build uses the committed PNGs.
out="${1:-$(cd "$(dirname "$0")/../.." && pwd)/package/nuubos/nuubos-users/src/avatars}"
api="https://api.dicebear.com/9.x"
bg="b6e3f4,c0aede,d1d4f9,ffd5dc,ffdfbf"
tmp="$(mktemp -d)" || { echo "[FAIL] mktemp"; exit 1; }
trap 'rm -rf "$tmp"' EXIT
n=0
for entry in \
	big-smile:Felix big-smile:Aneka big-smile:Mimi big-smile:Sasha \
	big-smile:Nova big-smile:Pixel big-smile:Kiwi big-smile:Luna \
	fun-emoji:Felix fun-emoji:Mimi fun-emoji:Sasha fun-emoji:Lilly \
	fun-emoji:Leo fun-emoji:Rocket fun-emoji:Juno fun-emoji:Otto \
	open-peeps:Felix open-peeps:Aneka open-peeps:Lilly open-peeps:Nova \
	open-peeps:Pixel open-peeps:Mochi open-peeps:Kiwi open-peeps:Otto; do
	n=$((n + 1))
	id=$(printf '%02d' "$n")
	style=${entry%%:*}
	seed=${entry#*:}
	if ! curl -sfL "$api/$style/svg?seed=$seed&backgroundColor=$bg&radius=50" -o "$tmp/$id.svg"; then
		echo "[FAIL] download $style seed=$seed"; exit 1
	fi
	if ! rsvg-convert -w 192 -h 192 "$tmp/$id.svg" -o "$out/$id.png"; then
		echo "[FAIL] rasterize $style seed=$seed"; exit 1
	fi
	echo "[PASS] $id.png <- $style seed=$seed"
done
