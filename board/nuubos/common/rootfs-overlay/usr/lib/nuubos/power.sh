#!/bin/sh

POWER_CONFIG=/state/config/nuubos.conf

CPU_POLICY=/sys/devices/system/cpu/cpufreq/policy0
GPU_DEVFREQ=/sys/class/devfreq/1800000.gpu

AUTO_CPU_MAX=1512000
AUTO_GPU_MAX=600000000

BATTERY_SAVER_CPU_MAX=1200000
BATTERY_SAVER_GPU_MAX=504000000

power_log()
{
	echo "nuubOS power: $*"
}

find_cpu_thermal_zone()
{
	for zone in /sys/class/thermal/thermal_zone*; do
		[ -r "$zone/type" ] || continue

		if [ "$(cat "$zone/type")" = "cpu-thermal" ]; then
			echo "$zone"
			return 0
		fi
	done

	return 1
}

apply_thermal_policy()
{
	TZ="$(find_cpu_thermal_zone)" || {
		power_log "cpu-thermal zone not found" >&2
		return 1
	}

	[ -w "$TZ/policy" ] || {
		power_log "thermal policy is not writable" >&2
		return 1
	}

	#
	# Switching governor initializes the kernel default PID values.
	# Do it only when necessary, then apply the validated H700 tuning.
	#
	if [ "$(cat "$TZ/policy")" != "power_allocator" ]; then
		echo power_allocator > "$TZ/policy" || return 1
	fi

	echo 20 > "$TZ/k_po" || return 1
	echo 40 > "$TZ/k_pu" || return 1
	echo 0  > "$TZ/k_i" || return 1
	echo 0  > "$TZ/k_d" || return 1
	echo 0  > "$TZ/integral_cutoff" || return 1

	return 0
}

apply_limits()
{
	MODE="$1"

	[ -w "$CPU_POLICY/scaling_max_freq" ] || {
		power_log "CPU cpufreq policy unavailable" >&2
		return 1
	}

	[ -w "$GPU_DEVFREQ/max_freq" ] || {
		power_log "GPU devfreq unavailable" >&2
		return 1
	}

	case "$MODE" in
		AUTO)
			CPU_MAX="$AUTO_CPU_MAX"
			GPU_MAX="$AUTO_GPU_MAX"
			;;
		BATTERY_SAVER)
			CPU_MAX="$BATTERY_SAVER_CPU_MAX"
			GPU_MAX="$BATTERY_SAVER_GPU_MAX"
			;;
		*)
			power_log "invalid mode: $MODE" >&2
			return 1
			;;
	esac

	echo "$CPU_MAX" > "$CPU_POLICY/scaling_max_freq" || return 1
	echo "$GPU_MAX" > "$GPU_DEVFREQ/max_freq" || return 1

	return 0
}

apply_power_mode()
{
	MODE="$1"

	apply_thermal_policy || return 1
	apply_limits "$MODE" || return 1

	power_log "mode applied: $MODE"
	return 0
}

read_power_mode()
{
	SYSTEM_CONFIG=/state/config/nuubos-system.conf

	if [ -f "$SYSTEM_CONFIG" ]; then
		PROFILE="$(sed -n 's/^PERFORMANCE_PROFILE=//p' "$SYSTEM_CONFIG" | head -n 1)"
		case "$PROFILE" in
			auto) echo AUTO; return 0 ;;
			battery-saver) echo BATTERY_SAVER; return 0 ;;
		esac
	fi

	if [ ! -f "$POWER_CONFIG" ]; then
		echo AUTO
		return 0
	fi

	MODE="$(sed -n 's/^POWER_MODE=//p' "$POWER_CONFIG" | head -n 1)"

	case "$MODE" in
		AUTO|BATTERY_SAVER)
			echo "$MODE"
			;;
		*)
			power_log "invalid or missing saved mode, using AUTO" >&2
			echo AUTO
			;;
	esac
}

save_power_mode()
{
	MODE="$1"

	[ -f "$POWER_CONFIG" ] || {
		power_log "persistent config unavailable: $POWER_CONFIG" >&2
		return 1
	}

	TMP="${POWER_CONFIG}.tmp.$$"

	awk -v mode="$MODE" '
		BEGIN {
			found = 0
		}

		/^POWER_MODE=/ {
			print "POWER_MODE=" mode
			found = 1
			next
		}

		{
			print
		}

		END {
			if (!found)
				print "POWER_MODE=" mode
		}
	' "$POWER_CONFIG" > "$TMP" || {
		rm -f "$TMP"
		return 1
	}

	chmod 600 "$TMP" || {
		rm -f "$TMP"
		return 1
	}

	mv "$TMP" "$POWER_CONFIG" || {
		rm -f "$TMP"
		return 1
	}

	return 0
}
