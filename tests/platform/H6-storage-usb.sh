#!/usr/bin/env bash
set -uo pipefail

AREA="H6"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

# ---------------------------------------------------------------------------
# Boot storage discovery
# ---------------------------------------------------------------------------

root_dev="$(
    remote "sed -n 's/.*root=\([^ ]*\).*/\1/p' /proc/cmdline" 2>/dev/null ||
    true
)"

case "$root_dev" in
    /dev/mmcblk*p1)
        disk="${root_dev%p1}"
        ;;
    *)
        fail "unable to derive TF1 boot disk from root=${root_dev:-unknown}"
        exit 0
        ;;
esac

p1="${disk}p1"
p2="${disk}p2"
p3="${disk}p3"
p4="${disk}p4"

disk_name="${disk#/dev/}"
p1_name="${p1#/dev/}"
p2_name="${p2#/dev/}"
p3_name="${p3#/dev/}"
p4_name="${p4#/dev/}"

pass "TF1 boot disk derived from root: $disk"

# Keep raw storage state with the regression artifacts.
remote "
echo '===== SFDISK ====='
sfdisk -d '$disk' 2>/dev/null || true

echo
echo '===== BLKID ====='
blkid '$p1' '$p2' '$p3' '$p4' 2>/dev/null || true

echo
echo '===== MOUNTS ====='
cat /proc/mounts | grep -E '(/state|/userdata|${disk_name})' || true
" > "$NUBOS_RESULT_DIR/H6-storage-layout.txt" 2>&1 || true

# ---------------------------------------------------------------------------
# Required first-boot storage tools
# ---------------------------------------------------------------------------

storage_tools=(
    sfdisk
    partx
    blockdev
    blkid
    mkfs.exfat
)

missing_tools=()

for tool in "${storage_tools[@]}"; do
    remote_has "$tool" || missing_tools+=("$tool")
done

