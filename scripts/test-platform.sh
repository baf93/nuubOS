#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="quick"
PROFILE="${NUBOS_PROFILE:-rg35xx-pro}"
TARGET="${NUBOS_TARGET:-root@192.168.5.36}"

usage() {
    cat <<'EOF'
Usage:
  scripts/test-platform.sh [quick|full|stress|perf|static|H0|H1|H2|H3|H4|H5|H6|H7|H8]
                           [--target root@IP]
                           [--profile rg35xx-pro|rgcubexx|rg40xx-v]

Environment overrides:
  NUBOS_TARGET
  NUBOS_PROFILE
  NUBOS_TEST_VECTOR_DIR
EOF
}

while (($#)); do
    case "$1" in
        quick|full|stress|perf|static|H0|H1|H2|H3|H4|H5|H6|H7|H8)
            MODE="$1"
            shift
            ;;
        --target)
            TARGET="$2"
            shift 2
            ;;
        --profile)
            PROFILE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

PROFILE_FILE="$ROOT/tests/platform/profiles/$PROFILE.conf"
case "$PROFILE" in
    rg35xx-pro|rgcubexx|rg40xx-v) ;;
    *)
        echo "Unsupported platform profile: $PROFILE" >&2
        exit 2
        ;;
esac

if [[ ! -f "$PROFILE_FILE" ]]; then
    echo "Missing profile: $PROFILE_FILE" >&2
    exit 2
fi

check_device_map() {
    local selector="$ROOT/patches/u-boot/platform/h700/0001-sunxi-h700-universal-uboot-device-selector.patch"
    local kernel_map="$ROOT/patches/linux/h700/0020-arm64-dts-allwinner-add-anbernic-h700-family.patch"
    local kernel_makefile="$ROOT/external/linux/arch/arm64/boot/dts/allwinner/Makefile"
    local failures=0 id dtb
    local -a mappings=(
        'rg-sp:sun50i-h700-anbernic-rg-sp.dtb'
        'rg28xx:sun50i-h700-anbernic-rg28xx.dtb'
        'rg34xx:sun50i-h700-anbernic-rg34xx.dtb'
        'rg34xx-sp:sun50i-h700-anbernic-rg34xx-sp.dtb'
        'rg34xx-sp-v2:sun50i-h700-anbernic-rg34xx-sp-v2-panel.dtb'
        'rg35xx-2024:sun50i-h700-anbernic-rg35xx-2024.dtb'
        'rg35xx-2024-v6:sun50i-h700-anbernic-rg35xx-2024-rev6-panel.dtb'
        'rg35xx-h:sun50i-h700-anbernic-rg35xx-h.dtb'
        'rg35xx-h-v6:sun50i-h700-anbernic-rg35xx-h-rev6-panel.dtb'
        'rg35xx-plus:sun50i-h700-anbernic-rg35xx-plus.dtb'
        'rg35xx-plus-v6:sun50i-h700-anbernic-rg35xx-plus-rev6-panel.dtb'
        'rg35xx-pro:sun50i-h700-anbernic-rg35xx-pro.dtb'
        'rg35xx-sp:sun50i-h700-anbernic-rg35xx-sp.dtb'
        'rg35xx-sp-v2:sun50i-h700-anbernic-rg35xx-sp-v2-panel.dtb'
        'rg40xx-h:sun50i-h700-anbernic-rg40xx-h.dtb'
        'rg40xx-h-v2:sun50i-h700-anbernic-rg40xx-h-v2-panel.dtb'
        'rg40xx-v:sun50i-h700-anbernic-rg40xx-v.dtb'
        'rg40xx-v-v2:sun50i-h700-anbernic-rg40xx-v-v2-panel.dtb'
        'rgcubexx:sun50i-h700-anbernic-rgcubexx.dtb'
    )

    [[ -r "$selector" && -r "$kernel_map" && -r "$kernel_makefile" ]] || {
        echo "[FAIL] [STATIC] mapping source patch missing"
        return 1
    }

    for mapping in "${mappings[@]}"; do
        id="${mapping%%:*}"
        dtb="${mapping#*:}"
        if grep -Fq "x${id}; then setenv fdtfile ${dtb}" "$selector" &&
           { grep -Fq "${dtb}" "$kernel_makefile" ||
             grep -Fq "${dtb}" "$kernel_map"; }; then
            echo "[PASS] [STATIC] $id -> $dtb"
        else
            echo "[FAIL] [STATIC] inconsistent mapping: $id -> $dtb"
            ((failures++))
        fi
    done

    if [[ "${#mappings[@]}" == "19" && "$failures" == "0" ]]; then
        echo "RESULT: PASS (19 DEVICE mappings)"
        return 0
    fi

    echo "RESULT: FAIL ($failures inconsistent mappings)"
    return 1
}

