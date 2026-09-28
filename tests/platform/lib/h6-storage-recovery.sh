#!/usr/bin/env bash

run_h6_storage_recovery() {
    local ctl_src
    local host_omit_count
    local remote_omit_count

    ctl_src="$NUBOS_ROOT/board/nuubos/common/rootfs-overlay/usr/sbin/nuubos-storagectl"

    info "storage transaction recovery qualification"

    # -----------------------------------------------------------------------
    # Static/deployed regression for exFAT directory timestamp handling.
    #
    # Four production rsync paths must ignore directory mtimes:
    #   TF1 -> TF2 copy
    #   TF1 -> TF2 verify
    #   TF2 -> TF1 copy
    #   TF2 -> TF1 verify
    # -----------------------------------------------------------------------

    host_omit_count="$(
        grep -c -- '--omit-dir-times' "$ctl_src" 2>/dev/null ||
        true
    )"

    if [[ "$host_omit_count" == "4" ]]; then
        pass "storagectl has --omit-dir-times on all 4 rsync paths"
    else
        fail "storagectl --omit-dir-times count=$host_omit_count expected=4"
    fi

    if ! remote '[ -x /usr/sbin/nuubos-storagectl ]'; then
        skip "installed storagectl unavailable; recovery qualification skipped"
        return 0
    fi

    remote_omit_count="$(
        remote "grep -c -- '--omit-dir-times' /usr/sbin/nuubos-storagectl 2>/dev/null" ||
        true
    )"

    if [[ "$remote_omit_count" == "4" ]]; then
        pass "deployed storagectl has --omit-dir-times on all 4 rsync paths"
    else
        fail "deployed storagectl --omit-dir-times count=${remote_omit_count:-0} expected=4"
        skip "dynamic storage recovery requires deployed current storagectl"
        return 0
    fi

    # -----------------------------------------------------------------------
    # Dynamic recovery matrix.
    #
    # Safety properties:
    # - requires an already-owned TF2;
    # - requires TF1 and TF2 to be exact mirrors before changing anything;
    # - actual sfdisk/mkfs/rereadpt are NEVER executed;
    # - original nuubos.conf and backup-state are restored;
    # - original TF2 ownership marker is backed up and restored on failure;
    # - no reboot or poweroff is performed here.
    # -----------------------------------------------------------------------

    remote_sh <<'EOS' || true
CTL=/usr/sbin/nuubos-storagectl

STATUS=/run/nuubos/storage/tf2.status
TF1=/run/nuubos/storage/tf1
TF2=/dev/mmcblk2p1

STATE=/state/migrations
PENDING_ADOPT=$STATE/tf1-to-tf2.pending
PENDING_SYNC=$STATE/tf2-to-tf1.pending

CONF=/state/config/nuubos.conf
BACKUP_STATE=/state/storage/tf1-backup.state

WORK=/tmp/nuubos-h6-storage-recovery
MNT=$WORK/tf2
BIN=$WORK/bin

CONF_SAVE=$STATE/.h6r-nuubos.conf
MARKER_SAVE=$STATE/.h6r-tf2-marker
BACKUP_SAVE=$STATE/.h6r-backup-state
BACKUP_MISSING=$STATE/.h6r-backup-state-missing

PREFIX=.__nuubos_test_h6r_

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

status_value()
{
    sed -n "s/^${1}=//p" "$STATUS" 2>/dev/null | tail -1
}

abort_test()
{
    fail "$1"
    exit 1
}

mount_tf2_rw()
{
    mkdir -p "$MNT"
    mount -t exfat -o rw,noatime "$TF2" "$MNT"
}

mount_tf2_ro()
{
    mkdir -p "$MNT"
    mount -t exfat -o ro,noatime "$TF2" "$MNT"
}

umount_tf2()
{
    sync
    umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
}

