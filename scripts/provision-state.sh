#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DEVICE="${1:-${NUBOS_FLASH_DEVICE:-/dev/mmcblk0}}"
LOCAL_CONF="$ROOT/local/dev-network.conf"
SSH_PUBKEY="${NUBOS_SSH_PUBKEY:-$HOME/.ssh/id_ed25519.pub}"

part_dev()
{
    local dev="$1"
    local number="$2"

    case "$dev" in
        *[0-9])
            printf '%sp%s\n' "$dev" "$number"
            ;;
        *)
            printf '%s%s\n' "$dev" "$number"
            ;;
    esac
}

die()
{
    echo "ERROR: $*" >&2
    exit 1
}

STATE_DEV="$(part_dev "$DEVICE" 3)"

[[ -b "$DEVICE" ]] ||
    die "block device missing: $DEVICE"

[[ -b "$STATE_DEV" ]] ||
    die "STATE partition missing: $STATE_DEV"

label="$(
    sudo blkid -s LABEL -o value "$STATE_DEV" 2>/dev/null || true
)"

[[ "$label" == "NUUBOS_STATE" ]] ||
    die "$STATE_DEV is not NUUBOS_STATE"

[[ -f "$LOCAL_CONF" ]] ||
    die "missing $LOCAL_CONF"

[[ -f "$SSH_PUBKEY" ]] ||
    die "missing SSH public key: $SSH_PUBKEY"

# shellcheck disable=SC1090
source "$LOCAL_CONF"

for var in \
    WIFI_SSID WIFI_PSK \
    IP_ADDRESS NETMASK GATEWAY DNS
do
    [[ -n "${!var:-}" ]] ||
        die "$var missing from $LOCAL_CONF"
done

MOUNT_DIR="$(mktemp -d)"

cleanup()
{
    if mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
        sudo umount "$MOUNT_DIR"
    fi

    rmdir "$MOUNT_DIR" 2>/dev/null || true
}

trap cleanup EXIT

sudo mount "$STATE_DEV" "$MOUNT_DIR"

sudo mkdir -p \
    "$MOUNT_DIR/config" \
    "$MOUNT_DIR/network" \
    "$MOUNT_DIR/migrations" \
    "$MOUNT_DIR/ssh/dropbear" \
    "$MOUNT_DIR/ssh/root"

sudo chmod 700 \
    "$MOUNT_DIR/network" \
    "$MOUNT_DIR/ssh" \
    "$MOUNT_DIR/ssh/dropbear" \
    "$MOUNT_DIR/ssh/root"

if [[ ! -f "$MOUNT_DIR/config/nuubos.conf" ]]; then
    sudo tee "$MOUNT_DIR/config/nuubos.conf" >/dev/null <<'EOF2'
NUUBOS_CONFIG_VERSION=5
STORAGE_MODE=AUTO
USER_LOGIN_MODE=AUTO
POWER_MODE=AUTO
AUDIO_OUTPUT=AUTO
AUDIO_VOLUME_SPEAKER=100
AUDIO_VOLUME_HEADPHONES=100
AUDIO_VOLUME_BLUETOOTH=100
DISPLAY_BRIGHTNESS=60
EOF2
fi

sudo chmod 600 "$MOUNT_DIR/config/nuubos.conf"

sudo tee "$MOUNT_DIR/network/interfaces" >/dev/null <<EOF2
auto lo
iface lo inet loopback

auto wlan0
iface wlan0 inet static
    address $IP_ADDRESS
    netmask $NETMASK
    gateway $GATEWAY
    pre-up /usr/sbin/wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
    post-up /usr/sbin/iw dev wlan0 set power_save off
    post-down killall wpa_supplicant 2>/dev/null || true
EOF2

SSID_ESC="$(
    printf '%s' "$WIFI_SSID" |
    sed 's/\\/\\\\/g; s/"/\\"/g'
)"

PSK_ESC="$(
    printf '%s' "$WIFI_PSK" |
    sed 's/\\/\\\\/g; s/"/\\"/g'
)"

sudo tee "$MOUNT_DIR/network/wpa_supplicant.conf" >/dev/null <<EOF2
ctrl_interface=/var/run/wpa_supplicant
update_config=0
country=IT

network={
    ssid="$SSID_ESC"
    psk="$PSK_ESC"
}
EOF2

sudo chmod 600 \
    "$MOUNT_DIR/network/wpa_supplicant.conf"

sudo tee "$MOUNT_DIR/network/dns.conf" >/dev/null <<EOF2
nameserver $DNS
EOF2

sudo chmod 644 "$MOUNT_DIR/network/dns.conf"

sudo cp \
    "$SSH_PUBKEY" \
    "$MOUNT_DIR/ssh/root/authorized_keys"

sudo chmod 600 \
    "$MOUNT_DIR/ssh/root/authorized_keys"

sync

echo
echo "nuubOS STATE provisioned"
echo "STATE   : $STATE_DEV"
echo "IPv4    : $IP_ADDRESS"
echo "SSH key : $SSH_PUBKEY"