if [[ "$MODE" == "static" ]]; then
    check_device_map
    exit $?
fi

# Profiles under qualification can deliberately enable only the modules whose
# expectations have been observed on that device.
# shellcheck disable=SC1090
source "$PROFILE_FILE"
PROFILE_ENABLED_MODULES="${PROFILE_ENABLED_MODULES:-H0 H1 H2 H3 H4 H5 H6 H7 H8}"

STAMP="$(date +%Y-%m-%d_%H%M%S)"
RESULT_DIR="$ROOT/output/test-results/${STAMP}-${PROFILE}-${MODE}"
mkdir -p "$RESULT_DIR"

export NUBOS_ROOT="$ROOT"
export NUBOS_TARGET="$TARGET"
export NUBOS_PROFILE="$PROFILE"
export NUBOS_PROFILE_FILE="$PROFILE_FILE"
export NUBOS_TEST_MODE="$MODE"
export NUBOS_RESULT_DIR="$RESULT_DIR"
export NUBOS_TEST_VECTOR_DIR="${NUBOS_TEST_VECTOR_DIR:-$ROOT/output/test-vectors}"

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=7 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)


if [[ -t 1 && -z "${NO_COLOR:-}" && "${TERM:-dumb}" != "dumb" ]]; then
    C_RESET=$'\033[0m'
    C_BOLD=$'\033[1m'
    C_RED=$'\033[31m'
    C_GREEN=$'\033[32m'
    C_YELLOW=$'\033[33m'
    C_BLUE=$'\033[34m'
    C_CYAN=$'\033[36m'
else
    C_RESET=''
    C_BOLD=''
    C_RED=''
    C_GREEN=''
    C_YELLOW=''
    C_BLUE=''
    C_CYAN=''
fi

colorize_output() {
    local line

    while IFS= read -r line; do
        case "$line" in
            "[PASS]"*)
                printf '%b%s%b\n' "$C_GREEN" "$line" "$C_RESET"
                ;;
            "[FAIL]"*)
                printf '%b%s%b\n' "${C_BOLD}${C_RED}" "$line" "$C_RESET"
                ;;
            "[WARN]"*)
                printf '%b%s%b\n' "$C_YELLOW" "$line" "$C_RESET"
                ;;
            "[SKIP]"*)
                printf '%b%s%b\n' "$C_YELLOW" "$line" "$C_RESET"
                ;;
            "[INFO]"*)
                printf '%b%s%b\n' "$C_CYAN" "$line" "$C_RESET"
                ;;
            "RESULT: PASS WITH WARNINGS"*)
                printf '%b%s%b\n' "${C_BOLD}${C_YELLOW}" "$line" "$C_RESET"
                ;;
            "RESULT: PASS"*)
                printf '%b%s%b\n' "${C_BOLD}${C_GREEN}" "$line" "$C_RESET"
                ;;
            "RESULT: FAIL"*)
                printf '%b%s%b\n' "${C_BOLD}${C_RED}" "$line" "$C_RESET"
                ;;
            *)
                printf '%s\n' "$line"
                ;;
        esac
    done
}

echo "nuubOS Platform Regression Suite"
echo "Profile : $PROFILE"
echo "Target  : $TARGET"
echo "Mode    : $MODE"
echo "Results : $RESULT_DIR"
echo

