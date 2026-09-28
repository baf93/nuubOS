#!/usr/bin/env bash
set -uo pipefail

AREA="H8"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

if [[ "${HAS_RGB:-0}" != "1" ]]; then
    skip "RGB capability not present/qualified on this profile"
    exit 0
fi

remote '[ -x /usr/sbin/nuubos-rgbd ]' \
    && pass "nuubos-rgbd installed" \
    || fail "nuubos-rgbd missing"

remote '[ -x /usr/bin/nuubos-rgbctl ]' \
    && pass "nuubos-rgbctl installed" \
    || fail "nuubos-rgbctl missing"

remote 'pidof nuubos-rgbd >/dev/null 2>&1' \
    && pass "nuubos-rgbd running" \
    || fail "nuubos-rgbd not running"

remote '[ -S /run/nuubos/rgbd.sock ]' \
    && pass "RGB control socket present" \
    || fail "RGB control socket missing"

status="$(remote 'nuubos-rgbctl status' || true)"

[[ "$status" == *"supported=1"* ]] \
    && pass "RGB capability supported" \
    || fail "RGB status does not report supported=1: $status"

[[ "$status" == *"device=${RGB_DEVICE_ID}"* ]] \
    && pass "RGB device id ${RGB_DEVICE_ID}" \
    || fail "RGB device id mismatch: $status"

[[ "$status" == *"leds=${RGB_LED_COUNT}"* ]] \
    && pass "RGB LED count ${RGB_LED_COUNT}" \
    || fail "RGB LED count mismatch: $status"

[[ "$status" == *"zones=${RGB_ZONE_COUNT}"* ]] \
    && pass "RGB zone count ${RGB_ZONE_COUNT}" \
    || fail "RGB zone count mismatch: $status"

for ((i = 0; i < RGB_ZONE_COUNT; i++)); do
    var="RGB_ZONE${i}"
    expected="${!var:-}"

    if [[ -z "$expected" ]]; then
        fail "RGB zone${i} topology missing from profile"
        continue
    fi

    [[ "$status" == *"zone${i}=${expected}"* ]] \
        && pass "RGB zone${i} topology ${expected}" \
        || fail "RGB zone${i} topology mismatch: $status"
done

if [[ "${RGB_EXPECT_BOOT_OFF:-0}" == "1" ]]; then
    [[ "$status" == *"mode=off"* ]] \
        && pass "RGB mode off at regression baseline" \
        || fail "RGB expected mode=off: $status"

    [[ "$status" == *"backend=off"* ]] \
        && pass "RGB hardware backend inactive" \
        || fail "RGB expected backend=off: $status"
fi

if remote '[ -r /sys/kernel/debug/gpio ]'; then
    gpio="$(remote 'cat /sys/kernel/debug/gpio' || true)"

    if grep -Fq "rgb:kbd_backlight" <<< "$gpio"; then
        fail "obsolete rgb:kbd_backlight GPIO consumer still present"
    else
        pass "obsolete rgb:kbd_backlight consumer absent"
    fi

    if grep -Eq 'gpio-133 .*nuubos-rgbd|gpio-263 .*nuubos-rgbd' <<< "$gpio"; then
        fail "RGB GPIOs claimed while backend is off"
    else
        pass "RGB PE5/PI7 unclaimed while backend is off"
    fi
else
    warn "debugfs GPIO state unavailable"
fi

# Physical static/breathe/rainbow/per-LED-frame tests are intentionally not
# triggered here. They are acceptance tests, not non-invasive regressions.
