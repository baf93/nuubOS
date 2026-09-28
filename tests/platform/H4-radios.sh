#!/usr/bin/env bash
set -uo pipefail

AREA="H4"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

HOST="${NUBOS_TARGET#*@}"
PING_ANALYZER="$NUBOS_ROOT/tests/platform/lib/ping-analyze.py"

EXTENDED=0
case "$NUBOS_TEST_MODE" in
    H4|full|stress)
        EXTENDED=1
        ;;
esac

ping_check() {
    local log="$1"
    local label="$2"
    local -A m=()

    while IFS='=' read -r key value; do
        [[ -n "$key" ]] && m["$key"]="$value"
    done < <("$PING_ANALYZER" "$log")

    info "$label: samples=${m[SAMPLES]:-?} loss=${m[LOSS]:-?}%"
    info "$label: min=${m[MIN]:-?}ms avg=${m[AVG]:-?}ms max=${m[MAX]:-?}ms"
    info "$label: p95=${m[P95]:-?}ms p99=${m[P99]:-?}ms"

    if [[ -z "${m[SAMPLES]:-}" ]]; then
        fail "$label: unable to parse ping results"
        return
    fi

    if awk -v x="${m[LOSS]}" 'BEGIN { exit !(x == 0) }'; then
        pass "$label packet delivery = 0% loss"
    else
        fail "$label packet loss = ${m[LOSS]}%"
    fi

    if awk -v x="${m[AVG]}" 'BEGIN { exit !(x < 50) }'; then
        pass "$label average latency ${m[AVG]}ms"
    else
        fail "$label average latency ${m[AVG]}ms >= 50ms"
    fi

    if (( ${m[GE_1000MS]:-0} > 0 )); then
        fail "$label severe TX stall >=1000ms: ${m[GE_1000MS]} samples"
    elif (( ${m[LONGEST_GE_500MS]:-0} >= 3 )); then
        fail "$label sustained TX batching >=500ms: longest sequence ${m[LONGEST_GE_500MS]}"
    elif (( ${m[GE_500MS]:-0} > 0 )); then
        warn "$label transient latency >=500ms: ${m[GE_500MS]} samples, longest sequence ${m[LONGEST_GE_500MS]}"
    else
        pass "$label no TX stalls >=500ms"
    fi

    if awk -v p="${m[P99]}" -v mx="${m[MAX]}" \
        'BEGIN { exit !(p < 100 && mx < 200) }'
    then
        pass "$label latency quality p99=${m[P99]}ms"
    else
        warn "$label latency quality p99=${m[P99]}ms max=${m[MAX]}ms"
    fi

    info "$label: >=50ms=${m[GE_50MS]} >=100ms=${m[GE_100MS]} >=200ms=${m[GE_200MS]} >=500ms=${m[GE_500MS]}"
}

#
# Radio presence
#

if remote "[ -d '/sys/class/net/$WIFI_INTERFACE' ]"; then
    pass "Wi-Fi interface $WIFI_INTERFACE present"

    state="$(remote "cat /sys/class/net/$WIFI_INTERFACE/operstate 2>/dev/null" || true)"
    [[ "$state" == "up" ]] \
        && pass "Wi-Fi operstate up" \
        || warn "Wi-Fi operstate $state"
else
    fail "Wi-Fi interface $WIFI_INTERFACE missing"
fi

if remote '[ -d /sys/class/bluetooth/hci0 ]'; then
    pass "Bluetooth hci0 present"
else
    warn "Bluetooth hci0 missing"
fi

#
# Wi-Fi driver and power policy
#

driver="$(
    remote "
        basename \"\$(readlink -f /sys/class/net/$WIFI_INTERFACE/device/driver 2>/dev/null)\"
    " 2>/dev/null || true
)"

expected_driver="${WIFI_EXPECT_DRIVER:-rtw88_8821cs}"

if [[ "$driver" == "$expected_driver" ]]; then
    pass "Wi-Fi driver $driver"
else
    fail "Wi-Fi driver '${driver:-unknown}', expected '$expected_driver'"
fi

