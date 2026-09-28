#!/bin/sh

nuubos_exfat_health_value()
{
    local file="$1"
    local key="$2"

    sed -n "s/^${key}=//p" "$file" 2>/dev/null |
        tail -1
}

nuubos_exfat_dirty()
{
    local device="$1"
    local bytes
    local lo
    local hi
    local flags

    bytes="$(
        dd if="$device" \
            bs=1 \
            skip=106 \
            count=2 \
            2>/dev/null |
        od -An -tu1
    )"

    set -- $bytes

    [ "$#" -eq 2 ] || return 1

    lo="$1"
    hi="$2"

    flags=$((lo + hi * 256))

    printf '%s\n' $(((flags >> 1) & 1))
}

nuubos_exfat_publish()
{
    local status="$1"
    local device="$2"
    local before="$3"
    local fsck_run="$4"
    local fsck_rc="$5"
    local after="$6"
    local result="$7"
    local tmp

    mkdir -p "$(dirname "$status")"

    tmp="$status.tmp.$$"

    cat > "$tmp" <<EOF2
DEVICE=$device
DIRTY_BEFORE=$before
FSCK_RUN=$fsck_run
FSCK_RC=$fsck_rc
DIRTY_AFTER=$after
RESULT=$result
EOF2

    mv "$tmp" "$status"
}

nuubos_exfat_prepare()
{
    local device="$1"
    local status="$2"
    local before=UNKNOWN
    local after=UNKNOWN
    local fsck_run=0
    local fsck_rc=NOT_RUN

    #
    # Repair is permitted only while the volume is completely unmounted.
    #
    if awk -v dev="$device" '
        $1 == dev {
            found=1
        }

        END {
            exit !found
        }
    ' /proc/mounts
    then
        nuubos_exfat_publish \
            "$status" "$device" \
            "$before" "$fsck_run" "$fsck_rc" "$after" ERROR

        return 1
    fi

    before="$(nuubos_exfat_dirty "$device")" || {
        nuubos_exfat_publish \
            "$status" "$device" \
            UNKNOWN 0 NOT_RUN UNKNOWN ERROR

        return 1
    }

    if [ "$before" = "0" ]; then
        nuubos_exfat_publish \
            "$status" "$device" \
            0 0 NOT_RUN 0 HEALTHY

        return 0
    fi

    #
    # Never clear VolumeDirty manually.
    #
    fsck_run=1

    fsck.exfat -p "$device"
    fsck_rc=$?

    case "$fsck_rc" in
        0|1)
            ;;
        *)
            after="$(
                nuubos_exfat_dirty "$device" 2>/dev/null ||
                printf '%s\n' UNKNOWN
            )"

            nuubos_exfat_publish \
                "$status" "$device" \
                "$before" 1 "$fsck_rc" "$after" ERROR

            return 1
            ;;
    esac

    after="$(nuubos_exfat_dirty "$device")" || {
        nuubos_exfat_publish \
            "$status" "$device" \
            "$before" 1 "$fsck_rc" UNKNOWN ERROR

        return 1
    }

    if [ "$after" != "0" ]; then
        nuubos_exfat_publish \
            "$status" "$device" \
            "$before" 1 "$fsck_rc" "$after" ERROR

        return 1
    fi

    nuubos_exfat_publish \
        "$status" "$device" \
        "$before" 1 "$fsck_rc" 0 REPAIRED

    return 0
}
