#!/usr/bin/env bash
set -uo pipefail

AREA="H7"
source "$NUBOS_ROOT/tests/platform/lib/common.sh"

online="$(remote 'cat /sys/devices/system/cpu/online 2>/dev/null' || true)"
online_count="$(remote 'awk -F- '\''{print ($2 == "" ? 1 : $2 - $1 + 1)}'\'' /sys/devices/system/cpu/online 2>/dev/null' || true)"
online_count="${online_count:-0}"

if [[ "$online" == "0-3" && "$online_count" == "4" ]]; then
    pass "four CPUs online ($online)"
else
    fail "CPU online topology '${online:-unknown}' count=$online_count, expected 0-3/count=4"
fi

policies="$(remote 'find /sys/devices/system/cpu/cpufreq -maxdepth 1 -name "policy*" 2>/dev/null | sort' || true)"
[[ -n "$policies" ]] && pass "CPU cpufreq policy present" || fail "CPU cpufreq policy missing"

while IFS= read -r policy; do
    [[ -n "$policy" ]] || continue
    name="${policy##*/}"
    driver="$(remote "cat '$policy/scaling_driver' 2>/dev/null" || true)"
    governor="$(remote "cat '$policy/scaling_governor' 2>/dev/null" || true)"
    min="$(remote "cat '$policy/scaling_min_freq' 2>/dev/null" || true)"
    max="$(remote "cat '$policy/scaling_max_freq' 2>/dev/null" || true)"
    available="$(remote "cat '$policy/scaling_available_frequencies' 2>/dev/null" || true)"

    [[ -n "$driver" ]] && pass "$name cpufreq driver $driver" || fail "$name cpufreq driver missing"
    [[ -n "$governor" ]] && pass "$name governor $governor" || fail "$name governor missing"
    if [[ "$min" =~ ^[0-9]+$ && "$max" =~ ^[0-9]+$ && "$min" -gt 0 && "$max" -ge "$min" ]]; then
        pass "$name frequency range ${min}-${max} kHz"
    else
        fail "$name invalid frequency range min=${min:-?} max=${max:-?}"
    fi
    [[ -n "$available" ]] && pass "$name available frequencies exposed" || fail "$name available frequencies missing"
done <<< "$policies"

gpu_devfreq="$(remote 'for d in /sys/class/devfreq/*; do [ -e "$d" ] || continue; case "$(basename "$d")" in *gpu*|*1800000*) echo "$d"; break;; esac; done' || true)"
if [[ -n "$gpu_devfreq" ]]; then
    pass "GPU devfreq present (${gpu_devfreq##*/})"
    gpu_min="$(remote "cat '$gpu_devfreq/min_freq' 2>/dev/null" || true)"
    gpu_max="$(remote "cat '$gpu_devfreq/max_freq' 2>/dev/null" || true)"
    gpu_available="$(remote "cat '$gpu_devfreq/available_frequencies' 2>/dev/null" || true)"
    if [[ "$gpu_min" =~ ^[0-9]+$ && "$gpu_max" =~ ^[0-9]+$ && "$gpu_min" -gt 0 && "$gpu_max" -ge "$gpu_min" ]]; then
        pass "GPU frequency range ${gpu_min}-${gpu_max} Hz"
    else
        fail "GPU invalid frequency range min=${gpu_min:-?} max=${gpu_max:-?}"
    fi
    [[ -n "$gpu_available" ]] && pass "GPU available frequencies exposed" || fail "GPU available frequencies missing"
else
    fail "GPU devfreq missing"
fi

zones="$(remote 'find /sys/class/thermal -maxdepth 1 -name "thermal_zone*" 2>/dev/null | wc -l' || true)"
trips="$(remote '
n=0
for z in /sys/class/thermal/thermal_zone*; do
    [ -e "$z" ] || continue
    for t in "$z"/trip_point_*_temp; do
        [ -e "$t" ] || continue
        n=$((n + 1))
    done
done
echo "$n"
' || true)"
cooling="$(remote 'find /sys/class/thermal -maxdepth 1 -name "cooling_device*" 2>/dev/null | wc -l' || true)"
zones="${zones:-0}"; trips="${trips:-0}"; cooling="${cooling:-0}"

