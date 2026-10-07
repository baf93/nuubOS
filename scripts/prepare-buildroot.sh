#!/usr/bin/env bash

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BR="$ROOT/external/buildroot"
# Applied in this order, removed in reverse order.
PATCHES=(
    "$ROOT/patches/buildroot/0001-graphics-mesa25-wlroots0202-buildroot-2026.08.patch"
    "$ROOT/patches/buildroot/0002-ffmpeg-7.1.5-buildroot-2026.08.patch"
)
MODE="${1:-apply}"

if [ ! -d "$BR/.git" ]; then
    echo "ERROR: Buildroot tree missing: $BR"
    exit 1
fi

for patch in "${PATCHES[@]}"; do
    if [ ! -f "$patch" ]; then
        echo "ERROR: Buildroot patch missing: $patch"
        exit 1
    fi
done

applied()
{
    git -C "$BR" apply --reverse --check --whitespace=nowarn "$1" >/dev/null 2>&1
}

case "$MODE" in
apply)
    all_applied=1
    for patch in "${PATCHES[@]}"; do
        applied "$patch" || all_applied=0
    done
    if (( all_applied )); then
        echo "Buildroot patches already applied."
        exit 0
    fi

    if [ -n "$(git -C "$BR" status --short)" ]; then
        echo "ERROR: Buildroot tree is dirty before patching."
        git -C "$BR" status --short
        exit 1
    fi

    for patch in "${PATCHES[@]}"; do
        if ! git -C "$BR" apply --check --whitespace=nowarn "$patch"; then
            echo "ERROR: $(basename "$patch") cannot be applied cleanly."
            "$0" restore
            exit 1
        fi
        git -C "$BR" apply --whitespace=nowarn "$patch"
        echo "Applied $(basename "$patch")."
    done
    ;;
restore)
    for (( i = ${#PATCHES[@]} - 1; i >= 0; i-- )); do
        if applied "${PATCHES[i]}"; then
            git -C "$BR" apply --reverse --whitespace=nowarn "${PATCHES[i]}"
        fi
    done

    if [ -n "$(git -C "$BR" status --short)" ]; then
        echo "ERROR: Buildroot tree is not pristine after restore."
        git -C "$BR" status --short
        exit 1
    fi

    echo "Buildroot tree restored pristine."
    ;;
*)
    echo "Usage: $0 [apply|restore]"
    exit 2
    ;;
esac