if ((${#missing_tools[@]} == 0)); then
    pass "first-boot storage tools present"
else
    fail "first-boot storage tools missing: ${missing_tools[*]}"
fi

# ---------------------------------------------------------------------------
# TF1 partition presence
# ---------------------------------------------------------------------------

remote "[ -b '$p1' ]" \
    && pass "rootfs partition present ($p1)" \
    || fail "rootfs partition missing ($p1)"

remote "[ -b '$p2' ]" \
    && pass "NUUBOS_BOOT partition present ($p2)" \
    || fail "NUUBOS_BOOT partition missing ($p2)"

remote "[ -b '$p3' ]" \
    && pass "NUUBOS_STATE partition present ($p3)" \
    || fail "NUUBOS_STATE partition missing ($p3)"

remote "[ -b '$p4' ]" \
    && pass "NUUBOS_DATA partition present ($p4)" \
    || fail "NUUBOS_DATA partition missing ($p4)"

if ! remote "[ -b '$p1' ] && [ -b '$p2' ] && [ -b '$p3' ] && [ -b '$p4' ]"; then
    skip "TF1 geometry/filesystem checks unavailable"
    exit 0
fi

# ---------------------------------------------------------------------------
# TF1 geometry
#
# rootfs       = 2 GiB, starts at sector 2048, bootable
# NUUBOS_BOOT  = 16 MiB, immediately follows rootfs
# NUUBOS_STATE = 128 MiB, immediately follows NUUBOS_BOOT
# NUUBOS_DATA  = immediately follows NUUBOS_STATE and consumes the rest of TF1
# ---------------------------------------------------------------------------

p1_start="$(remote "cat '/sys/class/block/$p1_name/start'" || true)"
p1_size="$(remote "cat '/sys/class/block/$p1_name/size'" || true)"
p2_start="$(remote "cat '/sys/class/block/$p2_name/start'" || true)"
p2_size="$(remote "cat '/sys/class/block/$p2_name/size'" || true)"
p3_start="$(remote "cat '/sys/class/block/$p3_name/start'" || true)"
p3_size="$(remote "cat '/sys/class/block/$p3_name/size'" || true)"
p4_start="$(remote "cat '/sys/class/block/$p4_name/start'" || true)"
p4_size="$(remote "cat '/sys/class/block/$p4_name/size'" || true)"
disk_sectors="$(remote "blockdev --getsz '$disk'" || true)"

if [[ "$p1_start" == "2048" && "$p1_size" == "4194304" ]]; then
    pass "rootfs geometry 2 GiB at sector 2048"
else
    fail "rootfs geometry start=${p1_start:-?} size=${p1_size:-?}"
fi

if remote "sfdisk -d '$disk' 2>/dev/null | grep -F '$p1 :' | grep -qw bootable"; then
    pass "rootfs partition is bootable"
else
    fail "rootfs partition is not bootable"
fi

if remote "sfdisk -d '$disk' 2>/dev/null | grep -F '$p2 :' | grep -Eq 'type=(c|0[cC]|0x0[cC])([[:space:]]|$)'"; then
    pass "NUUBOS_BOOT MBR type 0x0c"
else
    fail "NUUBOS_BOOT MBR type is not 0x0c"
fi

expected_p2_start=$(( ${p1_start:-0} + ${p1_size:-0} ))

if [[ "$p2_start" == "$expected_p2_start" &&
      "$p2_size" == "32768" ]]; then
    pass "NUUBOS_BOOT geometry 16 MiB immediately after rootfs"
else
    fail "NUUBOS_BOOT geometry start=${p2_start:-?} size=${p2_size:-?}"
fi

expected_p3_start=$(( ${p2_start:-0} + ${p2_size:-0} ))

if [[ "$p3_start" == "$expected_p3_start" &&
      "$p3_size" == "262144" ]]; then
    pass "NUUBOS_STATE geometry 128 MiB immediately after NUUBOS_BOOT"
else
    fail "NUUBOS_STATE geometry start=${p3_start:-?} size=${p3_size:-?}"
fi

if remote "sfdisk -d '$disk' 2>/dev/null | grep -F '$p3 :' | grep -Eq 'type=(83|0x83)([[:space:]]|$)'"; then
    pass "NUUBOS_STATE MBR type 0x83"
else
    fail "NUUBOS_STATE MBR type is not 0x83"
fi

expected_p4_start=$(( ${p3_start:-0} + ${p3_size:-0} ))

if [[ "$p4_start" == "$expected_p4_start" &&
      $(( ${p4_start:-1} % 2048 )) -eq 0 ]]; then
    pass "NUUBOS_DATA starts after NUUBOS_STATE with 1 MiB alignment"
else
    fail "NUUBOS_DATA start=${p4_start:-?} expected=$expected_p4_start"
fi

expected_p4_size=$(( ${disk_sectors:-0} - ${p4_start:-0} ))

if [[ "$p4_size" == "$expected_p4_size" && "$expected_p4_size" -gt 0 ]]; then
    pass "NUUBOS_DATA consumes remaining TF1 capacity"
else
    fail "NUUBOS_DATA size=${p4_size:-?} expected=$expected_p4_size"
fi

if remote "
sfdisk -d '$disk' 2>/dev/null |
grep -F '$p4 :' |
grep -Eq 'type=7[[:space:]]*$'
"; then
    pass "USERDATA MBR type 0x07"
else
    fail "USERDATA MBR type is not 0x07"
fi

# ---------------------------------------------------------------------------
# rootfs / BOOT / STATE filesystems
# ---------------------------------------------------------------------------

p1_type="$(remote "blkid -s TYPE -o value '$p1' 2>/dev/null" || true)"
p1_label="$(remote "blkid -s LABEL -o value '$p1' 2>/dev/null" || true)"

[[ "$p1_type" == "ext4" ]] \
    && pass "rootfs filesystem ext4" \
    || fail "rootfs filesystem ${p1_type:-unknown}"

[[ "$p1_label" == "rootfs" ]] \
    && pass "rootfs label rootfs" \
    || fail "rootfs label ${p1_label:-unknown}"

p2_type="$(remote "blkid -s TYPE -o value '$p2' 2>/dev/null" || true)"
p2_label="$(remote "blkid -s LABEL -o value '$p2' 2>/dev/null" || true)"

[[ "$p2_type" == "vfat" ]] \
    && pass "NUUBOS_BOOT filesystem FAT" \
    || fail "NUUBOS_BOOT filesystem ${p2_type:-unknown}"

[[ "$p2_label" == "NUUBOS_BOOT" ]] \
    && pass "NUUBOS_BOOT label NUUBOS_BOOT" \
    || fail "NUUBOS_BOOT label ${p2_label:-unknown}"

p3_type="$(remote "blkid -s TYPE -o value '$p3' 2>/dev/null" || true)"
p3_label="$(remote "blkid -s LABEL -o value '$p3' 2>/dev/null" || true)"

[[ "$p3_type" == "ext4" ]] \
    && pass "STATE filesystem ext4" \
    || fail "STATE filesystem ${p3_type:-unknown}"

[[ "$p3_label" == "NUUBOS_STATE" ]] \
    && pass "STATE label NUUBOS_STATE" \
    || fail "STATE label ${p3_label:-unknown}"

if remote "
awk -v d='$p3' '
\$1 == d && \$2 == \"/state\" && \$3 == \"ext4\" {
    rw = (\$4 ~ /(^|,)rw(,|$)/)
    noatime = (\$4 ~ /(^|,)noatime(,|$)/)
    if (rw && noatime)
        ok=1
}
END { exit !ok }
' /proc/mounts
"; then
    pass "STATE mounted rw,noatime on /state"
else
    fail "STATE mount policy unexpected"
fi

# ---------------------------------------------------------------------------
# Persistent STATE contract
# ---------------------------------------------------------------------------

remote "grep -qx 'NUUBOS_CONFIG_VERSION=5' /state/config/nuubos.conf" \
    && pass "STATE config version 5" \
    || fail "STATE config version invalid"

# ---------------------------------------------------------------------------
# Persistent user-profile foundation
# ---------------------------------------------------------------------------

remote "grep -qx 'NUUBOS_USERS_VERSION=1' /state/users/.nuubos-users" \
    && pass "user registry version 1" \
    || fail "user registry missing or invalid"

login_mode="$(
    remote "sed -n 's/^USER_LOGIN_MODE=//p' /state/config/nuubos.conf | head -n 1" \
        || true
)"

case "$login_mode" in
    AUTO|DEFAULT|SELECT)
        pass "user login mode valid ($login_mode)"
        ;;
    *)
        fail "user login mode invalid (${login_mode:-missing})"
        ;;
