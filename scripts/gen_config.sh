#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BR="$ROOT/external/buildroot"
OUT="$ROOT/output/h700"
DEFCONFIG="nuubos_rg35xx_pro_defconfig"

"$ROOT/scripts/prepare-buildroot.sh" apply

cleanup()
{
    "$ROOT/scripts/prepare-buildroot.sh" restore || true
}

trap cleanup EXIT

make -C "$BR" \
    O="$OUT" \
    BR2_EXTERNAL="$ROOT" \
    "$DEFCONFIG"