wifi_ps="$(remote "iw dev '$WIFI_INTERFACE' get power_save 2>/dev/null" || true)"

case "$wifi_ps" in
    *"Power save: $WIFI_EXPECT_POWER_SAVE"*)
        pass "Wi-Fi power save $WIFI_EXPECT_POWER_SAVE"
        ;;
    *)
        fail "Wi-Fi power-save policy unexpected: ${wifi_ps:-unavailable}"
        ;;
esac

deep_lps="$(
    remote '
        cat /sys/module/rtw88_core/parameters/disable_lps_deep \
            2>/dev/null
    ' || true
)"

if [[ "$deep_lps" == "$WIFI_EXPECT_DISABLE_LPS_DEEP" ]]; then
    pass "rtw88 Deep LPS disabled"
else
    fail "rtw88 disable_lps_deep=${deep_lps:-unavailable}"
fi

deep_ps_failures="$(
    remote '
        dmesg |
        grep -cF \
            "firmware failed to ack driver for entering Deep Power mode"
    ' || true
)"
deep_ps_failures="${deep_ps_failures:-0}"

if [[ "$deep_ps_failures" == "0" ]]; then
    pass "no RTL8821CS Deep Power ACK failures"
else
    fail "RTL8821CS Deep Power ACK failures=$deep_ps_failures"
fi

if remote 'dmesg | grep -Ei "Bluetooth.*(timeout|failed)|hci.*(timeout|failed)" | grep -q .'; then
    warn "Bluetooth timeout/failure signature present"
else
    pass "no Bluetooth timeout/failure signature in dmesg"
fi

#
# Association / H4 5 GHz width regression
#

link="$(remote "iw dev '$WIFI_INTERFACE' link 2>/dev/null" || true)"
printf '%s\n' "$link" > "$NUBOS_RESULT_DIR/H4-link.txt"

if grep -q '^Connected to ' <<<"$link"; then
    pass "Wi-Fi associated"
else
    fail "Wi-Fi not associated"
fi

ssid="$(awk -F': ' '/SSID:/ {print $2; exit}' <<<"$link")"
freq="$(awk '/freq:/ {print int($2); exit}' <<<"$link")"
signal="$(awk -F': ' '/signal:/ {print $2; exit}' <<<"$link")"
tx_rate="$(awk -F': ' '/tx bitrate:/ {print $2; exit}' <<<"$link")"
rx_rate="$(awk -F': ' '/rx bitrate:/ {print $2; exit}' <<<"$link")"

info "link SSID=${ssid:-unknown}"
info "link frequency=${freq:-unknown}MHz signal=${signal:-unknown}"
info "link TX=${tx_rate:-unknown}"
info "link RX=${rx_rate:-unknown}"

is_5g=0

if [[ "$freq" =~ ^[0-9]+$ ]] && ((freq >= 5000)); then
    is_5g=1
    pass "5 GHz association"

    if grep -Eq '40MHz|80MHz|160MHz' <<<"$tx_rate $rx_rate"; then
        fail "RTL8821CS 5 GHz channel width exceeds 20 MHz"
    else
        pass "RTL8821CS 5 GHz limited to 20 MHz"
    fi
elif [[ "$freq" =~ ^[0-9]+$ ]] && ((freq >= 2400 && freq < 2500)); then
    pass "2.4 GHz association"
    skip "5 GHz HT20 production quirk not exercised"
else
    warn "unable to classify associated Wi-Fi band"
fi

#
# Production cleanliness
#

h4_dmesg="$NUBOS_RESULT_DIR/H4-radio-dmesg.txt"
remote 'dmesg' > "$h4_dmesg" 2>/dev/null || true

if grep -qE 'TXR diag|TXQ diag|RA diag|nuubOS diag:' "$h4_dmesg"; then
    fail "H4 diagnostic instrumentation detected"
else
    pass "no H4 diagnostic instrumentation"
fi

txr_timeouts="$(
    grep -c 'failed to get tx report from firmware' "$h4_dmesg" || true
)"