esac

default_user="$(
    remote "sed -n 's/^DEFAULT_USER=//p' /state/config/nuubos.conf | head -n 1" \
        || true
)"

if [[ "$default_user" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]]; then
    pass "default user UUID valid ($default_user)"
else
    fail "default user UUID invalid (${default_user:-missing})"
fi

if [[ -n "$default_user" ]] &&
   remote "[ -d '/state/users/$default_user' ]"; then
    pass "default user profile directory present"
else
    fail "default user profile directory missing"
fi

if [[ -n "$default_user" ]] &&
   remote "grep -qx 'NUUBOS_USER_PROFILE_VERSION=1' '/state/users/$default_user/profile.conf'"; then
    pass "default user profile version 1"
else
    fail "default user profile version invalid"
fi

if [[ -n "$default_user" ]] &&
   remote "grep -qx 'USER_ID=$default_user' '/state/users/$default_user/profile.conf'"; then
    pass "default user profile identity consistent"
else
    fail "default user profile identity inconsistent"
fi

if [[ -n "$default_user" ]] &&
   remote "grep -qx 'NUUBOS_USER_SETTINGS_VERSION=1' '/state/users/$default_user/settings.conf'"; then
    pass "default user settings version 1"
else
    fail "default user settings missing or invalid"
fi

if [[ -n "$default_user" ]] &&
   remote "find '/state/users/$default_user/secrets' \
           -maxdepth 0 -type d -perm 700 -print 2>/dev/null |
           grep -qx '/state/users/$default_user/secrets'"; then
    pass "default user secrets directory protected"
else
    fail "default user secrets directory missing or permissions invalid"
fi

