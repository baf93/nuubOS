#!/usr/bin/env bash
set -uo pipefail

AREA="H2"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

AUDIO_REQUIRE_ANALOG="${AUDIO_REQUIRE_ANALOG:-1}"
AUDIO_REQUIRE_HDMI="${AUDIO_REQUIRE_HDMI:-1}"

cards="$(remote 'cat /proc/asound/cards 2>/dev/null' || true)"

if [[ "$AUDIO_REQUIRE_ANALOG" == "1" ]]; then
    grep -q 'Codec' <<<"$cards" \
        && pass "analog codec card present" \
        || fail "analog codec card missing"
else
    skip "analog audio outside qualification boundary for this profile"
fi

if [[ "$AUDIO_REQUIRE_HDMI" == "1" ]]; then
    grep -q 'HDMI' <<<"$cards" \
        && pass "HDMI audio card present" \
        || fail "HDMI audio card missing"
else
    skip "HDMI audio pending separate HDMI qualification"
fi

if ! remote_has aplay; then
    skip "aplay unavailable; PCM open tests not run"
    exit 0
fi

remote 'aplay -l >/dev/null 2>&1' \
    && pass "ALSA playback enumeration" \
    || fail "ALSA playback enumeration failed"

if [[ "$NUBOS_TEST_MODE" == "quick" ]]; then
    exit 0
fi

test_pcm() {
    local card="$1"
    local rate="$2"
    local bytes

    bytes=$((rate * 2 * 2 / 10)) # 100 ms, S16_LE stereo

    if remote "dd if=/dev/zero bs=$bytes count=1 2>/dev/null | aplay -q -D hw:$card,0 -t raw -f S16_LE -r $rate -c 2"; then
        pass "hw:$card,0 S16_LE ${rate}Hz open/play/close"
    else
        fail "hw:$card,0 S16_LE ${rate}Hz failed"
    fi
}

if [[ "$AUDIO_REQUIRE_ANALOG" == "1" ]]; then
    test_pcm "$AUDIO_ANALOG_CARD" 44100
    test_pcm "$AUDIO_ANALOG_CARD" 48000
    test_pcm "$AUDIO_ANALOG_CARD" 96000
fi

if [[ "$AUDIO_REQUIRE_HDMI" == "1" ]]; then
    hdmi="$(remote "
for f in $HDMI_STATUS_GLOB; do
    [ -e \"\$f\" ] || continue
    cat \"\$f\"
    break
done
")"

    if [[ "$hdmi" == "connected" ]]; then
        test_pcm "$AUDIO_HDMI_CARD" 48000
    else
        skip "HDMI PCM playback (HDMI disconnected)"
    fi
fi

if [[ "$NUBOS_TEST_MODE" == "stress" &&
      "$AUDIO_REQUIRE_ANALOG" == "1" ]]; then
    failures=0

    for _ in $(seq 1 10); do
        remote "dd if=/dev/zero bs=19200 count=1 2>/dev/null | aplay -q -D hw:$AUDIO_ANALOG_CARD,0 -t raw -f S16_LE -r 48000 -c 2" \
            || failures=$((failures + 1))
    done

    [[ "$failures" == "0" ]] \
        && pass "10x analog PCM open/close stress" \
        || fail "analog PCM stress failures=$failures"
fi