if ! ssh "${SSH_OPTS[@]}" "$TARGET" 'true' >/dev/null 2>&1; then
    echo "[FAIL] [PRE] SSH unavailable: $TARGET"
    exit 1
fi

ssh "${SSH_OPTS[@]}" "$TARGET" '
echo "hostname=$(hostname)"
echo "kernel=$(uname -r)"
echo "machine=$(uname -m)"
echo "uptime=$(cut -d" " -f1 /proc/uptime)"
' > "$RESULT_DIR/system.txt" 2>&1 || true

ssh "${SSH_OPTS[@]}" "$TARGET" 'dmesg' > "$RESULT_DIR/dmesg-before.txt" 2>&1 || true

case "$MODE" in
    H0|H1|H2|H3|H4|H5|H6|H7|H8)
        MODULES=("$MODE")
        ;;
    perf)
        # H3 performance mode is intentionally VPU-only. It requires the
        # diagnostic GStreamer/v4l2codecs/fakevideosink stack on the target.
        MODULES=(H3)
        ;;
    *)
        MODULES=(H0 H1 H2 H3 H4 H5 H6 H7 H8)
        ;;
esac

MODULE_CRASHES=0

for module in "${MODULES[@]}"; do
    if [[ " $PROFILE_ENABLED_MODULES " != *" $module "* ]]; then
        module_log="$RESULT_DIR/${module}.log"
        echo "[SKIP] [$module] not qualified for profile $PROFILE" \
            | tee "$module_log" \
            | colorize_output
        continue
    fi

    script="$ROOT/tests/platform/${module}-"*
    # shellcheck disable=SC2086
    matches=( $script )
    if [[ ${#matches[@]} -ne 1 || ! -f "${matches[0]}" ]]; then
        echo "[FAIL] [$module] module not found"
        ((MODULE_CRASHES++))
        continue
    fi

    module_script="${matches[0]}"
    module_log="$RESULT_DIR/${module}.log"

    echo
    echo "================================================================"
    echo "$module  $(basename "$module_script")"
    echo "================================================================"

    bash "$module_script" 2>&1 | tee "$module_log" | colorize_output
    rc=${PIPESTATUS[0]}

    if ((rc != 0)); then
        echo "[FAIL] [$module] module crashed (exit $rc)" | tee -a "$module_log"
        ((MODULE_CRASHES++))
    fi
done

ssh "${SSH_OPTS[@]}" "$TARGET" 'dmesg' > "$RESULT_DIR/dmesg-after.txt" 2>&1 || true

PASS_COUNT=$(grep -h '^\[PASS\]' "$RESULT_DIR"/H*.log 2>/dev/null | wc -l)
WARN_COUNT=$(grep -h '^\[WARN\]' "$RESULT_DIR"/H*.log 2>/dev/null | wc -l)
FAIL_COUNT=$(grep -h '^\[FAIL\]' "$RESULT_DIR"/H*.log 2>/dev/null | wc -l)
SKIP_COUNT=$(grep -h '^\[SKIP\]' "$RESULT_DIR"/H*.log 2>/dev/null | wc -l)

{
    echo "nuubOS Platform Regression Suite"
    echo "Profile: $PROFILE"
    echo "Target : $TARGET"
    echo "Mode   : $MODE"
    echo
    printf "PASS: %d\n" "$PASS_COUNT"
    printf "WARN: %d\n" "$WARN_COUNT"
    printf "FAIL: %d\n" "$FAIL_COUNT"
    printf "SKIP: %d\n" "$SKIP_COUNT"
    echo
    if ((FAIL_COUNT == 0 && MODULE_CRASHES == 0)); then
        if ((WARN_COUNT > 0)); then
            echo "RESULT: PASS WITH WARNINGS"
        else
            echo "RESULT: PASS"
        fi
    else
        echo "RESULT: FAIL"
    fi
} | tee "$RESULT_DIR/summary.txt" | colorize_output

echo
echo "Detailed logs: $RESULT_DIR"

if ((FAIL_COUNT == 0 && MODULE_CRASHES == 0)); then
    exit 0
fi
exit 1