restore_environment()
{
    [ "$cleanup_done" -eq 0 ] || return 0
    cleanup_done=1

    #
    # First force TF1 active so TF2 can be repaired while unmounted.
    #
    "$CTL" storage-mode SINGLE >/dev/null 2>&1 || true

    rm -f \
        "$PENDING_ADOPT" \
        "$PENDING_SYNC"

    rm -f \
        /userdata/${PREFIX}* \
        "$TF1"/${PREFIX}* 2>/dev/null || true

    mkdir -p "$MNT"

    if mount -t exfat -o rw,noatime "$TF2" "$MNT" 2>/dev/null; then
        rm -f "$MNT"/${PREFIX}*

        if [ -f "$MARKER_SAVE" ]; then
            cp "$MARKER_SAVE" "$MNT/.nuubos-userdata"
        fi

        sync
        umount "$MNT" 2>/dev/null || true
    fi

    rmdir "$MNT" 2>/dev/null || true

    if [ -f "$BACKUP_SAVE" ]; then
        mkdir -p "$(dirname "$BACKUP_STATE")"
        cp "$BACKUP_SAVE" "$BACKUP_STATE"
    elif [ -f "$BACKUP_MISSING" ]; then
        rm -f "$BACKUP_STATE"
    fi

    if [ -f "$CONF_SAVE" ]; then
        cp "$CONF_SAVE" "$CONF"
    fi

    rm -f \
        "$CONF_SAVE" \
        "$MARKER_SAVE" \
        "$BACKUP_SAVE" \
        "$BACKUP_MISSING"

    rm -rf "$WORK"

    /etc/init.d/S05nuubos-storage restart >/dev/null 2>&1 || true
}

trap restore_environment EXIT HUP INT TERM

mkdir -p "$STATE" "$WORK"

[ -r "$STATUS" ] ||
    abort_test "storage recovery status unavailable"

[ "$(status_value TF2_STATE)" = "OWNED" ] || {
    skip "storage recovery matrix requires OWNED TF2"
    exit 0
}

CID="$(status_value TF2_CID)"
UUID="$(status_value TF2_UUID)"

[ -n "$CID" ] ||
    abort_test "storage recovery TF2 CID unavailable"

[ -n "$UUID" ] ||
    abort_test "storage recovery TF2 UUID unavailable"

cp "$CONF" "$CONF_SAVE" ||
    abort_test "unable to preserve storage configuration"

if [ -f "$BACKUP_STATE" ]; then
    cp "$BACKUP_STATE" "$BACKUP_SAVE" ||
        abort_test "unable to preserve backup state"
else
    : > "$BACKUP_MISSING"
fi

#
# Use AUTO for the initial mirror check so TF2 is the active source.
#
"$CTL" storage-mode AUTO >/dev/null ||
    abort_test "unable to enter AUTO for recovery qualification"

[ "$(status_value TF2_STATE)" = "OWNED" ] ||
    abort_test "TF2 lost OWNED state during recovery setup"

[ "$(status_value ACTIVE_STORAGE)" = "TF2" ] ||
    abort_test "AUTO did not activate owned TF2"

cp /userdata/.nuubos-userdata "$MARKER_SAVE" ||
    abort_test "unable to preserve TF2 ownership marker"

VERIFY="$(
    rsync \
        -rt \
        --delete-delay \
        --modify-window=1 \
        --omit-dir-times \
        --exclude='/.nuubos-userdata' \
        --dry-run \
        --itemize-changes \
        /userdata/ \
        "$TF1/"
)"

if [ -n "$VERIFY" ]; then
    skip "dynamic recovery matrix skipped: TF1/TF2 are not exact mirrors"
    exit 0
fi

pass "recovery precondition TF1/TF2 mirror exact"

# ===========================================================================
# TF2 -> TF1: PREPARED
# ===========================================================================

T=${PREFIX}sync_prepared

echo H6R-SYNC-PREPARED > "/userdata/$T"
rm -f "$TF1/$T" "$PENDING_SYNC"

cat > "$PENDING_SYNC" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF2_TO_TF1_SYNC
TF2_UUID=$UUID
PHASE=PREPARED
EOF2

"$CTL" sync-to-tf1 >/dev/null ||
    abort_test "TF2->TF1 PREPARED recovery failed"

[ ! -e "$PENDING_SYNC" ] &&
grep -qx H6R-SYNC-PREPARED "$TF1/$T" \
    && pass "TF2->TF1 PREPARED recovery" \
    || abort_test "TF2->TF1 PREPARED recovery validation failed"

rm -f "/userdata/$T" "$TF1/$T"

# ===========================================================================
# TF2 -> TF1: COPYING
# ===========================================================================

T=${PREFIX}sync_copying

echo H6R-SYNC-COPYING > "/userdata/$T"
rm -f "$TF1/$T" "$PENDING_SYNC"

cat > "$PENDING_SYNC" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF2_TO_TF1_SYNC
TF2_UUID=$UUID
PHASE=COPYING
EOF2

