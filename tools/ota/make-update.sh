#!/bin/sh
# Build and sign a nuubOS OTA release (EPIC-048) on the maintainer's PC.
#
#   tools/ota/make-update.sh PRIVATE_KEY.pem OUTDIR [NOTES_URL]
#
# Input: output/h700/images/rootfs.tar from a full official build
# (BR2_TARGET_ROOTFS_TAR). Output in OUTDIR, to publish as release assets:
#   nuubos-rootfs-<version>.tar.gz, nuubos-update.manifest(.sig)
# One-time key creation (keep the private key offline, never commit it):
#   openssl genpkey -algorithm ed25519 -out nuubos-release.pem
#   openssl pkey -in nuubos-release.pem -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n' \
#       > board/nuubos/common/ota/update-key.pub

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KEY="${1:-}"; OUT="${2:-}"; NOTES="${3:-}"
[ -f "$KEY" ] && [ -n "$OUT" ] || { echo "Usage: $0 PRIVATE_KEY.pem OUTDIR [NOTES_URL]" >&2; exit 2; }
TAR="$ROOT/output/h700/images/rootfs.tar"
[ -f "$TAR" ] || { echo "[FAIL] $TAR missing: run a full build with BR2_TARGET_ROOTFS_TAR" >&2; exit 1; }
VERSION="$(cat "$ROOT/VERSION")"
BUILD="$(git -C "$ROOT" describe --always --dirty --abbrev=12)"
mkdir -p "$OUT" || exit 1
PAYLOAD="nuubos-rootfs-$VERSION.tar.gz"
gzip -9 -c "$TAR" > "$OUT/$PAYLOAD" || exit 1
SHA="$(sha256sum "$OUT/$PAYLOAD" | cut -d' ' -f1)"
SIZE="$(wc -c < "$OUT/$PAYLOAD")"
printf 'format=1\nversion=%s\nbuild=%s\npayload=%s\nsize=%s\nsha256=%s\nnotes=%s\n' \
    "$VERSION" "$BUILD" "$PAYLOAD" "$SIZE" "$SHA" "$NOTES" > "$OUT/nuubos-update.manifest"
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$OUT/nuubos-update.manifest" \
    -out "$OUT/nuubos-update.manifest.sig" || exit 1
echo "[PASS] $OUT: $PAYLOAD ($SIZE bytes), manifest signed"
