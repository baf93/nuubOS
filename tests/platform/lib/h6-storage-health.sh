#!/usr/bin/env bash

run_h6_storage_health() {
    local health_src
    local s04_src
    local s05_src

    health_src="$NUBOS_ROOT/board/nuubos/common/rootfs-overlay/usr/lib/nuubos/exfat-health.sh"
    s04_src="$NUBOS_ROOT/board/nuubos/common/rootfs-overlay/etc/init.d/S04nuubos-userdata"
    s05_src="$NUBOS_ROOT/board/nuubos/common/rootfs-overlay/etc/init.d/S05nuubos-storage"

    info "exFAT storage health qualification"

    # -----------------------------------------------------------------------
    # Static/source qualification
    # -----------------------------------------------------------------------

    if [[ -f "$health_src" ]] && sh -n "$health_src"; then
        pass "exFAT health primitive source valid"
    else
        fail "exFAT health primitive source invalid"
        return 0
    fi

    grep -q 'nuubos_exfat_prepare' "$s04_src" \
        && pass "S04 qualifies TF1 before rw mount" \
        || fail "S04 TF1 health hook missing"

    grep -q 'nuubos_exfat_prepare' "$s05_src" \
        && pass "S05 qualifies TF2 before rw activation" \
        || fail "S05 TF2 health hook missing"

    grep -q 'REPAIR_TF2' "$s05_src" &&
    grep -q 'REPAIR_TF1' "$s05_src" &&
    grep -q 'STORAGE_RECOVERY' "$s05_src" \
        && pass "storage recovery actions present" \
        || fail "storage recovery actions incomplete"

    # -----------------------------------------------------------------------
    # Deployed qualification
    # -----------------------------------------------------------------------

    if remote '
        [ -r /usr/lib/nuubos/exfat-health.sh ] &&
        sh -n /usr/lib/nuubos/exfat-health.sh &&
        grep -q nuubos_exfat_prepare /etc/init.d/S04nuubos-userdata &&
        grep -q nuubos_exfat_prepare /etc/init.d/S05nuubos-storage
    '; then
        pass "deployed exFAT health integration present"
    else
        fail "deployed exFAT health integration missing"
        return 0
    fi

    if remote '
        STATUS=/run/nuubos/storage/tf2.status

        grep -q "^STORAGE_HEALTH=" "$STATUS" &&
        grep -q "^TF1_HEALTH=" "$STATUS" &&
        grep -q "^TF2_HEALTH=" "$STATUS"
    '; then
        pass "storage health state published"
    else
        fail "storage health state fields missing"
    fi

    # -----------------------------------------------------------------------
    # Dynamic qualification.
    #
    # No real filesystem is corrupted. Dirty/failure conditions are created
    # either on a temporary image or by replacing the health helper briefly.
    # -----------------------------------------------------------------------

    remote_sh <<'EOS' || true
STATUS_DIR=/run/nuubos/storage
STATUS=$STATUS_DIR/tf2.status
TF1_HEALTH=$STATUS_DIR/tf1.health

ROOT_DEV="$(
    sed -n 's/.*root=\([^ ]*\).*/\1/p' /proc/cmdline
)"

case "$ROOT_DEV" in
    /dev/mmcblk*p1)
        TF1="${ROOT_DEV%p1}p4"
        ;;
    *)
        echo "[FAIL] [H6] unable to derive TF1 from root=${ROOT_DEV:-unknown}"
        exit 1
        ;;
esac

REAL=/usr/lib/nuubos/exfat-health.sh
SAVE=/tmp/h6-health.real

TEST_IMG=/tmp/h6-health-fail.img
TEST_STATUS=/tmp/h6-health-fail.status
TEST_BIN=/tmp/h6-health-bin

fail_count=0
cleanup_done=0

pass()
{
    echo "[PASS] [H6] $*"
}

fail()
{
    echo "[FAIL] [H6] $*"
    fail_count=$((fail_count + 1))
}

skip()
{
    echo "[SKIP] [H6] $*"
}

