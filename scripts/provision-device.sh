#!/usr/bin/env bash
set -euo pipefail

BLOCK_DEVICE=""
TARGET_DEVICE=""

case "$#" in
    1)
        case "$1" in
            -h|--help|help)
                ;;
            *)
                BLOCK_DEVICE="${NUBOS_FLASH_DEVICE:-/dev/mmcblk0}"
                TARGET_DEVICE="$1"
                ;;
        esac
        ;;
    2)
        BLOCK_DEVICE="$1"
        TARGET_DEVICE="$2"
        ;;
esac

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

usage()
{
    cat <<EOF2
Usage:
  scripts/provision-device.sh <block-device> <device-id>

Examples:
  scripts/provision-device.sh /dev/mmcblk0 rg35xx-pro
  scripts/provision-device.sh /dev/sdb rgcubexx
  scripts/provision-device.sh /dev/sdb rg40xx-v

Supported device IDs:
  rg-sp
  rg28xx
  rg34xx
  rg34xx-sp
  rg34xx-sp-v2
  rg35xx-2024
  rg35xx-2024-v6
  rg35xx-h
  rg35xx-h-v6
  rg35xx-plus
  rg35xx-plus-v6
  rg35xx-pro
  rg35xx-sp
  rg35xx-sp-v2
  rg40xx-h
  rg40xx-h-v2
  rg40xx-v
  rg40xx-v-v2
  rgcubexx
EOF2
}

if [[ -z "$TARGET_DEVICE" ]]; then
    usage >&2
    exit 2
fi

case "$TARGET_DEVICE" in
    rg-sp|\
    rg28xx|\
    rg34xx|\
    rg34xx-sp|\
    rg34xx-sp-v2|\
    rg35xx-2024|\
    rg35xx-2024-v6|\
    rg35xx-h|\
    rg35xx-h-v6|\
    rg35xx-plus|\
    rg35xx-plus-v6|\
    rg35xx-pro|\
    rg35xx-sp|\
    rg35xx-sp-v2|\
    rg40xx-h|\
    rg40xx-h-v2|\
    rg40xx-v|\
    rg40xx-v-v2|\
    rgcubexx)
        ;;
    *)
        die "unsupported DEVICE value: $TARGET_DEVICE"
        ;;
esac

[[ -b "$BLOCK_DEVICE" ]] ||
    die "block device missing: $BLOCK_DEVICE"

BOOT_DEV="$(part_dev "$BLOCK_DEVICE" 2)"

[[ -b "$BOOT_DEV" ]] ||
    die "NUUBOS_BOOT partition missing: $BOOT_DEV"

label="$(
    sudo blkid -s LABEL -o value "$BOOT_DEV" 2>/dev/null || true
)"

[[ "$label" == "NUUBOS_BOOT" ]] ||
    die "$BOOT_DEV is not NUUBOS_BOOT (label=${label:-<none>})"

MOUNT_DIR="$(mktemp -d)"

cleanup()
{
    if mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
        sudo umount "$MOUNT_DIR"
    fi

    rmdir "$MOUNT_DIR" 2>/dev/null || true
}

trap cleanup EXIT

sudo mount "$BOOT_DEV" "$MOUNT_DIR"

printf 'DEVICE=%s\n' "$TARGET_DEVICE" |
    sudo tee "$MOUNT_DIR/nuubos.cfg" >/dev/null

sync

written="$(
    sudo cat "$MOUNT_DIR/nuubos.cfg"
)"

expected="DEVICE=$TARGET_DEVICE"

[[ "$written" == "$expected" ]] ||
    die "failed to verify nuubos.cfg"

echo
echo "nuubOS hardware provisioned"
echo "BOOT   : $BOOT_DEV"
echo "DEVICE : $TARGET_DEVICE"