if ((txr_timeouts == 0)); then
    pass "no firmware TX-report timeout messages"
else
    warn "firmware TX-report timeout messages=$txr_timeouts"
fi

#
# Idle network regression
#

if [[ ! -x "$PING_ANALYZER" ]]; then
    fail "ping analyzer unavailable: $PING_ANALYZER"
else
    if ((EXTENDED)); then
        ping_count=600
    else
        ping_count=100
    fi

    idle_log="$NUBOS_RESULT_DIR/H4-ping-idle.log"

    info "idle ping: interval=100ms count=$ping_count"

    ping \
        -c "$ping_count" \
        -i 0.1 \
        "$HOST" \
        > "$idle_log" 2>&1 || true

    ping_check "$idle_log" "idle"
fi

#
# Moonlight-like sustained host -> handheld load
#

if ((EXTENDED)); then
    if ! command -v python3 >/dev/null 2>&1; then
        fail "python3 unavailable for 25 Mbit/s load generator"
    else
        load_mbit=25
        load_seconds=60

        load_log="$NUBOS_RESULT_DIR/H4-load-25mbit.log"
        loaded_ping_log="$NUBOS_RESULT_DIR/H4-ping-load-25mbit.log"

        info "Moonlight-like load: host -> target ${load_mbit}Mbit/s ${load_seconds}s"

        (
            python3 - "$load_mbit" "$load_seconds" <<'PY' |
import sys
import time

mbit = float(sys.argv[1])
duration = float(sys.argv[2])

rate = mbit * 1_000_000 / 8
chunk_size = 32 * 1024
chunk = b"\0" * chunk_size

start = time.monotonic()
sent = 0

while time.monotonic() - start < duration:
    sys.stdout.buffer.write(chunk)
    sys.stdout.buffer.flush()

    sent += chunk_size

    target = start + sent / rate
    delay = target - time.monotonic()

    if delay > 0:
        time.sleep(delay)

elapsed = time.monotonic() - start
effective_mbit = sent * 8 / elapsed / 1_000_000

print(f"LOAD_BYTES={sent}", file=sys.stderr)
print(f"LOAD_SECONDS={elapsed:.3f}", file=sys.stderr)
print(f"LOAD_EFFECTIVE_MBIT={effective_mbit:.3f}", file=sys.stderr)
PY
            ssh \
                "${SSH_OPTS[@]}" \
                -o Compression=no \
                "$NUBOS_TARGET" \
                'dd of=/dev/null bs=64K 2>/dev/null'
        ) > "$load_log" 2>&1 &

        load_pid=$!

        ping \
            -c 600 \
            -i 0.1 \
            "$HOST" \
            > "$loaded_ping_log" 2>&1 || true

        wait "$load_pid"
        load_rc=$?

        effective_mbit="$(
            awk -F= '/^LOAD_EFFECTIVE_MBIT=/ {print $2}' "$load_log" |
            tail -1
        )"

        if ((load_rc != 0)); then
            fail "25 Mbit/s sustained transfer failed rc=$load_rc"
        elif [[ -z "$effective_mbit" ]]; then
            fail "unable to determine sustained transfer rate"
        else
            load_percent="$(
                awk -v x="$effective_mbit" -v target="$load_mbit"                     'BEGIN { printf "%.1f", (x / target) * 100 }'
            )"

            if awk -v x="$effective_mbit" -v target="$load_mbit"                 'BEGIN { exit !(x >= target * 0.90) }'
            then
                pass "sustained transfer ${effective_mbit} Mbit/s (${load_percent}% of ${load_mbit} Mbit/s target)"
            elif awk -v x="$effective_mbit" -v target="$load_mbit"                 'BEGIN { exit !(x >= target * 0.80) }'
            then
                warn "sustained transfer ${effective_mbit} Mbit/s (${load_percent}% of ${load_mbit} Mbit/s target)"
            else
                fail "sustained transfer ${effective_mbit} Mbit/s (${load_percent}% of ${load_mbit} Mbit/s target)"
            fi
        fi

        ping_check "$loaded_ping_log" "loaded"
    fi
fi