value()
{
    sed -n "s/^${1}=//p" "$STATUS" 2>/dev/null | tail -1
}

userdata_source()
{
    awk '
        $2 == "/userdata" {
            source=$1
        }

        END {
            print source
        }
    ' /proc/mounts
}

ORIGINAL_MODE="$(
    sed -n 's/^STORAGE_MODE=//p' \
        /state/config/nuubos.conf
)"

cleanup()
{
    [ "$cleanup_done" -eq 0 ] || return 0
    cleanup_done=1

    if [ -f "$SAVE" ]; then
        cp "$SAVE" "$REAL"
        chmod 755 "$REAL"
    fi

    /etc/init.d/S05nuubos-storage stop >/dev/null 2>&1 || true
    /etc/init.d/S04nuubos-userdata restart >/dev/null 2>&1 || true

    if [ -n "$ORIGINAL_MODE" ]; then
        /usr/sbin/nuubos-storagectl \
            storage-mode "$ORIGINAL_MODE" \
            >/dev/null 2>&1 || true
    fi

    rm -rf \
        "$SAVE" \
        "$TEST_IMG" \
        "$TEST_STATUS" \
        "$TEST_BIN"
}

trap cleanup EXIT HUP INT TERM

# ===========================================================================
# Nominal health state
# ===========================================================================

TF1_RESULT="$(
    sed -n 's/^RESULT=//p' "$TF1_HEALTH" 2>/dev/null |
    tail -1
)"

case "$TF1_RESULT" in
    HEALTHY|REPAIRED)
        pass "TF1 boot health qualified ($TF1_RESULT)"
        ;;
    *)
        fail "TF1 boot health invalid (${TF1_RESULT:-missing})"
        ;;
esac

if [ "$(value TF2_STATE)" = "OWNED" ]; then
    TF2_RESULT="$(
        sed -n 's/^RESULT=//p' \
            "$STATUS_DIR/tf2.health" 2>/dev/null |
        tail -1
    )"

    case "$TF2_RESULT" in
        HEALTHY|REPAIRED)
            pass "owned TF2 health qualified ($TF2_RESULT)"
            ;;
        *)
            fail "owned TF2 health invalid (${TF2_RESULT:-missing})"
            ;;
    esac
fi

# ===========================================================================
# Primitive: failed fsck must NOT hide VolumeDirty
# ===========================================================================

rm -rf "$TEST_IMG" "$TEST_STATUS" "$TEST_BIN"
mkdir -p "$TEST_BIN"

dd if=/dev/zero \
    of="$TEST_IMG" \
    bs=512 \
    count=1 \
    2>/dev/null

printf '\002\000' |
dd of="$TEST_IMG" \
    bs=1 \
    seek=106 \
    conv=notrunc \
    2>/dev/null

cat > "$TEST_BIN/fsck.exfat" <<'EOF2'
#!/bin/sh
exit 8
EOF2

chmod 755 "$TEST_BIN/fsck.exfat"

. "$REAL"

if PATH="$TEST_BIN:$PATH" \
   nuubos_exfat_prepare "$TEST_IMG" "$TEST_STATUS"
then
    fail "failed exFAT repair was accepted"
else
    RESULT="$(sed -n 's/^RESULT=//p' "$TEST_STATUS")"
    RC="$(sed -n 's/^FSCK_RC=//p' "$TEST_STATUS")"
    DIRTY="$(nuubos_exfat_dirty "$TEST_IMG")"

    if [ "$RESULT" = "ERROR" ] &&
       [ "$RC" = "8" ] &&
       [ "$DIRTY" = "1" ]; then
        pass "failed exFAT repair preserves dirty evidence"
    else
        fail "failed exFAT repair state incorrect"
    fi
fi

rm -rf "$TEST_IMG" "$TEST_STATUS" "$TEST_BIN"

# Dynamic selector failures require an owned TF2.
if [ "$(value TF2_STATE)" != "OWNED" ]; then
    skip "storage health failure matrix requires OWNED TF2"
    cleanup
    trap - EXIT HUP INT TERM
    exit "$fail_count"
fi