user_count="$(
    remote '
        n=0
        for d in /state/users/*; do
            [ -d "$d" ] || continue
            n=$((n + 1))
        done
        echo "$n"
    ' || true
)"
user_count="${user_count:-0}"

((user_count > 0)) \
    && pass "persistent user profiles present ($user_count)" \
    || fail "no persistent user profiles"

active_user="$(
    remote 'cat /run/nuubos/user/active 2>/dev/null' || true
)"

if ((user_count == 1)); then
    [[ "$active_user" == "$default_user" ]] \
        && pass "single-user active profile equals default user" \
        || fail "single-user active profile inconsistent"
elif [[ "$login_mode" == "DEFAULT" ]]; then
    [[ "$active_user" == "$default_user" ]] \
        && pass "default-login active profile equals default user" \
        || fail "default-login active profile inconsistent"
else
    [[ -z "$active_user" ]] \
        && pass "multi-user selection has no premature active profile" \
        || fail "active profile published before user selection"
fi

storage_mode="$(
    remote "sed -n 's/^STORAGE_MODE=//p' /state/config/nuubos.conf" ||
    true
)"

case "$storage_mode" in
    AUTO|SINGLE|DUAL)
        pass "STATE storage mode valid ($storage_mode)"
        ;;
    *)
        fail "STATE storage mode invalid (${storage_mode:-missing})"
        ;;
esac

if remote "[ \"\$(readlink /etc/dropbear)\" = '/state/ssh/dropbear' ]"; then
    pass "Dropbear server state linked to STATE"
else
    fail "Dropbear server state not linked to STATE"
fi

if remote "[ \"\$(readlink /root/.ssh)\" = '/state/ssh/root' ]"; then
    pass "root SSH state linked to STATE"
else
    fail "root SSH state not linked to STATE"
fi

if remote "
find /state/ssh/dropbear \
    -maxdepth 1 \
    -type f \
    -name 'dropbear_*_host_key' |
grep -q .
"; then
    pass "persistent SSH server identity present"
else
    fail "persistent SSH server identity missing"
fi

remote "[ -f /state/ssh/root/id_dropbear ]" \
    && pass "persistent SSH client identity present" \
    || fail "persistent SSH client identity missing"

remote "[ -x /etc/init.d/S03nuubos-state ]" \
    && pass "S03 STATE initializer installed" \
    || fail "S03 STATE initializer missing"

# ---------------------------------------------------------------------------
# USERDATA filesystem
# ---------------------------------------------------------------------------

p4_type="$(remote "blkid -s TYPE -o value '$p4' 2>/dev/null" || true)"
p4_label="$(remote "blkid -s LABEL -o value '$p4' 2>/dev/null" || true)"

[[ "$p4_type" == "exfat" ]] \
    && pass "USERDATA filesystem exFAT" \
    || fail "USERDATA filesystem ${p4_type:-unknown}"

[[ "$p4_label" == "NUUBOS_DATA" ]] \
    && pass "USERDATA label NUUBOS_DATA" \
    || fail "USERDATA label ${p4_label:-unknown}"

if remote "
awk -v d='$p4' '
\$1 == d && \$2 == \"/userdata\" && \$3 == \"exfat\" {
    rw = (\$4 ~ /(^|,)rw(,|$)/)
    noatime = (\$4 ~ /(^|,)noatime(,|$)/)
    if (rw && noatime)
        ok=1
}
END { exit !ok }
' /proc/mounts
"; then
    pass "USERDATA mounted rw,noatime on /userdata"
else
    fail "USERDATA mount policy unexpected"
fi

remote "grep -qx 'NUUBOS_USERDATA_VERSION=2' /userdata/.nuubos-userdata" \
    && pass "USERDATA marker version 2" \
    || fail "USERDATA marker invalid"

remote "[ -x /etc/init.d/S04nuubos-userdata ]" \
    && pass "S04 USERDATA initializer installed" \
    || fail "S04 USERDATA initializer missing"

# ---------------------------------------------------------------------------
# USERDATA directory contract
# ---------------------------------------------------------------------------

userdata_dirs=(
applications
bios
cache
cheats
downloads
library
logs
music
ports
roms
themes
users
"users/$default_user"
"users/$default_user/saves"
"users/$default_user/states"
"users/$default_user/screenshots"
"users/$default_user/recordings"
"users/$default_user/library"
"users/$default_user/appdata"
)

legacy_user_dirs=(
saves
states
screenshots
recordings
)

legacy_present=()

for dir in "${legacy_user_dirs[@]}"; do
    remote "[ ! -e '/userdata/$dir' ]" ||
        legacy_present+=("$dir")
done

if ((${#legacy_present[@]} == 0)); then
    pass "legacy flat per-user USERDATA directories absent"
else
    fail "legacy flat USERDATA directories present: ${legacy_present[*]}"
fi

if [[ -n "$active_user" ]]; then
    if remote "
        awk '
            \$2 == \"/run/nuubos/userdata\" {
                found=1
            }

            END {
                exit !found
            }
        ' /proc/mounts
    "; then
        pass "active-user USERDATA runtime view mounted"
    else
        fail "active-user USERDATA runtime view missing"
    fi

    if remote "
        awk -v expected='/users/$active_user' '
            \$4 == expected &&
            \$5 == \"/run/nuubos/userdata\" {
                ok=1
            }

            END {
                exit !ok
            }
        ' /proc/self/mountinfo
    "; then
        pass "active-user USERDATA runtime view targets active profile"
    else
        fail "active-user USERDATA runtime view targets wrong profile"
    fi
fi

missing_dirs=()

for dir in "${userdata_dirs[@]}"; do
    remote "[ -d '/userdata/$dir' ]" || missing_dirs+=("$dir")
done

if ((${#missing_dirs[@]} == 0)); then
    pass "USERDATA directory skeleton complete"
else
    fail "USERDATA directories missing: ${missing_dirs[*]}"
fi

# Write/remove coverage is restricted to the explicitly invasive modes.
if [[ "$NUBOS_TEST_MODE" == "full" || "$NUBOS_TEST_MODE" == "stress" ]] && remote '
f="/userdata/.nuubos-regression-write.$$"
trap "rm -f \"$f\"" EXIT HUP INT TERM

printf "%s\n" "nuubOS-H6-write-test" > "$f" &&
grep -qx "nuubOS-H6-write-test" "$f" &&
rm -f "$f" &&
sync
'; then
    pass "USERDATA read/write/remove"
else
    if [[ "$NUBOS_TEST_MODE" != "full" && "$NUBOS_TEST_MODE" != "stress" ]]; then
        skip "USERDATA read/write/remove (full/stress only)"
    else
        fail "USERDATA read/write/remove"
    fi
fi

if remote '[ ! -e /state/migrations/userdata-init.pending ]'; then
    pass "USERDATA initialization transaction complete"
else
    fail "USERDATA initialization transaction still pending"
fi

# ---------------------------------------------------------------------------
# TF2 baseline
#
# TF2 policy (AUTO/SINGLE/DUAL) will be validated separately. The normal H6
# regression must never format or modify an inserted TF2 card.
# ---------------------------------------------------------------------------

if remote '[ -d /sys/class/mmc_host/mmc2 ]'; then
    pass "TF2 MMC host present"
else
    fail "TF2 MMC host missing"
fi

if remote '[ -b /dev/mmcblk2 ]'; then
    pass "TF2 card detected"
else
    skip "TF2 card not inserted"
fi

# ---------------------------------------------------------------------------
# TF2 storage ownership / selector state
# ---------------------------------------------------------------------------

tf2_status="/run/nuubos/storage/tf2.status"

if remote "[ -f '$tf2_status' ]"; then
    pass "TF2 storage status published"
else
    fail "TF2 storage status missing"
fi

if remote "[ -f '$tf2_status' ]"; then
    tf2_state="$(
        remote "sed -n 's/^TF2_STATE=//p' '$tf2_status'" ||
        true
    )"

    action_required="$(
        remote "sed -n 's/^ACTION_REQUIRED=//p' '$tf2_status'" ||
        true
    )"

    prompt_user="$(
        remote "sed -n 's/^PROMPT_USER=//p' '$tf2_status'" ||
        true
    )"

    status_mode="$(
        remote "sed -n 's/^STORAGE_MODE=//p' '$tf2_status'" ||
        true
    )"

    active_storage="$(
        remote "sed -n 's/^ACTIVE_STORAGE=//p' '$tf2_status'" ||
        true
    )"

    storage_ready="$(
        remote "sed -n 's/^STORAGE_READY=//p' '$tf2_status'" ||
        true
    )"

    effective_userdata="$(
        remote 'awk '"'"'$2=="/userdata"{s=$1} END{print s}'"'"' /proc/mounts' ||
        true
    )"

    [[ "$status_mode" == "$storage_mode" ]] \
        && pass "TF2 status storage mode consistent ($status_mode)" \
        || fail "TF2 status mode '${status_mode:-missing}' != '$storage_mode'"

    case "$tf2_state" in
        ABSENT)
            pass "TF2 classified ABSENT"

            if [[ "$storage_mode" == "DUAL" ]]; then
                [[ "$action_required" == "INSERT" &&
                   "$prompt_user" == "1" ]] \
                    && pass "DUAL missing-TF2 action published" \
                    || fail "DUAL missing-TF2 action invalid"

                [[ "$active_storage" == "NONE" &&
                   "$storage_ready" == "0" ]] \
                    && pass "DUAL missing TF2 leaves storage not ready" \
                    || fail "DUAL missing TF2 selector state invalid"
            else
                [[ "$action_required" == "NONE" &&
                   "$prompt_user" == "0" ]] \
                    && pass "absent TF2 requires no action" \
                    || fail "absent TF2 action invalid"

                [[ "$active_storage" == "TF1" &&
                   "$storage_ready" == "1" &&
                   "$effective_userdata" == "$p4" ]] \
                    && pass "absent TF2 leaves TF1 active" \
                    || fail "absent TF2 active storage invalid"
            fi
            ;;

        FOREIGN)
            pass "TF2 classified FOREIGN"

            if [[ "$storage_mode" == "SINGLE" ]]; then
                [[ "$action_required" == "NONE" &&
                   "$prompt_user" == "0" ]] \
                    && pass "SINGLE ignores foreign TF2" \
                    || fail "SINGLE foreign-TF2 action invalid"
            else
                [[ "$action_required" == "FORMAT" &&
                   "$prompt_user" == "1" ]] \
                    && pass "foreign TF2 requests explicit format" \
                    || fail "foreign TF2 action invalid"
            fi

            if remote 'mount | grep -qE "^/dev/mmcblk2(p[0-9]+)? "'
            then
                fail "foreign TF2 unexpectedly mounted"
            else
                pass "foreign TF2 remains unmounted"
            fi

            if [[ "$storage_mode" == "DUAL" ]]; then
                [[ "$active_storage" == "NONE" &&
                   "$storage_ready" == "0" ]] \
                    && pass "DUAL foreign TF2 leaves storage not ready" \
                    || fail "DUAL foreign TF2 selector state invalid"
            else
                [[ "$active_storage" == "TF1" &&
                   "$storage_ready" == "1" &&
                   "$effective_userdata" == "$p4" ]] \
                    && pass "foreign TF2 leaves TF1 USERDATA active" \
                    || fail "foreign TF2 changed active USERDATA"
            fi
            ;;

        OWNED)
            pass "TF2 classified OWNED"

            [[ "$action_required" == "NONE" &&
               "$prompt_user" == "0" ]] \
                && pass "owned TF2 requires no user action" \
                || fail "owned TF2 action state invalid"

            if [[ "$storage_mode" == "SINGLE" ]]; then
                [[ "$active_storage" == "TF1" &&
                   "$storage_ready" == "1" &&
                   "$effective_userdata" == "$p4" ]] \
                    && pass "SINGLE keeps TF1 active with owned TF2" \
                    || fail "SINGLE owned-TF2 selector state invalid"
            else
                [[ "$active_storage" == "TF2" &&
                   "$storage_ready" == "1" &&
                   "$effective_userdata" == "/dev/mmcblk2p1" ]] \
                    && pass "$storage_mode activates owned TF2" \
                    || fail "$storage_mode owned-TF2 selector state invalid"

                if remote "
awk '
\$1 == \"$p4\" &&
\$2 == \"/run/nuubos/storage/tf1\" {
    found=1
}
END { exit !found }
' /proc/mounts
"; then
                    pass "TF1 remains available as stable sync view"
                else
                    fail "TF1 stable sync view missing"
                fi
            fi
            ;;

        *)
            fail "unknown TF2 state '${tf2_state:-missing}'"
            ;;
    esac

    if [[ "$storage_ready" == "1" ]]; then
        remote "[ -e '/run/nuubos/storage/ready' ]" \
            && pass "storage ready file consistent" \
            || fail "storage ready file missing"
    elif [[ "$storage_ready" == "0" ]]; then
        remote "[ ! -e '/run/nuubos/storage/ready' ]" \
            && pass "storage not-ready state consistent" \
            || fail "storage ready file unexpectedly present"
    else
        fail "invalid STORAGE_READY '${storage_ready:-missing}'"
    fi
fi

remote "[ -x /etc/init.d/S05nuubos-storage ]" \
    && pass "S05 storage selector installed" \
    || fail "S05 storage selector missing"

# ---------------------------------------------------------------------------
# Invasive storage qualification
#
# These helpers preserve and restore state, but they intentionally exercise
# configuration, service restarts, repair and recovery paths. Keep them out of
# quick so that quick remains strictly observational/read-only.
# ---------------------------------------------------------------------------

case "$NUBOS_TEST_MODE" in
    full|stress)
        # shellcheck disable=SC1091
        source "$NUBOS_ROOT/tests/platform/lib/h6-storage-backup.sh"
        run_h6_storage_backup

        # shellcheck disable=SC1091
        source "$NUBOS_ROOT/tests/platform/lib/h6-storage-health.sh"
        run_h6_storage_health

        # shellcheck disable=SC1091
        source "$NUBOS_ROOT/tests/platform/lib/h6-storage-recovery.sh"
        run_h6_storage_recovery
        ;;
    *)
        skip "storage backup/health/recovery qualification (full/stress only)"
        ;;
esac

# ---------------------------------------------------------------------------
# USB baseline
# ---------------------------------------------------------------------------

if remote '[ -d /sys/bus/usb/devices ]'; then
    pass "USB bus sysfs present"
else
    fail "USB bus sysfs missing"
fi

if [ "${USB_REQUIRE_OTG:-0}" = "1" ]; then
    if remote 'test "$(tr -d "\000" </proc/device-tree/soc/usb@5100000/dr_mode 2>/dev/null)" = "otg"'; then
        pass "USB0 device-tree mode OTG"
    else
        fail "USB0 device-tree mode is not OTG"
    fi

    if remote '[ -e /proc/device-tree/soc/phy@5100400/usb0_id_det-gpios ]'; then
        pass "USB0 ID detection described by device tree"
    else
        fail "USB0 ID detection missing from device tree"
    fi

    if remote '[ -e /proc/device-tree/soc/phy@5100400/usb0_vbus_power-supply ]'; then
        pass "USB0 external VBUS detection supply described"
    else
        fail "USB0 external VBUS detection supply missing"
    fi

    if remote '[ -e /proc/device-tree/soc/phy@5100400/usb0_vbus-supply ]'; then
        pass "USB0 switched VBUS supply described"
    else
        fail "USB0 switched VBUS supply missing"
    fi

    if remote 'readlink -f /sys/bus/platform/devices/5101000.usb/driver 2>/dev/null | grep -q "/ehci-platform$"'; then
        pass "USB0 EHCI host controller bound"
    else
        fail "USB0 EHCI host controller not bound"
    fi

    if remote 'readlink -f /sys/bus/platform/devices/5101400.usb/driver 2>/dev/null | grep -q "/ohci-platform$"'; then
        pass "USB0 OHCI host controller bound"
    else
        fail "USB0 OHCI host controller not bound"
    fi

    if remote 'grep -q "usb0_id_det" /sys/kernel/debug/gpio 2>/dev/null'; then
        pass "USB0 ID detection GPIO claimed"
    else
        fail "USB0 ID detection GPIO not claimed"
    fi

    if remote 'grep -q "regulator-usb0-vbus" /sys/kernel/debug/gpio 2>/dev/null'; then
        pass "USB0 switched VBUS GPIO claimed"
    else
        fail "USB0 switched VBUS GPIO not claimed"
    fi
fi
