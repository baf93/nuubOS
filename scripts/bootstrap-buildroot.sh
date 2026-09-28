#!/usr/bin/env bash

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BR="$ROOT/external/buildroot"
REPO="https://github.com/buildroot/buildroot.git"
REF="2026.08"

mkdir -p "$ROOT/external"

if [ ! -d "$BR/.git" ]; then
    echo "===== CLONE BUILDROOT ====="
    git clone "$REPO" "$BR"
    if [ "$?" -ne 0 ]; then
        echo "[FAIL] Buildroot clone failed"
        exit 1
    fi
fi

echo "===== FETCH BUILDROOT TAGS ====="
git -C "$BR" fetch --tags origin
if [ "$?" -ne 0 ]; then
    echo "[FAIL] Buildroot fetch failed"
    exit 1
fi

echo
echo "===== CHECKOUT PINNED BUILDROOT ====="
git -C "$BR" checkout --detach "$REF"
if [ "$?" -ne 0 ]; then
    echo "[FAIL] Buildroot checkout failed for $REF"
    exit 1
fi

actual="$(git -C "$BR" describe --tags --exact-match 2>/dev/null || true)"
if [ "$actual" = "$REF" ]; then
    echo "[PASS] Buildroot pinned to $REF"
else
    echo "[WARN] exact tag verification returned: ${actual:-<none>}"
    git -C "$BR" rev-parse HEAD
fi