cp "$REAL" "$SAVE" || {
    fail "unable to preserve deployed health helper"
    cleanup
    trap - EXIT HUP INT TERM
    exit "$fail_count"
}

# ===========================================================================
# TF2 fsck failure mock
# ===========================================================================

cat > "$REAL" <<'EOF2'
#!/bin/sh

nuubos_exfat_health_value()
{
    sed -n "s/^${2}=//p" "$1" 2>/dev/null |
        tail -1
}

nuubos_exfat_prepare()
{
    STATUS="$2"

    mkdir -p "$(dirname "$STATUS")"

    cat > "$STATUS" <<EOF3
DEVICE=/dev/mmcblk2p1
DIRTY_BEFORE=1
FSCK_RUN=1
FSCK_RC=8
DIRTY_AFTER=1
RESULT=ERROR
EOF3

    return 1
}
EOF2

chmod 755 "$REAL"

# AUTO: safe fallback to TF1.
/usr/sbin/nuubos-storagectl storage-mode AUTO >/dev/null

SOURCE="$(userdata_source)"

if [ "$(value ACTIVE_STORAGE)" = "TF1" ] &&
   [ "$(value STORAGE_READY)" = "1" ] &&
   [ "$(value STORAGE_HEALTH)" = "DEGRADED" ] &&
   [ "$(value TF2_HEALTH)" = "ERROR" ] &&
   [ "$(value ACTION_REQUIRED)" = "REPAIR_TF2" ] &&
   [ "$(value PROMPT_USER)" = "1" ] &&
   [ "$SOURCE" = "$TF1" ]; then
    pass "AUTO falls back to TF1 after TF2 repair failure"
else
    fail "AUTO TF2 repair-failure policy incorrect"
fi

# DUAL: no active USERDATA.
/usr/sbin/nuubos-storagectl storage-mode DUAL >/dev/null

SOURCE="$(userdata_source)"

if [ "$(value ACTIVE_STORAGE)" = "NONE" ] &&
   [ "$(value STORAGE_READY)" = "0" ] &&
   [ "$(value STORAGE_HEALTH)" = "FAILED" ] &&
   [ "$(value TF2_HEALTH)" = "ERROR" ] &&
   [ "$(value ACTION_REQUIRED)" = "REPAIR_TF2" ] &&
   [ "$(value PROMPT_USER)" = "1" ] &&
   [ -z "$SOURCE" ]; then
    pass "DUAL blocks USERDATA after TF2 repair failure"
else
    fail "DUAL TF2 repair-failure policy incorrect"
fi

if awk -v tf1="$TF1" '
    $1 == tf1 &&
    $2 == "/run/nuubos/storage/tf1" {
        found=1
    }

    END {
        exit !found
    }
' /proc/mounts
then
    pass "DUAL retains healthy TF1 recovery view"
else
    fail "DUAL lost healthy TF1 recovery view"
fi

# Restore real TF2 health helper.
cp "$SAVE" "$REAL"
chmod 755 "$REAL"

# ===========================================================================
# TF1 failure with healthy TF2
# ===========================================================================

/etc/init.d/S05nuubos-storage stop >/dev/null 2>&1 || true
umount /userdata 2>/dev/null || true

cat > "$TF1_HEALTH" <<EOF2
DEVICE=$TF1
DIRTY_BEFORE=1
FSCK_RUN=1
FSCK_RC=8
DIRTY_AFTER=1
RESULT=ERROR
EOF2

sed -i \
    's/^STORAGE_MODE=.*/STORAGE_MODE=AUTO/' \
    /state/config/nuubos.conf

/etc/init.d/S05nuubos-storage start >/dev/null

SOURCE="$(userdata_source)"

if [ "$(value ACTIVE_STORAGE)" = "TF2" ] &&
   [ "$(value STORAGE_READY)" = "1" ] &&
   [ "$(value STORAGE_HEALTH)" = "DEGRADED" ] &&
   [ "$(value TF1_HEALTH)" = "ERROR" ] &&
   [ "$(value TF2_HEALTH)" = "HEALTHY" ] &&
   [ "$(value ACTION_REQUIRED)" = "REPAIR_TF1" ] &&
   [ "$(value PROMPT_USER)" = "1" ] &&
   [ "$SOURCE" = "/dev/mmcblk2p1" ]; then
    pass "AUTO falls back to TF2 after TF1 repair failure"