"$CTL" sync-to-tf1 >/dev/null ||
    abort_test "TF2->TF1 COPYING recovery failed"

[ ! -e "$PENDING_SYNC" ] &&
grep -qx H6R-SYNC-COPYING "$TF1/$T" \
    && pass "TF2->TF1 COPYING recovery" \
    || abort_test "TF2->TF1 COPYING recovery validation failed"

rm -f "/userdata/$T" "$TF1/$T"

# ===========================================================================
# TF2 -> TF1: VERIFIED after a real selector unmount/remount
# ===========================================================================

T=${PREFIX}sync_verified

echo H6R-SYNC-VERIFIED > "/userdata/$T"
rm -f "$TF1/$T" "$PENDING_SYNC"

"$CTL" sync-to-tf1 >/dev/null ||
    abort_test "unable to construct TF2->TF1 VERIFIED state"

cat > "$PENDING_SYNC" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF2_TO_TF1_SYNC
TF2_UUID=$UUID
PHASE=VERIFIED
EOF2

#
# Real unmount/remount without reboot.
#
"$CTL" storage-mode SINGLE >/dev/null ||
    abort_test "unable to unmount TF2 for VERIFIED recovery"

"$CTL" storage-mode AUTO >/dev/null ||
    abort_test "unable to remount TF2 for VERIFIED recovery"

"$CTL" sync-to-tf1 >/dev/null ||
    abort_test "TF2->TF1 VERIFIED remount recovery failed"

[ ! -e "$PENDING_SYNC" ] &&
grep -qx H6R-SYNC-VERIFIED "/userdata/$T" &&
grep -qx H6R-SYNC-VERIFIED "$TF1/$T" \
    && pass "TF2->TF1 VERIFIED recovery across remount" \
    || abort_test "TF2->TF1 VERIFIED remount validation failed"

rm -f "/userdata/$T" "$TF1/$T"

# ===========================================================================
# TF2 -> TF1: wrong UUID refusal
# ===========================================================================

T=${PREFIX}sync_wrong_uuid

echo H6R-WRONG-UUID > "/userdata/$T"
rm -f "$TF1/$T" "$PENDING_SYNC"

cat > "$PENDING_SYNC" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF2_TO_TF1_SYNC
TF2_UUID=FFFF-FFFF
PHASE=PREPARED
EOF2

if "$CTL" sync-to-tf1 >/dev/null 2>&1; then
    abort_test "TF2->TF1 wrong UUID transaction accepted"
fi

[ ! -e "$TF1/$T" ] \
    && pass "TF2->TF1 wrong UUID refused before copy" \
    || abort_test "TF2->TF1 wrong UUID modified TF1"

rm -f "$PENDING_SYNC" "/userdata/$T" "$TF1/$T"

# ===========================================================================
# TF2 -> TF1: insufficient capacity refusal
# ===========================================================================

T=${PREFIX}sync_no_space

echo H6R-NO-SPACE > "/userdata/$T"
rm -f "$TF1/$T" "$PENDING_SYNC"

rm -rf "$BIN"
mkdir -p "$BIN"

REAL_DF="$(command -v df)"

cat > "$BIN/df" <<EOF2
#!/bin/sh
if [ "\${1:-}" = "-Pk" ]; then
    cat <<OUT
Filesystem 1024-blocks Used Available Capacity Mounted on
/dev/fake 1024 0 1024 0% $TF1
OUT
else
    exec "$REAL_DF" "\$@"
fi
EOF2

chmod 755 "$BIN/df"

if PATH="$BIN:$PATH" "$CTL" sync-to-tf1 >/dev/null 2>&1; then
    abort_test "TF2->TF1 insufficient capacity accepted"
fi

[ ! -e "$TF1/$T" ] &&
[ ! -e "$PENDING_SYNC" ] \
    && pass "TF2->TF1 insufficient capacity refused before transaction" \
    || abort_test "TF2->TF1 capacity refusal modified destination"

rm -rf "$BIN"
rm -f "/userdata/$T" "$TF1/$T"

# ===========================================================================
# Adoption recovery requires TF1 to be authoritative.
#
# Mirror is exact at this point, so switching to SINGLE cannot lose data.
# ===========================================================================

"$CTL" storage-mode SINGLE >/dev/null ||
    abort_test "unable to enter SINGLE for adoption recovery"

[ "$(status_value ACTIVE_STORAGE)" = "TF1" ] ||
    abort_test "SINGLE did not activate TF1"

