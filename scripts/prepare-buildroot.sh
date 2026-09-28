#!/usr/bin/env bash

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BR="$ROOT/external/buildroot"
PATCH="$ROOT/patches/buildroot/0001-graphics-mesa25-wlroots0202-buildroot-2026.08.patch"
MODE="${1:-apply}"

if [ ! -d "$BR/.git" ]; then
    echo "ERROR: Buildroot tree missing: $BR"
    exit 1
fi

if [ ! -f "$PATCH" ]; then
    echo "ERROR: graphics compatibility patch missing: $PATCH"
    exit 1
fi

case "$MODE" in
apply)
    if git -C "$BR" apply --reverse --check --whitespace=nowarn "$PATCH" >/dev/null 2>&1; then
        echo "Buildroot graphics compatibility patch already applied."
        exit 0
    fi

    if [ -n "$(git -C "$BR" status --short)" ]; then
        echo "ERROR: Buildroot tree is dirty before graphics patch."
        git -C "$BR" status --short
        exit 1
    fi

    if ! git -C "$BR" apply --check --whitespace=nowarn "$PATCH"; then
        echo "ERROR: graphics compatibility patch cannot be applied cleanly."
        exit 1
    fi

    git -C "$BR" apply --whitespace=nowarn "$PATCH"
    echo "Applied Mesa 25.0.6 + wlroots 0.20.2 compatibility patch."
    ;;
restore)
    if git -C "$BR" apply --reverse --check --whitespace=nowarn "$PATCH" >/dev/null 2>&1; then
        git -C "$BR" apply --reverse --whitespace=nowarn "$PATCH"
    fi

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
