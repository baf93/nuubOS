#!/bin/sh
# Build and sign a nuubOS OTA release (EPIC-048) on the maintainer's PC.
#
#   tools/ota/make-update.sh OUTDIR CHANNEL [NOTES_FILE]
#
# NOTES_FILE: the release notes shown on the console (Software Update,
# WHAT'S NEW) and on the GitHub release, plain text, one item per line
# ("- " bullets allowed), at most 4000 bytes; signed inside the manifest.
#
# CHANNEL: stable | rc | nightly. Input: output/h700/images/rootfs.ext4
# (the system slot image) and rootfs.tar (its build serial) from a full
# image build. Output in OUTDIR, published by tools/ota/publish.sh:
#   <tag>/nuubos-system-<version>.img.gz       payload (release <tag>)
#   channel-<channel>/nuubos-update.manifest(.sig)
#   release.env, release-notes.txt              what publish.sh needs
# <tag> = v<version>, the version of the image (/etc/os-release, set by
# post-build.sh from VERSION and the image's channel, which must be CHANNEL). The manifest is signed with the release key,
# derived from the release passphrase (tools/ota/release-key.py: asked once
# here, checked against update-key.pub before the long compression).
# NUUBOS_UPDATE_SOURCES: other https hosts with the same layout, space
# separated, tried before GitHub by the consoles that install this release
# (empty: GitHub only).

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-}"; CHANNEL="${2:-}"; NOTES_FILE="${3:-}"
KEYTOOL="$ROOT/tools/ota/release-key.py"
[ -n "$OUT" ] || { echo "Usage: $0 OUTDIR stable|rc|nightly [NOTES_FILE]" >&2; exit 2; }
NOTES=""
if [ -n "$NOTES_FILE" ]; then
    [ -f "$NOTES_FILE" ] || { echo "[FAIL] $NOTES_FILE missing" >&2; exit 1; }
    [ "$(wc -c < "$NOTES_FILE")" -le 4000 ] || { echo "[FAIL] release notes longer than 4000 bytes" >&2; exit 1; }
    # One manifest line: backslash and new lines escaped (\\, \n).
    NOTES="$(sed 's/\\/\\\\/g' "$NOTES_FILE" | awk 'NR > 1 { printf "\\n" } { printf "%s", $0 }')"
fi
case "$CHANNEL" in stable|rc|nightly) ;; *) echo "[FAIL] channel must be stable, rc or nightly" >&2; exit 2 ;; esac
IMAGES="$ROOT/output/h700/images"
IMG="$IMAGES/rootfs.ext4"; TAR="$IMAGES/rootfs.tar"
[ -f "$IMG" ] && [ -f "$TAR" ] || { echo "[FAIL] $IMG or $TAR missing: run a full image build" >&2; exit 1; }
SERIAL="$(tar -xOf "$TAR" ./usr/share/nuubos/build-serial 2>/dev/null | tr -d ' \n')"
[ -n "$SERIAL" ] || { echo "[FAIL] the image has no build serial (/usr/share/nuubos/build-serial)" >&2; exit 1; }
# Fits a system slot (partitions.sh: 4194304 sectors).
IMAGE_SIZE="$(stat -L -c %s "$IMG")"
[ "$IMAGE_SIZE" -le 2147483648 ] || { echo "[FAIL] image larger than a system slot" >&2; exit 1; }
if [ -z "${NUUBOS_RELEASE_PASSPHRASE:-}" ]; then
    printf 'nuubOS release passphrase: ' >&2
    stty -echo 2>/dev/null; read -r NUUBOS_RELEASE_PASSPHRASE; stty echo 2>/dev/null; echo >&2
fi
export NUUBOS_RELEASE_PASSPHRASE
python3 "$KEYTOOL" check || exit 1
VERSION="$(tar -xOf "$TAR" ./etc/os-release 2>/dev/null | sed -n 's/^VERSION_ID=//p' | tr -d '" ')"
IMAGE_CHANNEL="$(tar -xOf "$TAR" ./usr/share/nuubos/release-channel 2>/dev/null | tr -d ' \n')"
[ -n "$VERSION" ] || { echo "[FAIL] the image has no VERSION_ID in /etc/os-release" >&2; exit 1; }
[ "$IMAGE_CHANNEL" = "$CHANNEL" ] || {
    echo "[FAIL] the image was built for the ${IMAGE_CHANNEL:-unknown} channel, not $CHANNEL" >&2; exit 1; }
BUILD="$(tar -xOf "$TAR" ./usr/share/nuubos/build-id 2>/dev/null | tr -d ' \n')"
case "$BUILD" in *-dirty) echo "[FAIL] the image was built from a dirty tree ($BUILD)" >&2; exit 1 ;; esac
TAG="v$VERSION"
PAYLOAD="nuubos-system-$VERSION.img.gz"
mkdir -p "$OUT/$TAG" "$OUT/channel-$CHANNEL" || exit 1
echo "Compressing the system image..."
IMAGE_SHA="$(sha256sum < "$IMG" | cut -d' ' -f1)"
gzip -9 -n -c "$IMG" > "$OUT/$TAG/$PAYLOAD" || exit 1
SHA="$(sha256sum < "$OUT/$TAG/$PAYLOAD" | cut -d' ' -f1)"
SIZE="$(stat -c %s "$OUT/$TAG/$PAYLOAD")"
M="$OUT/channel-$CHANNEL/nuubos-update.manifest"
printf 'format=2\nchannel=%s\nversion=%s\nserial=%s\nbuild=%s\npayload=%s/%s\nsize=%s\nsha256=%s\nimage_size=%s\nimage_sha256=%s\nnotes=%s\nsources=%s\n' \
    "$CHANNEL" "$VERSION" "$SERIAL" "$BUILD" "$TAG" "$PAYLOAD" "$SIZE" "$SHA" "$IMAGE_SIZE" "$IMAGE_SHA" "$NOTES" "${NUUBOS_UPDATE_SOURCES:-}" > "$M"
python3 "$KEYTOOL" sign "$M" "$M.sig" || exit 1
printf 'TAG=%s\nCHANNEL=%s\nVERSION=%s\nPAYLOAD=%s\n' "$TAG" "$CHANNEL" "$VERSION" "$PAYLOAD" > "$OUT/release.env"
if [ -n "$NOTES_FILE" ]; then cp "$NOTES_FILE" "$OUT/release-notes.txt"; else rm -f "$OUT/release-notes.txt"; fi
echo "[PASS] $OUT: $TAG/$PAYLOAD ($SIZE bytes), channel $CHANNEL manifest signed"
