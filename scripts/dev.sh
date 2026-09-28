#!/usr/bin/env bash

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mkdir -p "$ROOT/.docker-home"
mkdir -p "$ROOT/.cache/nuubos/buildroot-ccache"
mkdir -p "$ROOT/dl"
mkdir -p "$ROOT/output"

echo "===== BUILD DEVELOPMENT CONTAINER ====="

docker build     -t nuubos-dev:0.5     -f "$ROOT/docker/development/Dockerfile"     "$ROOT/docker/development"

if [ "$?" -ne 0 ]; then
    echo "[FAIL] Development container build failed"
else
    echo
    echo "[PASS] Development container ready"
    echo
    echo "===== ENTER DEVELOPMENT CONTAINER ====="

    docker run --rm -it         --user "$(id -u):$(id -g)"         -e HOME=/workspace/.docker-home         -e BR2_DL_DIR=/workspace/dl         -e BR2_CCACHE_DIR=/workspace/.cache/nuubos/buildroot-ccache         -v "$ROOT:/workspace"         -w /workspace         nuubos-dev:0.5         bash
fi
