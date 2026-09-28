#!/bin/sh
set -eu

BINARIES="${1:?missing images directory}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

cp "$SCRIPT_DIR/nuubos.cfg" "$BINARIES/nuubos.cfg"