((zones > 0)) && pass "thermal zones present ($zones)" || fail "thermal zones missing"
((trips > 0)) && pass "thermal trips present ($trips)" || fail "thermal trips missing"
((cooling > 0)) && pass "cooling devices present ($cooling)" || fail "cooling devices missing"

idle_driver="$(remote 'cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null' || true)"
[[ "$idle_driver" == "none" ]] \
    && pass "cpuidle has no registered runtime driver" \
    || fail "cpuidle driver '${idle_driver:-missing}', expected none"

idle_governor="$(remote 'cat /sys/devices/system/cpu/cpuidle/current_governor 2>/dev/null' || true)"
[[ "$idle_governor" == "menu" ]] \
    && pass "cpuidle governor menu" \
    || fail "cpuidle governor '${idle_governor:-missing}', expected menu"

if remote '! find /sys/devices/system/cpu/cpu*/cpuidle -maxdepth 1 -name "state*" 2>/dev/null | grep -q .'; then
    pass "no cpuidle state directories exposed"
else
    fail "unexpected cpuidle state directory exposed"
fi

if remote 'test ! -e /proc/device-tree/cpus/idle-states/cpu-sleep-0'; then
    pass "no deep cpu-sleep-0 device-tree state"
else
    fail "unexpected deep cpu-sleep-0 device-tree state"
fi

if remote 'dmesg | grep -qi PSCI'; then
    pass "PSCI initialization present in dmesg"
else
    fail "PSCI initialization missing from dmesg"
fi

bad_psci_log="$(remote 'dmesg | grep -Ei "PSCI.*(fail|error|invalid|denied)|(fail|error|invalid|denied).*(PSCI)" | tail -n 20' || true)"
if [[ -z "$bad_psci_log" ]]; then
    pass "no PSCI error signature in dmesg"
else
    fail "PSCI error signature found in dmesg"
    printf '%s\n' "$bad_psci_log"
fi

bad_power_log="$(remote 'dmesg | grep -Ei "soft lockup|hard LOCKUP|watchdog.*lockup|rcu.*stall|kernel panic|Oops:|Internal error: Oops" | tail -n 20' || true)"
if [[ -z "$bad_power_log" ]]; then
    pass "no lockup/stall/panic signature in dmesg"
else
    fail "lockup/stall/panic signature found in dmesg"
    printf '%s\n' "$bad_power_log"
fi

case "$NUBOS_TEST_MODE" in
    full|stress)
        duration="${H7_FULL_LOAD_SECONDS:-60}"
        [[ "$NUBOS_TEST_MODE" == "stress" ]] && duration="${H7_STRESS_LOAD_SECONDS:-300}"
        if remote "
pids=''
trap 'kill \$pids 2>/dev/null || true' EXIT HUP INT TERM
for _cpu in 0 1 2 3; do
    (end=\$((\$(date +%s) + $duration)); while [ \"\$(date +%s)\" -lt \"\$end\" ]; do :; done) &
    pids=\"\$pids \$!\"
done
wait
        "; then
            pass "sustained CPU load completed (${duration}s)"
            remote '
for z in /sys/class/thermal/thermal_zone*; do
    [ -e "$z" ] || continue
    printf "%s type=%s temp=%s\n" "$(basename "$z")" \
        "$(cat "$z/type" 2>/dev/null)" "$(cat "$z/temp" 2>/dev/null)"
done
for c in /sys/class/thermal/cooling_device*; do
    [ -e "$c" ] || continue
    printf "%s type=%s state=%s/%s\n" "$(basename "$c")" \
        "$(cat "$c/type" 2>/dev/null)" "$(cat "$c/cur_state" 2>/dev/null)" \
        "$(cat "$c/max_state" 2>/dev/null)"
done
' > "$NUBOS_RESULT_DIR/H7-load-thermal.txt" 2>&1 || true
            pass "post-load thermal/cooling snapshot captured"
        else
            fail "sustained CPU load failed"
        fi
        ;;
    *)
        skip "sustained load/throttling observation (full/stress only)"
        ;;
esac