make_tf2_foreign()
{
    mount_tf2_rw || return 1

    rm -f "$MNT/.nuubos-userdata"

    sync
    umount_tf2

    /etc/init.d/S05nuubos-storage restart >/dev/null || return 1

    [ "$(status_value TF2_STATE)" = "FOREIGN" ]
}

check_tf2_text()
{
    FILE="$1"
    TEXT="$2"

    mount_tf2_ro || return 1

    grep -qx "$TEXT" "$MNT/$FILE"
    RC=$?

    umount_tf2
    return "$RC"
}

remove_test_both()
{
    FILE="$1"

    rm -f "/userdata/$FILE" "$TF1/$FILE" 2>/dev/null || true

    if mount_tf2_rw 2>/dev/null; then
        rm -f "$MNT/$FILE"
        sync
        umount_tf2
    fi
}

# ===========================================================================
# TF1 -> TF2: FORMATTED
# ===========================================================================

T=${PREFIX}adopt_formatted

echo H6R-ADOPT-FORMATTED > "/userdata/$T"

make_tf2_foreign ||
    abort_test "unable to construct FORMATTED foreign TF2 state"

cat > "$PENDING_ADOPT" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF1_TO_TF2_ADOPT
TF2_CID=$CID
PHASE=FORMATTED
EOF2

"$CTL" adopt-tf2 --confirm-erase-cid "$CID" >/dev/null ||
    abort_test "TF1->TF2 FORMATTED recovery failed"

[ ! -e "$PENDING_ADOPT" ] &&
[ "$(status_value TF2_STATE)" = "OWNED" ] &&
check_tf2_text "$T" H6R-ADOPT-FORMATTED \
    && pass "TF1->TF2 FORMATTED recovery" \
    || abort_test "TF1->TF2 FORMATTED validation failed"

remove_test_both "$T"

# ===========================================================================
# TF1 -> TF2: COPYING
# ===========================================================================

T=${PREFIX}adopt_copying
STALE=${PREFIX}adopt_partial_stale

echo H6R-ADOPT-COPYING > "/userdata/$T"

mount_tf2_rw ||
    abort_test "unable to prepare COPYING state"

rm -f "$MNT/.nuubos-userdata" "$MNT/$T"
echo H6R-STALE > "$MNT/$STALE"

sync
umount_tf2

/etc/init.d/S05nuubos-storage restart >/dev/null ||
    abort_test "unable to publish COPYING foreign state"

cat > "$PENDING_ADOPT" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF1_TO_TF2_ADOPT
TF2_CID=$CID
PHASE=COPYING
EOF2

"$CTL" adopt-tf2 --confirm-erase-cid "$CID" >/dev/null ||
    abort_test "TF1->TF2 COPYING recovery failed"

mount_tf2_ro ||
    abort_test "unable to validate COPYING target"

COPY_OK=0

if grep -qx H6R-ADOPT-COPYING "$MNT/$T" &&
   [ ! -e "$MNT/$STALE" ] &&
   [ ! -e "$PENDING_ADOPT" ] &&
   [ "$(status_value TF2_STATE)" = "OWNED" ]; then
    COPY_OK=1
fi

umount_tf2

[ "$COPY_OK" -eq 1 ] \
    && pass "TF1->TF2 COPYING recovery removes stale partial data" \
    || abort_test "TF1->TF2 COPYING validation failed"

remove_test_both "$T"
remove_test_both "$STALE"

# ===========================================================================
# TF1 -> TF2: VERIFIED after a real remount
# ===========================================================================

mount_tf2_rw ||
    abort_test "unable to construct VERIFIED target"

rm -f "$MNT/.nuubos-userdata"

rsync \
    -rt \
    --delete \
    --modify-window=1 \
    --omit-dir-times \
    --exclude='/.nuubos-userdata' \
    /userdata/ \
    "$MNT/" ||
    abort_test "unable to construct VERIFIED mirror"

VERIFY="$(
    rsync \
        -rt \
        --delete \
        --modify-window=1 \
        --omit-dir-times \
        --exclude='/.nuubos-userdata' \
        --dry-run \
        --itemize-changes \
        /userdata/ \
        "$MNT/"
)"

[ -z "$VERIFY" ] ||
    abort_test "synthetic VERIFIED mirror is not exact"

umount_tf2

/etc/init.d/S05nuubos-storage restart >/dev/null ||
    abort_test "unable to publish VERIFIED foreign state"

