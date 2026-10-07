#!/bin/sh
set -eu

TARGET_DIR="$1"
INITTAB="$TARGET_DIR/etc/inittab"

#
# nuubOS SYSTEM carries no persistent per-device/user state by policy.
#
# During development the filesystem may remain writable, but persistent
# configuration belongs in STATE and must never be embedded in SYSTEM.
#

mkdir -p \
    "$TARGET_DIR/etc/network" \
    "$TARGET_DIR/etc/wpa_supplicant" \
    "$TARGET_DIR/root/.ssh"

chmod 700 "$TARGET_DIR/root/.ssh"

#
# Production-safe networking baseline.
# Wi-Fi is enabled only after STATE has been provisioned.
#
cat > "$TARGET_DIR/etc/network/interfaces" <<'NETWORK_EOF'
auto lo
iface lo inet loopback
NETWORK_EOF

#
# Never ship credentials or user authorization in SYSTEM.
#
rm -f \
    "$TARGET_DIR/etc/wpa_supplicant/wpa_supplicant.conf" \
    "$TARGET_DIR/root/.ssh/authorized_keys" \
    "$TARGET_DIR/run/resolv.conf"

#
# Product release boot keeps tty1 free of getty output.
if [ -f "$INITTAB" ]; then
    sed -i '/^tty1::respawn:/d' "$INITTAB"
fi

echo "nuubOS rootfs configured with persistent state externalized"

#
# Product identity (EPIC-048 update checks, EPIC-051/052 build identifiers).
#
NUUBOS_TOP="${BR2_EXTERNAL_NUUBOS_PATH:-$(dirname "$0")/../../..}"
NUUBOS_VERSION="$(cat "$NUUBOS_TOP/VERSION" 2>/dev/null || echo 0)"
NUUBOS_BUILD="$(git -C "$NUUBOS_TOP" describe --always --dirty --abbrev=12 2>/dev/null || echo unknown)"
# Buildroot's /etc/os-release is a link to /usr/lib/os-release: replace both.
rm -f "$TARGET_DIR/etc/os-release" "$TARGET_DIR/usr/lib/os-release"
cat > "$TARGET_DIR/etc/os-release" <<OS_RELEASE_EOF
NAME=nuubOS
ID=nuubos
VERSION_ID=$NUUBOS_VERSION
BUILD_ID=$NUUBOS_BUILD
PRETTY_NAME="nuubOS $NUUBOS_VERSION"
HOME_URL="https://github.com/baf93/nuubOS"
OS_RELEASE_EOF
ln -s ../../etc/os-release "$TARGET_DIR/usr/lib/os-release"
mkdir -p "$TARGET_DIR/usr/share/nuubos"
printf '%s\n' "$NUUBOS_BUILD" > "$TARGET_DIR/usr/share/nuubos/build-id"
