#!/usr/bin/env bash
set -uo pipefail

AREA="H5"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

event_for_name() {
    local name="$1"

    remote "
for e in /sys/class/input/event*; do
    [ -e \"\$e\" ] || continue

    n=\$(cat \"\$e/device/name\" 2>/dev/null || true)

    if [ \"\$n\" = '$name' ]; then
        basename \"\$e\"
        exit 0
    fi
done

exit 1
"
}

capability() {
    local event="$1"
    local type="$2"

    remote "cat '/sys/class/input/$event/device/capabilities/$type' 2>/dev/null"
}

cap_has_bit() {
    local bitmap="$1"
    local bit="$2"

    local -a words
    local word_index bit_index array_index word

    read -ra words <<< "$bitmap"

    word_index=$((bit / 64))
    bit_index=$((bit % 64))
    array_index=$((${#words[@]} - 1 - word_index))

    ((array_index >= 0)) || return 1

    word="${words[$array_index]}"

    (( (16#$word & (1 << bit_index)) != 0 ))
}

check_key_set() {
    local event="$1"
    local label="$2"
    shift 2

    local bitmap item code name
    local -a missing=()

    bitmap="$(capability "$event" key || true)"

    if [[ -z "$bitmap" ]]; then
        fail "$label key capability unavailable"
        return
    fi

    for item in "$@"; do
        code="${item%%:*}"
        name="${item#*:}"

        if [[ "$code" == "317" && "${INPUT_HAS_L3:-1}" != "1" ]]; then
            continue
        fi

        if [[ "$code" == "318" && "${INPUT_HAS_R3:-1}" != "1" ]]; then
            continue
        fi

        if ! cap_has_bit "$bitmap" "$code"; then
            missing+=("$name($code)")
        fi
    done

    if ((${#missing[@]} == 0)); then
        pass "$label expected key capabilities"
    else
        fail "$label missing keys: ${missing[*]}"
    fi
}

check_abs_set() {
    local event="$1"
    local bitmap
    local -a missing=()

    bitmap="$(capability "$event" abs || true)"

    [[ -n "$bitmap" ]] || {
        fail "analog ABS capability unavailable"
        return
    }

    cap_has_bit "$bitmap" 0 || missing+=("ABS_X")
    cap_has_bit "$bitmap" 1 || missing+=("ABS_Y")

    if [[ "${INPUT_HAS_RIGHT_STICK:-1}" == "1" ]]; then
        cap_has_bit "$bitmap" 3 || missing+=("ABS_RX")
        cap_has_bit "$bitmap" 4 || missing+=("ABS_RY")
    fi

    if ((${#missing[@]} == 0)); then
        if [[ "${INPUT_HAS_RIGHT_STICK:-1}" == "1" ]]; then
            pass "analog ABS_X/ABS_Y/ABS_RX/ABS_RY capabilities"
        else
            pass "analog ABS_X/ABS_Y capabilities"
        fi
    else
        fail "analog missing axes: ${missing[*]}"
    fi
}

bitmap_nonzero() {
    local bitmap="$1"
    local word

    for word in $bitmap; do
        [[ "$word" =~ ^0+$ ]] || return 0
    done

    return 1
}

check_nonzero_capability() {
    local event="$1"
    local type="$2"
    local label="$3"
    local bitmap

    bitmap="$(capability "$event" "$type" || true)"

    if bitmap_nonzero "$bitmap"; then
        pass "$label"
    else
        fail "$label"
    fi
}

check_ff_rumble() {
    local event="$1"
    local bitmap

    # FF_RUMBLE = 0x50 (80).
    bitmap="$(capability "$event" ff || true)"
    if [[ -n "$bitmap" ]] && cap_has_bit "$bitmap" 80; then
        pass "external controller exposes FF_RUMBLE"
    else
        fail "external controller does not expose FF_RUMBLE"
    fi
}

check_internal_rumble() {
    local event="$1"
    local ev_bitmap ff_bitmap

    # EV_FF = 0x15 (21).
    ev_bitmap="$(capability "$event" ev || true)"
    if [[ -n "$ev_bitmap" ]] && cap_has_bit "$ev_bitmap" 21; then
        pass "internal rumble device exposes EV_FF"
    else
        fail "internal rumble device does not expose EV_FF"
    fi

    # FF_RUMBLE = 0x50 (80).
    ff_bitmap="$(capability "$event" ff || true)"
    if [[ -n "$ff_bitmap" ]] && cap_has_bit "$ff_bitmap" 80; then
        pass "internal rumble device exposes FF_RUMBLE"
    else
        fail "internal rumble device does not expose FF_RUMBLE"
    fi

    if remote '
dmesg |
grep -Ei \
"(pwm-vibrator|sun8i-pwm).*(fail|failed|error|timeout|pending)|PH0 already requested" |
grep -q .
'; then
        fail "internal rumble failure signature present in dmesg"
    else
        pass "no internal rumble failure signature in dmesg"
    fi
}

iio_name_present() {
    local name="$1"

    remote "
for d in /sys/bus/iio/devices/iio:device*; do
    [ -e \"\$d\" ] || continue

    n=\$(cat \"\$d/name\" 2>/dev/null || true)

    [ \"\$n\" = '$name' ] && exit 0
done

exit 1
"
}

# ---------------------------------------------------------------------------
# Input enumeration
# ---------------------------------------------------------------------------

count="$(remote 'find /dev/input -maxdepth 1 -name "event*" 2>/dev/null | wc -l' || true)"
count="${count:-0}"

if ((count >= INPUT_MIN_EVENT_NODES)); then
    pass "input event nodes present ($count >= $INPUT_MIN_EVENT_NODES)"
else
    fail "input event nodes insufficient ($count < $INPUT_MIN_EVENT_NODES)"
fi

power_event="$(event_for_name "$INPUT_POWER_NAME" || true)"
analog_event="$(event_for_name "$INPUT_ANALOG_NAME" || true)"
gamepad_event="$(event_for_name "$INPUT_GAMEPAD_NAME" || true)"
volume_event="$(event_for_name "$INPUT_VOLUME_NAME" || true)"

[[ -n "$power_event" ]] \
    && pass "$INPUT_POWER_NAME present ($power_event)" \
    || fail "$INPUT_POWER_NAME missing"

[[ -n "$analog_event" ]] \
    && pass "$INPUT_ANALOG_NAME present ($analog_event)" \
    || fail "$INPUT_ANALOG_NAME missing"

[[ -n "$gamepad_event" ]] \
    && pass "$INPUT_GAMEPAD_NAME present ($gamepad_event)" \
    || fail "$INPUT_GAMEPAD_NAME missing"

[[ -n "$volume_event" ]] \
    && pass "$INPUT_VOLUME_NAME present ($volume_event)" \
    || fail "$INPUT_VOLUME_NAME missing"

# ---------------------------------------------------------------------------
# Internal platform rumble
#
# This is distinct from force feedback exposed by an optional external
# controller. Devices that require an internal vibration motor declare it
# explicitly in their platform profile.
# ---------------------------------------------------------------------------

if [[ "${INPUT_REQUIRE_INTERNAL_RUMBLE:-0}" == "1" ]]; then
    internal_rumble_name="${INPUT_INTERNAL_RUMBLE_NAME:-pwm-vibrator}"
    internal_rumble_event="$(event_for_name "$internal_rumble_name" || true)"

    if [[ -n "$internal_rumble_event" ]]; then
        pass "$internal_rumble_name present ($internal_rumble_event)"
        check_internal_rumble "$internal_rumble_event"
        skip "internal rumble physical confirmation remains manual"
    else
        fail "$internal_rumble_name missing"
    fi
else
    skip "internal rumble not required by platform profile"
fi

# ---------------------------------------------------------------------------
# Userspace HID bridge
#
# Required for Bluetooth HOG devices handled by BlueZ through /dev/uhid.
# ---------------------------------------------------------------------------

if [[ "${INPUT_REQUIRE_UHID:-0}" == "1" ]]; then
    if remote 'test -c /dev/uhid'; then
        pass "/dev/uhid character device present"
    else
        fail "/dev/uhid character device missing"
    fi
else
    skip "UHID runtime capability not required by platform profile"
fi

# ---------------------------------------------------------------------------
# Power
# KEY_POWER = 116
# ---------------------------------------------------------------------------

if [[ -n "$power_event" ]]; then
    check_key_set "$power_event" "power button" \
        "116:KEY_POWER"
fi

# ---------------------------------------------------------------------------
# Analog sticks
#
# ABS_X  = 0
# ABS_Y  = 1
# ABS_RX = 3
# ABS_RY = 4
# ---------------------------------------------------------------------------

if [[ -n "$analog_event" ]]; then
    check_abs_set "$analog_event"

    poll="$(remote "cat '/sys/class/input/$analog_event/device/poll' 2>/dev/null" || true)"

    if [[ "$poll" == "$INPUT_ANALOG_POLL_MS" ]]; then
        pass "analog poll interval ${poll}ms"
    else
        fail "analog poll interval ${poll:-missing}ms expected ${INPUT_ANALOG_POLL_MS}ms"
    fi
fi

# ---------------------------------------------------------------------------
# Built-in gamepad
#
# BTN_SOUTH  304  B
# BTN_EAST   305  A
# BTN_NORTH  307  X
# BTN_WEST   308  Y
# BTN_TL     310  L1
# BTN_TR     311  R1
# BTN_TL2    312  L2
# BTN_TR2    313  R2
# BTN_SELECT 314
# BTN_START  315
# BTN_MODE   316
# BTN_THUMBL 317  L3
# BTN_THUMBR 318  R3
# DPAD       544-547
# ---------------------------------------------------------------------------

if [[ -n "$gamepad_event" ]]; then
    check_key_set "$gamepad_event" "built-in gamepad" \
        "304:BTN_SOUTH" \
        "305:BTN_EAST" \
        "307:BTN_NORTH" \
        "308:BTN_WEST" \
        "310:BTN_TL" \
        "311:BTN_TR" \
        "312:BTN_TL2" \
        "313:BTN_TR2" \
        "314:BTN_SELECT" \
        "315:BTN_START" \
        "316:BTN_MODE" \
        "317:BTN_THUMBL" \
        "318:BTN_THUMBR" \
        "544:BTN_DPAD_UP" \
        "545:BTN_DPAD_DOWN" \
        "546:BTN_DPAD_LEFT" \
        "547:BTN_DPAD_RIGHT"
fi

# ---------------------------------------------------------------------------
# External controller acceptance
#
# Optional by design: no particular external controller is a platform
# requirement. To validate a connected controller:
#
#   NUBOS_EXTERNAL_INPUT_NAME="Controller name" ./scripts/test-platform.sh H5
#
# EV_KEY is required for a game controller. EV_ABS is reported when present
# but is not mandatory because digital-only controllers are valid devices.
# ---------------------------------------------------------------------------

external_name="${NUBOS_EXTERNAL_INPUT_NAME:-}"

if [[ -n "$external_name" ]]; then
    external_event="$(event_for_name "$external_name" || true)"

    if [[ -n "$external_event" ]]; then
        pass "external controller '$external_name' present ($external_event)"

        check_nonzero_capability \
            "$external_event" key \
            "external controller exposes key/button capabilities"

        external_abs="$(capability "$external_event" abs || true)"

        if bitmap_nonzero "$external_abs"; then
            pass "external controller exposes absolute-axis capabilities"
        else
            skip "external controller exposes no absolute axes"
        fi

        if [[ "${NUBOS_EXPECT_FF_RUMBLE:-0}" == "1" &&
              "${INPUT_ENABLE_FF_RUMBLE_CHECK:-0}" == "1" ]]; then
            check_ff_rumble "$external_event"
            skip "physical vibration confirmation remains manual"
        elif [[ "${NUBOS_EXPECT_FF_RUMBLE:-0}" == "1" ]]; then
            skip "FF_RUMBLE check not qualified for this platform profile"
        else
            skip "FF_RUMBLE check not requested for this controller"
        fi
    else
        fail "external controller '$external_name' missing"
    fi
else
    skip "external controller acceptance (set NUBOS_EXTERNAL_INPUT_NAME)"
fi

# ---------------------------------------------------------------------------
# Volume buttons
#
# KEY_VOLUMEDOWN = 114
# KEY_VOLUMEUP   = 115
# ---------------------------------------------------------------------------

if [[ -n "$volume_event" ]]; then
    check_key_set "$volume_event" "volume buttons" \
        "114:KEY_VOLUMEDOWN" \
        "115:KEY_VOLUMEUP"
fi

# ---------------------------------------------------------------------------
# IIO / ADC mux
# ---------------------------------------------------------------------------

if iio_name_present "sun20i-gpadc"; then
    pass "sun20i-gpadc IIO device present"
else
    fail "sun20i-gpadc IIO device missing"
fi

if iio_name_present "adc-mux"; then
    pass "adc-mux IIO device present"
else
    fail "adc-mux IIO device missing"
fi

# ---------------------------------------------------------------------------
# Kernel errors
# ---------------------------------------------------------------------------

if remote '
dmesg |
grep -Ei \
"(adc-joystick|sun20i-gpadc|adc-mux|gpio-mux).*(fail|failed|error|timeout)|(fail|failed|error|timeout).*(adc-joystick|sun20i-gpadc|adc-mux|gpio-mux)" |
grep -q .
'; then
    fail "ADC/IIO/mux failure signature present in dmesg"
else
    pass "no ADC/IIO/mux failure signature in dmesg"
fi

# ---------------------------------------------------------------------------
# Interactive physical acceptance
#
# Automatically enabled by "full".
# Can also be requested independently with:
#
#   NUBOS_INTERACTIVE_INPUT=1 ./scripts/test-platform.sh H5
#
# Reset is intentionally excluded because it resets the whole platform and
# terminates the active regression run.
# ---------------------------------------------------------------------------

interactive="${NUBOS_INTERACTIVE_INPUT:-0}"

case "$NUBOS_TEST_MODE" in
    quick)
        interactive=0
        ;;
    full)
        [[ "${INPUT_ENABLE_PHYSICAL_ACCEPTANCE:-0}" == "1" ]] && interactive=1
        ;;
esac

if [[ "$interactive" == "1" ]]; then
    helper="$NUBOS_ROOT/tests/platform/lib/input-interactive.py"

    interactive_key() {
        local event="$1"
        local code="$2"
        local label="$3"

        if python3 "$helper" key \
            --target "$NUBOS_TARGET" \
            --event "$event" \
            --code "$code" \
            --label "$label" \
            --timeout "$INPUT_INTERACTIVE_TIMEOUT"
        then
            pass "physical $label"
        else
            fail "physical $label"
        fi
    }

    interactive_abs() {
        local event="$1"
        local code="$2"
        local direction="$3"
        local threshold="$4"
        local label="$5"

        if python3 "$helper" abs \
            --target "$NUBOS_TARGET" \
            --event "$event" \
            --code "$code" \
            --direction "$direction" \
            --threshold "$threshold" \
            --label "$label" \
            --timeout "$INPUT_INTERACTIVE_TIMEOUT"
        then
            pass "physical $label"
        else
            fail "physical $label"
        fi
    }

    echo
    echo "----------------------------------------------------------------"
    echo "H5 interactive physical input acceptance"
    echo "----------------------------------------------------------------"

    # Face buttons
    interactive_key "$gamepad_event" 305 "A / BTN_EAST"
    interactive_key "$gamepad_event" 304 "B / BTN_SOUTH"
    interactive_key "$gamepad_event" 307 "X / BTN_NORTH"
    interactive_key "$gamepad_event" 308 "Y / BTN_WEST"

    # D-pad
    interactive_key "$gamepad_event" 544 "D-pad UP"
    interactive_key "$gamepad_event" 545 "D-pad DOWN"
    interactive_key "$gamepad_event" 546 "D-pad LEFT"
    interactive_key "$gamepad_event" 547 "D-pad RIGHT"

    # Shoulders / triggers
    interactive_key "$gamepad_event" 310 "L1"
    interactive_key "$gamepad_event" 311 "R1"
    interactive_key "$gamepad_event" 312 "L2"
    interactive_key "$gamepad_event" 313 "R2"

    # System controls
    interactive_key "$gamepad_event" 314 "SELECT"
    interactive_key "$gamepad_event" 315 "START"
    interactive_key "$gamepad_event" 316 "MENU"

    # Stick clicks
    if [[ "${INPUT_HAS_L3:-1}" == "1" ]]; then
        interactive_key "$gamepad_event" 317 "L3"
    fi

    if [[ "${INPUT_HAS_R3:-1}" == "1" ]]; then
        interactive_key "$gamepad_event" 318 "R3"
    fi

    # Volume
    interactive_key "$volume_event" 115 "VOLUME+"
    interactive_key "$volume_event" 114 "VOLUME-"

    # Power: short press only.
    interactive_key "$power_event" 116 "POWER (pressione breve)"

    # Analog polarity / physical wiring.
    #
    # Pro validated semantics:
    #   left/up   -> low
    #   right/down -> high
    #
    # ABS_X=0 ABS_Y=1 ABS_RX=3 ABS_RY=4

    interactive_abs \
        "$analog_event" 0 low "$INPUT_ANALOG_LOW_MAX" \
        "Left stick LEFT"

    interactive_abs \
        "$analog_event" 0 high "$INPUT_ANALOG_HIGH_MIN" \
        "Left stick RIGHT"

    interactive_abs \
        "$analog_event" 1 low "$INPUT_ANALOG_LOW_MAX" \
        "Left stick UP"

    interactive_abs \
        "$analog_event" 1 high "$INPUT_ANALOG_HIGH_MIN" \
        "Left stick DOWN"

    if [[ "${INPUT_HAS_RIGHT_STICK:-1}" == "1" ]]; then
        interactive_abs \
        "$analog_event" 3 low "$INPUT_ANALOG_LOW_MAX" \
        "Right stick LEFT"

        interactive_abs \
        "$analog_event" 3 high "$INPUT_ANALOG_HIGH_MIN" \
        "Right stick RIGHT"

        interactive_abs \
        "$analog_event" 4 low "$INPUT_ANALOG_LOW_MAX" \
        "Right stick UP"

        interactive_abs \
        "$analog_event" 4 high "$INPUT_ANALOG_HIGH_MIN" \
        "Right stick DOWN"
    fi

    pass "interactive physical input acceptance completed"

else
    skip "physical input acceptance (enabled in full mode)"
fi

# The Reset button is a direct hardware reset rather than a Linux input
# device. It was validated separately during H5 bring-up.
