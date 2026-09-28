#!/bin/sh
set -eu

TARGET_DIR="$1"

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
