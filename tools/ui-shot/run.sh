#!/usr/bin/env bash
#
# Headless nuubUI layout snapshots.
#
# Renders the real nuubui-home Slint UI with the Slint software renderer at
# 640x480, 720x720, 1280x720 and 1920x1080 for Settings and Quick Menu scenes and
# writes PNGs to output/ui-shot/<lang>/. Development aid only: nothing here is
# shipped in the image, and it never replaces hardware qualification.
#
# Usage: [ONLY=scene,prefix] tools/ui-shot/run.sh [lang ...]   (default: en it)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/output/ui-shot"
LANGS=("$@")
[ "${#LANGS[@]}" -gt 0 ] || LANGS=(en it)

mkdir -p "$OUT/target" "$OUT/fclink"
# The development container has the fontconfig runtime but no dev symlink;
# the Buildroot host rustc would otherwise link the aarch64 sysroot copy.
ln -sf /usr/lib/x86_64-linux-gnu/libfontconfig.so.1 "$OUT/fclink/libfontconfig.so"

STATUS=0
for L in "${LANGS[@]}"; do
    mkdir -p "$OUT/$L"
    rm -f "$OUT/$L"/*.png
    docker run --rm --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -e LANG_FILE="/i18n/$L.lang" \
        -e ONLY="${ONLY:-}" \
        -e CARGO_HOME=/workspace/dl/br-cargo-home \
        -e CARGO_TARGET_DIR=/target \
        -e RUSTFLAGS="-L native=/fclink -L native=/usr/lib/x86_64-linux-gnu -L native=/lib/x86_64-linux-gnu" \
        -e PATH=/usr/bin:/bin:/workspace/output/h700/host/bin \
        -v "$ROOT:/workspace:ro" \
        -v "$ROOT/dl:/workspace/dl" \
        -v "$OUT/fclink:/fclink:ro" \
        -v "$ROOT/tools/ui-shot:/shot:ro" \
        -v "$OUT/target:/target" \
        -v "$ROOT/package/nuubos/nuubui-home/src/ui:/ui:ro" \
        -v "$ROOT/package/nuubos/nuubos-quick-menu/src/ui:/qm:ro" \
        -v "$ROOT/package/nuubos/nuubos-localization/src/i18n:/i18n:ro" \
        -v "$OUT/$L:/out" \
        -v /usr/share/fonts:/usr/share/fonts:ro \
        -w /shot \
        nuubos-dev:0.5 \
        sh -c 'cargo build --offline --release --locked --target-dir /target > /tmp/build.log 2>&1; rc=$?; grep -E "^(error|warning: unused)" -A6 /tmp/build.log; [ "$rc" = 0 ] || exit 1; /target/release/ui-shot; [ -n "$ONLY" ] || SCENES=qm /target/release/ui-shot'
    BUILD=$?
    # A failed build must not report the previous binary's snapshots.
    if [ "$BUILD" -ne 0 ]; then
        echo "[FAIL] $L: ui-shot build or run failed"
        STATUS=1
    elif ls "$OUT/$L"/*.png >/dev/null 2>&1; then
        echo "[PASS] $L: $(ls "$OUT/$L"/*.png | wc -l) snapshots in $OUT/$L"
    else
        echo "[FAIL] $L: no snapshots produced"
        STATUS=1
    fi
done
exit "$STATUS"