else
    fail "AUTO TF1 repair-failure policy incorrect"
fi

/usr/sbin/nuubos-storagectl storage-mode DUAL >/dev/null

SOURCE="$(userdata_source)"

if [ "$(value ACTIVE_STORAGE)" = "NONE" ] &&
   [ "$(value STORAGE_READY)" = "0" ] &&
   [ "$(value STORAGE_HEALTH)" = "FAILED" ] &&
   [ "$(value TF1_HEALTH)" = "ERROR" ] &&
   [ "$(value ACTION_REQUIRED)" = "REPAIR_TF1" ] &&
   [ "$(value PROMPT_USER)" = "1" ] &&
   [ -z "$SOURCE" ]; then
    pass "DUAL blocks USERDATA after TF1 repair failure"
else
    fail "DUAL TF1 repair-failure policy incorrect"
fi

/usr/sbin/nuubos-storagectl storage-mode SINGLE >/dev/null

SOURCE="$(userdata_source)"

if [ "$(value ACTIVE_STORAGE)" = "NONE" ] &&
   [ "$(value STORAGE_READY)" = "0" ] &&
   [ "$(value STORAGE_HEALTH)" = "FAILED" ] &&
   [ "$(value TF1_HEALTH)" = "ERROR" ] &&
   [ "$(value ACTION_REQUIRED)" = "REPAIR_TF1" ] &&
   [ "$(value PROMPT_USER)" = "1" ] &&
   [ -z "$SOURCE" ]; then
    pass "SINGLE blocks unhealthy TF1"
else
    fail "SINGLE TF1 repair-failure policy incorrect"
fi

# ===========================================================================
# TF1 + TF2 failure -> global recovery
# ===========================================================================

cat > "$REAL" <<'EOF2'
#!/bin/sh

nuubos_exfat_health_value()
{
    sed -n "s/^${2}=//p" "$1" 2>/dev/null |
        tail -1
}

nuubos_exfat_prepare()
{
    STATUS="$2"

    mkdir -p "$(dirname "$STATUS")"

    cat > "$STATUS" <<EOF3
DEVICE=/dev/mmcblk2p1
DIRTY_BEFORE=1
FSCK_RUN=1
FSCK_RC=8
DIRTY_AFTER=1
RESULT=ERROR
EOF3

    return 1
}
EOF2

chmod 755 "$REAL"

sed -i \
    's/^STORAGE_MODE=.*/STORAGE_MODE=AUTO/' \
    /state/config/nuubos.conf

/etc/init.d/S05nuubos-storage start >/dev/null

SOURCE="$(userdata_source)"

if [ "$(value ACTIVE_STORAGE)" = "NONE" ] &&
   [ "$(value STORAGE_READY)" = "0" ] &&
   [ "$(value STORAGE_HEALTH)" = "FAILED" ] &&
   [ "$(value TF1_HEALTH)" = "ERROR" ] &&
   [ "$(value TF2_HEALTH)" = "ERROR" ] &&
   [ "$(value ACTION_REQUIRED)" = "STORAGE_RECOVERY" ] &&
   [ "$(value PROMPT_USER)" = "1" ] &&
   [ -z "$SOURCE" ]; then
    pass "dual filesystem failure enters STORAGE_RECOVERY"
else
    fail "dual filesystem failure recovery state incorrect"
fi

# ===========================================================================
# Restore real machine state
# ===========================================================================

cleanup
trap - EXIT HUP INT TERM

if [ "$(value STORAGE_READY)" = "1" ] &&
   [ "$(value STORAGE_HEALTH)" = "OK" ]; then
    pass "storage health qualification restored healthy runtime state"
else
    fail "storage health qualification cleanup failed"
fi

exit "$fail_count"
EOS
}
