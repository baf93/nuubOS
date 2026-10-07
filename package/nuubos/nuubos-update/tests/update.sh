#!/bin/sh
# Host test: OTA verification (real updatectl, OpenSSL-signed manifest,
# local HTTP server) and install/rollback/confirm on a fake root.
# Run: sh package/nuubos/nuubos-update/tests/update.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
T="$(mktemp -d)"
trap 'kill "$HTTP" 2>/dev/null; rm -rf "${T:?}"' EXIT
FAIL=0
ck() { if eval "$2"; then :; else echo "[FAIL] $1"; FAIL=1; fi; }
R="$T/r"; U="$R/userdata/.nuubos-update"; W="$T/www"
mkdir -p "$R/usr/bin" "$R/etc" "$R/usr/share/nuubos" "$R/state" "$R/userdata" "$W"
echo 'VERSION_ID=0.5' > "$R/etc/os-release"
echo "old-a" > "$R/usr/bin/a"; chmod 755 "$R/usr/bin/a"; echo keep > "$R/usr/bin/untouched"

# Release: payload (rootfs tar.gz) + signed manifest.
mkdir -p "$T/root/usr/bin" "$T/root/usr/lib/new" "$T/root/dev" "$T/root/userdata"
echo "new-a" > "$T/root/usr/bin/a"; chmod 755 "$T/root/usr/bin/a"
echo "new-b" > "$T/root/usr/lib/new/b"; echo "x" > "$T/root/userdata/never"
(cd "$T/root" && tar -czf "$W/nuubos-rootfs-0.6.tar.gz" .)
SHA="$(sha256sum "$W/nuubos-rootfs-0.6.tar.gz" | cut -d' ' -f1)"; SIZE="$(wc -c < "$W/nuubos-rootfs-0.6.tar.gz")"
printf 'format=1\nversion=0.6\nbuild=test\npayload=nuubos-rootfs-0.6.tar.gz\nsize=%s\nsha256=%s\nnotes=https://example.invalid/0.6\n' "$SIZE" "$SHA" > "$W/nuubos-update.manifest"
openssl genpkey -algorithm ed25519 -out "$T/key.pem" 2>/dev/null
openssl pkeyutl -sign -inkey "$T/key.pem" -rawin -in "$W/nuubos-update.manifest" -out "$W/nuubos-update.manifest.sig"
PUB="$(openssl pkey -in "$T/key.pem" -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')"
PORT=$((20000 + $$ % 20000))
(cd "$W" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) & HTTP=$!
sleep 1
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
    -DUPDATE_URL="\"http://127.0.0.1:$PORT\"" -DUPDATE_PUBKEY_HEX="\"$PUB\"" -DUPDATE_DIR="\"$U\"" \
    -DOS_RELEASE="\"$R/etc/os-release\"" -DBUILD_ID_FILE="\"$R/usr/share/nuubos/build-id\"" \
    "$SRC/updatectl.c" -o "$T/updatectl" -lcurl -lsodium || exit 1
gcc -std=c11 -D_GNU_SOURCE -Wno-format-truncation -DUPDATE_URL="\"http://127.0.0.1:$PORT\"" \
    -DUPDATE_DIR="\"$T/nokey\"" "$SRC/updatectl.c" -o "$T/updatectl-nokey" -lcurl -lsodium || exit 1
C="$T/updatectl"

ck "no key: unavailable" '"$T/updatectl-nokey" check | grep -q "ERR unavailable"'
ck "check finds 0.6" '$C check | grep -q "OK available 0.6"'
ck "status" '$C status | grep -q "available=1" && $C status | grep -q "notes=https://example.invalid/0.6"'
# Resume: start with a correct prefix of the payload.
head -c 100 "$W/nuubos-rootfs-0.6.tar.gz" > "$U/payload.part"
OUT="$(NUUBOS_JOB_ID=1 $C download)"
ck "download (resumed) verified" 'echo "$OUT" | grep -q "@result downloaded" && cmp -s "$U/payload" "$W/nuubos-rootfs-0.6.tar.gz"'
ck "verify" '[ "$($C verify)" = OK ]'

# Tampered manifest is refused, the good one kept.
cp "$W/nuubos-update.manifest" "$T/good.manifest"
sed -i 's/version=0.6/version=9.9/' "$W/nuubos-update.manifest"
ck "tampered manifest rejected" '$C check | grep -q "ERR signature" && grep -q "version=0.6" "$U/manifest"'
cp "$T/good.manifest" "$W/nuubos-update.manifest"
# Corrupt payload part is not kept.
echo garbage > "$T/bad.part"; rm -f "$U/payload"; cp "$T/bad.part" "$U/payload.part"
cp "$W/nuubos-rootfs-0.6.tar.gz" "$T/real.tgz"; echo corrupt >> "$W/nuubos-rootfs-0.6.tar.gz"
ck "corrupt download rejected" '$C download | grep -q "ERR" && [ ! -f "$U/payload" ] && [ ! -f "$U/payload.part" ]'
cp "$T/real.tgz" "$W/nuubos-rootfs-0.6.tar.gz"; $C download >/dev/null

A() { NUUBOS_ROOT="$R" NUUBOS_UPDATECTL="$C" sh "$SRC/nuubos-update-apply" "$@"; }
ck "install marks pending" '[ "$(A install)" = "OK restart-required" ] && A status | grep -q "pending=1"'
ck "sleep never installs" 'A apply sleep; A status | grep -q "pending=1" && [ "$(cat "$R/usr/bin/a")" = old-a ]'
A apply restart
ck "applied" '[ "$(cat "$R/usr/bin/a")" = new-a ] && [ -f "$R/usr/lib/new/b" ] && A status | grep -q "applied=1"'
ck "mode kept" '[ -x "$R/usr/bin/a" ]'
ck "userdata never written by payload" '[ ! -e "$R/userdata/never" ]'
ck "untouched kept" '[ "$(cat "$R/usr/bin/untouched")" = keep ]'
A boot; A boot
ck "two boots: still applied" 'A status | grep -q "attempts=2"'
A boot
ck "third unconfirmed boot rolls back" '[ "$(cat "$R/usr/bin/a")" = old-a ] && [ ! -e "$R/usr/lib/new/b" ] && [ ! -d "$R/usr/lib/new" ] && A status | grep -q "result=rolled-back"'
ck "rolled-back mode kept" '[ -x "$R/usr/bin/a" ]'
# Install again and confirm.
A install >/dev/null; A apply restart; A boot
ck "confirm keeps update" '[ "$(A confirm)" = OK ] && [ "$(cat "$R/usr/bin/a")" = new-a ] && A status | grep -q "result=ok" && [ ! -f "$U/rollback.tar" ]'
[ "$FAIL" = 0 ] && echo "[PASS] update"
exit "$FAIL"
