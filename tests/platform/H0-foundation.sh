#!/usr/bin/env bash
set -uo pipefail
AREA="H0"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

arch="$(remote 'uname -m')"
kernel="$(remote 'uname -r')"

[[ "$arch" == "$EXPECTED_ARCH" ]] \
    && pass "architecture $arch" \
    || fail "architecture $arch, expected $EXPECTED_ARCH"

[[ "$kernel" == "$EXPECTED_KERNEL_PREFIX"* ]] \
    && pass "kernel $kernel" \
    || fail "kernel $kernel, expected ${EXPECTED_KERNEL_PREFIX}*"

remote '[ -r /proc/device-tree/compatible ]' \
    && pass "device tree active" \
    || fail "device tree unavailable"

model="$(remote 'tr -d "\\000" </proc/device-tree/model 2>/dev/null' || true)"
compatible="$(remote 'tr "\\000" "\\n" </proc/device-tree/compatible 2>/dev/null' || true)"

[[ -z "${EXPECTED_MODEL:-}" || "$model" == "$EXPECTED_MODEL" ]] \
    && pass "device-tree model ${model:-available}" \
    || fail "device-tree model '${model:-missing}', expected '$EXPECTED_MODEL'"

if [[ -z "${EXPECTED_COMPATIBLE:-}" ]]; then
    skip "profile-compatible identity not specified"
elif grep -Fxq "$EXPECTED_COMPATIBLE" <<< "$compatible"; then
    pass "device-tree compatible $EXPECTED_COMPATIBLE"
else
    fail "device-tree compatible '$EXPECTED_COMPATIBLE' missing"
fi

remote '[ -d /sys/kernel/debug ]' \
    && pass "debugfs mountpoint present" \
    || warn "debugfs mountpoint missing"

fatal="$(remote 'dmesg | grep -Ei "Kernel panic|Oops:|Kernel BUG at|BUG: unable to handle|BUG: kernel NULL pointer dereference|Unable to handle kernel paging request|Unable to handle kernel NULL pointer dereference|Internal error: Oops" | tail -n 20' || true)"
if [[ -z "$fatal" ]]; then
    pass "no fatal kernel signatures"
else
    fail "fatal kernel signature found"
    printf '%s\n' "$fatal"
fi