cat > "$PENDING_ADOPT" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF1_TO_TF2_ADOPT
TF2_CID=$CID
PHASE=VERIFIED
EOF2

"$CTL" adopt-tf2 --confirm-erase-cid "$CID" >/dev/null ||
    abort_test "TF1->TF2 VERIFIED remount recovery failed"

[ ! -e "$PENDING_ADOPT" ] &&
[ "$(status_value TF2_STATE)" = "OWNED" ] \
    && pass "TF1->TF2 VERIFIED recovery across remount" \
    || abort_test "TF1->TF2 VERIFIED recovery validation failed"

# ===========================================================================
# TF1 -> TF2: OWNED
# ===========================================================================

cat > "$PENDING_ADOPT" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF1_TO_TF2_ADOPT
TF2_CID=$CID
PHASE=OWNED
EOF2

"$CTL" adopt-tf2 --confirm-erase-cid "$CID" >/dev/null ||
    abort_test "TF1->TF2 OWNED recovery failed"

[ ! -e "$PENDING_ADOPT" ] &&
[ "$(status_value TF2_STATE)" = "OWNED" ] &&
[ "$(status_value ACTIVE_STORAGE)" = "TF1" ] \
    && pass "TF1->TF2 OWNED recovery finalizes without migration" \
    || abort_test "TF1->TF2 OWNED recovery validation failed"

# ===========================================================================
# TF1 -> TF2: PREPARED
#
# The control flow is real, but the only destructive commands are mocked:
#   sfdisk
#   mkfs.exfat
#   blockdev --rereadpt
# ===========================================================================

T=${PREFIX}adopt_prepared

echo H6R-ADOPT-PREPARED > "/userdata/$T"

make_tf2_foreign ||
    abort_test "unable to construct PREPARED foreign state"

rm -rf "$BIN"
mkdir -p "$BIN"

REAL_BLOCKDEV="$(command -v blockdev)"

cat > "$BIN/sfdisk" <<'EOF2'
#!/bin/sh
cat >/dev/null
exit 0
EOF2

cat > "$BIN/mkfs.exfat" <<'EOF2'
#!/bin/sh
exit 0
EOF2

cat > "$BIN/blockdev" <<EOF2
#!/bin/sh
if [ "\${1:-}" = "--rereadpt" ]; then
    exit 0
fi
exec "$REAL_BLOCKDEV" "\$@"
EOF2

chmod 755 \
    "$BIN/sfdisk" \
    "$BIN/mkfs.exfat" \
    "$BIN/blockdev"

cat > "$PENDING_ADOPT" <<EOF2
NUUBOS_MIGRATION_VERSION=1
MIGRATION_TYPE=TF1_TO_TF2_ADOPT
TF2_CID=$CID
PHASE=PREPARED
EOF2

PATH="$BIN:$PATH" \
"$CTL" adopt-tf2 --confirm-erase-cid "$CID" >/dev/null ||
    abort_test "TF1->TF2 PREPARED mocked recovery failed"

[ ! -e "$PENDING_ADOPT" ] &&
[ "$(status_value TF2_STATE)" = "OWNED" ] &&
check_tf2_text "$T" H6R-ADOPT-PREPARED \
    && pass "TF1->TF2 PREPARED control-flow with destructive operations mocked" \
    || abort_test "TF1->TF2 PREPARED validation failed"

remove_test_both "$T"
rm -rf "$BIN"

# ===========================================================================
# Restore original machine state and validate cleanup.
# ===========================================================================

restore_environment
trap - EXIT HUP INT TERM

ORIGINAL_MODE="$(
    sed -n 's/^STORAGE_MODE=//p' "$CONF"
)"

[ -n "$ORIGINAL_MODE" ] ||
    abort_test "restored storage mode missing"

[ "$(status_value STORAGE_MODE)" = "$ORIGINAL_MODE" ] &&
[ "$(status_value TF2_STATE)" = "OWNED" ] \
    && pass "storage recovery qualification restored original selector state" \
    || fail "storage recovery qualification did not restore selector state"

if find "$STATE" -maxdepth 1 -type f \
    \( -name 'tf1-to-tf2.pending' -o -name 'tf2-to-tf1.pending' \) |
    grep -q .
then
    fail "storage recovery qualification left pending transactions"
else
    pass "storage recovery qualification left no pending transactions"
fi

exit "$fail_count"
EOS
}
