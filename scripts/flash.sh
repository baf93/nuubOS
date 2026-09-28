#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODE="${1:-}"
DEVICE="${2:-${NUBOS_FLASH_DEVICE:-/dev/mmcblk0}}"

IMGDIR="$ROOT/output/h700/images"
DISK_IMAGE="$IMGDIR/nuubOS-anbernic-h700.img"
ROOTFS_IMAGE="$IMGDIR/rootfs.ext4"
UBOOT_IMAGE="$IMGDIR/u-boot-sunxi-with-spl.bin"

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

P1="$(part_dev "$DEVICE" 1)"
P2="$(part_dev "$DEVICE" 2)"
P3="$(part_dev "$DEVICE" 3)"

usage()
{
    cat <<EOF2
Usage:
  scripts/flash.sh install [device]
  scripts/flash.sh update  [device]

Modes:
  install  Write the complete nuubOS H700 disk image.
           DESTRUCTIVE: partition table, NUUBOS_BOOT,
           rootfs and NUUBOS_STATE are replaced.

           The resulting NUUBOS_BOOT/nuubos.cfg contains:
               DEVICE=

           Select the target hardware afterwards with:
               scripts/provision-device.sh <block-device> <device-id>

  update   Update only raw U-Boot and rootfs.
           NUUBOS_BOOT and NUUBOS_STATE are preserved.
           Before the first experimental SPL update, run
           provision-device.sh so NUUBOS_BOOT contains nuubos.cfg.

Layout:
  raw      U-Boot/SPL
  p1       rootfs       ext4
  p2       NUUBOS_BOOT  FAT
  p3       NUUBOS_STATE ext4

Default device:
  $DEVICE
EOF2
}

unmount_partitions()
{
    local part

    #
    # Never raw-write a disk while any child partition remains mounted.
    #
    while IFS= read -r part; do
        [[ "$part" == "$DEVICE" ]] && continue
        sudo umount "$part" 2>/dev/null || true
    done < <(lsblk -lnpo NAME "$DEVICE")
}

check_device()
{
    if [[ ! -b "$DEVICE" ]]; then
        echo "ERROR: block device not found: $DEVICE" >&2
        exit 1
    fi
}

install_image()
{
    [[ -f "$DISK_IMAGE" ]] || {
        echo "ERROR: missing image: $DISK_IMAGE" >&2
        exit 1
    }

    echo "nuubOS INSTALL"
    echo "Device : $DEVICE"
    echo "Image  : $DISK_IMAGE"
    echo
    echo "WARNING: this replaces the complete partition table."
    echo "WARNING: verify the target device before continuing."

    unmount_partitions

    sudo dd \
        if="$DISK_IMAGE" \
        of="$DEVICE" \
        bs=4M \
        status=progress \
        conv=fsync

    sync

    sudo partprobe "$DEVICE" 2>/dev/null || true

    echo
    echo "INSTALL complete."
    echo
    echo "Hardware is not selected yet."
    echo "Run:"
    echo "  scripts/provision-device.sh $DEVICE <device-id>"
}

update_system()
{
    [[ -f "$ROOTFS_IMAGE" ]] || {
        echo "ERROR: missing rootfs: $ROOTFS_IMAGE" >&2
        exit 1
    }

    [[ -f "$UBOOT_IMAGE" ]] || {
        echo "ERROR: missing U-Boot: $UBOOT_IMAGE" >&2
        exit 1
    }

    if [[ ! -b "$P1" || ! -b "$P2" || ! -b "$P3" ]]; then
        echo "ERROR: nuubOS BOOT/rootfs/STATE layout not present." >&2
        echo "Use: scripts/flash.sh install $DEVICE" >&2
        exit 1
    fi

    boot_label="$(
        sudo blkid -s LABEL -o value "$P2" 2>/dev/null || true
    )"

    if [[ "$boot_label" != "NUUBOS_BOOT" ]]; then
        echo "ERROR: $P2 is not a nuubOS BOOT partition." >&2
        echo "Found label: ${boot_label:-<none>}" >&2
        echo "Refusing non-destructive update." >&2
        exit 1
    fi

    state_label="$(
        sudo blkid -s LABEL -o value "$P3" 2>/dev/null || true
    )"

    if [[ "$state_label" != "NUUBOS_STATE" ]]; then
        echo "ERROR: $P3 is not a nuubOS STATE partition." >&2
        echo "Found label: ${state_label:-<none>}" >&2
        echo "Refusing non-destructive update." >&2
        exit 1
    fi

    rootfs_bytes="$(stat -Lc '%s' "$ROOTFS_IMAGE")"
    partition_bytes="$(sudo blockdev --getsize64 "$P1")"

    if ((rootfs_bytes > partition_bytes)); then
        echo "ERROR: rootfs image does not fit rootfs partition." >&2
        echo "rootfs=$rootfs_bytes partition=$partition_bytes" >&2
        exit 1
    fi

    echo "nuubOS UPDATE"
    echo "Device : $DEVICE"
    echo "rootfs : $P1 (replaced)"
    echo "BOOT   : $P2 (preserved)"
    echo "STATE  : $P3 (preserved)"
    echo

    unmount_partitions

    #
    # U-Boot/SPL lives outside the partition table at offset 8 KiB.
    # Sector 0 and the partition layout are deliberately preserved.
    #
    sudo dd \
        if="$UBOOT_IMAGE" \
        of="$DEVICE" \
        bs=1K \
        seek=8 \
        conv=notrunc,fsync \
        status=none

    #
    # Replace rootfs only.
    #
    sudo dd \
        if="$ROOTFS_IMAGE" \
        of="$P1" \
        bs=4M \
        status=progress \
        conv=fsync

    sync

    echo
    echo "UPDATE complete."
    echo "NUUBOS_BOOT and NUUBOS_STATE preserved."
}

case "$MODE" in
    install)
        check_device
        install_image
        ;;
    update)
        check_device
        update_system
        ;;
    -h|--help|help)
        usage
        ;;
    "")
        usage >&2
        exit 2
        ;;
    *)
        echo "ERROR: unknown mode: $MODE" >&2
        usage >&2
        exit 2
        ;;
esac
