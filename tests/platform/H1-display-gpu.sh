#!/usr/bin/env bash
set -uo pipefail
AREA="H1"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

remote "[ -e '$DRM_CARD' ]" \
    && pass "$DRM_CARD present" \
    || fail "$DRM_CARD missing"

remote "[ -e '$DRM_RENDER' ]" \
    && pass "$DRM_RENDER present" \
    || fail "$DRM_RENDER missing"

gpu_driver="$(remote "basename \"\$(readlink -f /sys/class/drm/renderD128/device/driver 2>/dev/null)\"" 2>/dev/null || true)"
if [[ "$gpu_driver" == "panfrost" ]]; then
    pass "Panfrost bound to renderD128"
else
    fail "renderD128 driver is '${gpu_driver:-unknown}', expected panfrost"
fi

hdmi_status="$(remote "
for f in $HDMI_STATUS_GLOB; do
    [ -e \"\$f\" ] || continue
    cat \"\$f\"
    break
done
" 2>/dev/null || true)"

if [[ "$REQUIRE_HDMI_CONNECTED" == "1" ]]; then
    [[ "$hdmi_status" == "connected" ]] \
        && pass "HDMI connected" \
        || fail "HDMI status '${hdmi_status:-missing}'"
else
    [[ -n "$hdmi_status" ]] \
        && pass "HDMI connector present ($hdmi_status)" \
        || fail "HDMI connector missing"
fi

if remote "find /sys/kernel/iommu_groups -type l -name '$DISPLAY_IOMMU_DEVICE' | grep -q ."; then
    pass "display mixer attached to IOMMU"
else
    fail "display mixer not attached to IOMMU"
fi

faults="$(iommu_fault_count)"
if [[ "$faults" == "0" ]]; then
    pass "IOMMU page faults = 0"
else
    fail "IOMMU page faults = $faults"
fi

if [[ "$HAS_INTERNAL_LCD" == "1" ]]; then
    lcd_status="$(remote "
for f in $INTERNAL_LCD_STATUS_GLOB; do
    [ -e \"\$f\" ] || continue
    cat \"\$f\"
    break
done
" 2>/dev/null || true)"

    if [[ "$lcd_status" == "connected" ]]; then
        pass "internal LCD connected"
    else
        fail "internal LCD status '${lcd_status:-missing}'"
    fi

    if remote "[ -d '$BACKLIGHT_DEVICE' ]"; then
        pass "backlight device present ($BACKLIGHT_DEVICE)"
    else
        fail "backlight device missing ($BACKLIGHT_DEVICE)"
    fi

    backlight_max="$(
        remote "cat '$BACKLIGHT_DEVICE/max_brightness' 2>/dev/null"             2>/dev/null || true
    )"

    if [[ "$backlight_max" == "$BACKLIGHT_EXPECT_MAX" ]]; then
        pass "backlight max brightness = $backlight_max"
    else
        fail "backlight max brightness '${backlight_max:-missing}', expected $BACKLIGHT_EXPECT_MAX"
    fi

    backlight_brightness="$(
        remote "cat '$BACKLIGHT_DEVICE/brightness' 2>/dev/null"             2>/dev/null || true
    )"

    if [[ "$backlight_brightness" =~ ^[0-9]+$ &&
          "$backlight_max" =~ ^[0-9]+$ ]] &&
       (( backlight_brightness >= 0 &&
          backlight_brightness <= backlight_max )); then
        pass "backlight brightness in range ($backlight_brightness/$backlight_max)"
    else
        fail "backlight brightness '${backlight_brightness:-missing}' outside valid range"
    fi
else
    skip "internal LCD (device has no internal panel)"
fi
