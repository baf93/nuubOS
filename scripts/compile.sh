#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BR="$ROOT/external/buildroot"
OUT="$ROOT/output/h700"
DEFCONFIG="nuubos_rg35xx_pro_defconfig"

JOBS="${BR2_JLEVEL:-8}"

CLEAN=0

if [[ "${1:-}" == "--clean" ]]; then
    CLEAN=1
    shift
fi

"$ROOT/scripts/prepare-buildroot.sh" apply

cleanup()
{
    "$ROOT/scripts/prepare-buildroot.sh" restore || true
}

trap cleanup EXIT

if (( CLEAN )); then
    echo "===== CLEAN OUTPUT ====="
    rm -rf "$OUT"
fi

echo "===== DEFCONFIG ====="

make -C "$BR" \
    O="$OUT" \
    BR2_EXTERNAL="$ROOT" \
    "$DEFCONFIG"

echo
echo "===== BUILD ====="

make -C "$BR" \
    O="$OUT" \
    BR2_EXTERNAL="$ROOT" \
    BR2_JLEVEL="$JOBS" \
    "$@"
