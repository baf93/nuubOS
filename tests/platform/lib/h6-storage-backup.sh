#!/usr/bin/env bash

run_h6_storage_backup() {
    info "storage backup scheduler qualification"

    remote_sh <<'EOS' || true
CTL=/usr/sbin/nuubos-storagectl
CONF=/state/config/nuubos.conf
STATE=/state/storage/tf1-backup.state

WORK=/tmp/h6-backup-boundary
BIN=$WORK/bin
CONF_SAVE=$WORK/nuubos.conf
STATE_SAVE=$WORK/tf1-backup.state

NOW=1800000000

fail_count=0
state_existed=0
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

cleanup()
{
    [ "$cleanup_done" -eq 0 ] || return 0
    cleanup_done=1

    if [ -f "$CONF_SAVE" ]; then
        cp "$CONF_SAVE" "$CONF"
    fi

    if [ "$state_existed" -eq 1 ]; then
        cp "$STATE_SAVE" "$STATE"
    else
        rm -f "$STATE"
    fi

    rm -rf "$WORK"
}

trap cleanup EXIT HUP INT TERM

rm -rf "$WORK"
mkdir -p "$BIN"

cp "$CONF" "$CONF_SAVE" || {
    fail "unable to preserve backup configuration"
    exit "$fail_count"
}

if [ -f "$STATE" ]; then
    cp "$STATE" "$STATE_SAVE" || {
        fail "unable to preserve backup state"
        exit "$fail_count"
    }

    state_existed=1
fi

REAL_DATE="$(command -v date)"

cat > "$BIN/date" <<EOF2
#!/bin/sh

if [ "\${1:-}" = "+%s" ]; then
    echo $NOW
else
    exec "$REAL_DATE" "\$@"
fi
EOF2

chmod 755 "$BIN/date"

test_boundary()
{
    POLICY="$1"
    DELTA="$2"
    EXPECTED="$3"

    LAST=$((NOW - DELTA))

    "$CTL" backup-policy "$POLICY" >/dev/null || {
        fail "$POLICY policy configuration failed"
        return
    }

    mkdir -p "$(dirname "$STATE")"

    cat > "$STATE" <<EOF2
NUUBOS_BACKUP_VERSION=1
LAST_SUCCESS_EPOCH=$LAST
LAST_SOURCE_UUID=3D05-D8A9
EOF2

    OUT="$(
        PATH="$BIN:$PATH" \
            "$CTL" backup-status
    )"

    DUE="$(
        printf '%s\n' "$OUT" |
        sed -n 's/^TF1_BACKUP_DUE=//p'
    )"

    REASON="$(
        printf '%s\n' "$OUT" |
        sed -n 's/^TF1_BACKUP_REASON=//p'
    )"

    if [ "$DUE" = "$EXPECTED" ]; then
        pass "$POLICY backup boundary delta=$DELTA due=$DUE reason=$REASON"
    else
        fail "$POLICY backup boundary delta=$DELTA due=${DUE:-missing} expected=$EXPECTED"
    fi
}

test_boundary DAILY    86399   0
test_boundary DAILY    86400   1

test_boundary WEEKLY   604799  0
test_boundary WEEKLY   604800  1

test_boundary MONTHLY  2591999 0
test_boundary MONTHLY  2592000 1

cleanup
trap - EXIT HUP INT TERM

exit "$fail_count"
EOS
}
