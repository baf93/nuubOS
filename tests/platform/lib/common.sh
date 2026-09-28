#!/usr/bin/env bash

: "${NUBOS_ROOT:?NUBOS_ROOT missing}"
: "${NUBOS_TARGET:?NUBOS_TARGET missing}"
: "${NUBOS_PROFILE_FILE:?NUBOS_PROFILE_FILE missing}"
: "${NUBOS_TEST_MODE:?NUBOS_TEST_MODE missing}"

# shellcheck disable=SC1090
source "$NUBOS_PROFILE_FILE"

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=7 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)

info() { printf '[INFO] [%s] %s\n' "$AREA" "$*"; }
pass() { printf '[PASS] [%s] %s\n' "$AREA" "$*"; }
warn() { printf '[WARN] [%s] %s\n' "$AREA" "$*"; }
fail() { printf '[FAIL] [%s] %s\n' "$AREA" "$*"; }
skip() { printf '[SKIP] [%s] %s\n' "$AREA" "$*"; }

remote() {
    ssh "${SSH_OPTS[@]}" "$NUBOS_TARGET" "$@"
}

remote_sh() {
    ssh "${SSH_OPTS[@]}" "$NUBOS_TARGET" 'sh -s'
}

remote_has() {
    remote "command -v '$1' >/dev/null 2>&1"
}

remote_file_exists() {
    remote "[ -e '$1' ]"
}

remote_put() {
    local src="$1"
    local dst="$2"
    cat "$src" | ssh "${SSH_OPTS[@]}" "$NUBOS_TARGET" "cat > '$dst'"
}

irq_sum() {
    local pattern="$1"
    remote "awk '/$pattern/ {s=0; for(i=2;i<=5;i++) s+=\$i; print s}' /proc/interrupts"
}

iommu_fault_count() {
    remote 'dmesg | grep -c "sun50i-iommu.*Page fault"'
}

ve_clocks_idle() {
    remote '
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
grep -E "pll-ve|bus-ve|mbus-ve|[[:space:]]ve[[:space:]]" /sys/kernel/debug/clk/clk_summary 2>/dev/null
'
}

ensure_test_vectors() {
    local needed=("$@")
    local missing=0
    local f

    for f in "${needed[@]}"; do
        [[ -f "$NUBOS_TEST_VECTOR_DIR/$f" ]] || missing=1
    done

    ((missing == 0)) && return 0

    if [[ ! -x "$NUBOS_ROOT/scripts/test-vectors.sh" ]]; then
        return 1
    fi

    "$NUBOS_ROOT/scripts/test-vectors.sh" "${needed[@]}" >/dev/null
}

sync_test_vector() {
    local name="$1"
    local src="$NUBOS_TEST_VECTOR_DIR/$name"
    local dst="/tmp/$name"

    [[ -f "$src" ]] || return 1

    local host_hash remote_hash
    host_hash="$(sha256sum "$src" | awk '{print $1}')"
    remote_hash="$(remote "sha256sum '$dst' 2>/dev/null | awk '{print \$1}'" 2>/dev/null || true)"

    if [[ "$host_hash" != "$remote_hash" ]]; then
        remote_put "$src" "$dst" || return 1
        remote_hash="$(remote "sha256sum '$dst' | awk '{print \$1}'")"
    fi

    [[ "$host_hash" == "$remote_hash" ]]
}

run_decode_test() {
    local file="$1"
    local parser="$2"
    local caps="$3"
    local decoder="$4"
    local expected_irq="$5"
    local label="$6"

    local before after delta rc

    before="$(irq_sum '1c0e000.video-codec')"
    before="${before:-0}"

    remote "
gst-launch-1.0 -q \
  filesrc location='/tmp/$file' \
  ! $parser \
  ! '$caps' \
  ! $decoder video-device=/dev/video0 media-device=/dev/media0 \
  ! fakesink sync=false
"
    rc=$?

    after="$(irq_sum '1c0e000.video-codec')"
    after="${after:-0}"
    delta=$((after - before))

    if ((rc == 0)) && ((delta == expected_irq)); then
        pass "$label (IRQ $delta/$expected_irq)"
        return 0
    fi

    fail "$label (rc=$rc IRQ=$delta expected=$expected_irq)"
    return 1
}
