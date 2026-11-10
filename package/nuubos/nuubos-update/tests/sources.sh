#!/bin/sh
# Host test of the download sources (nuubos-updatectl): two local http
# hosts, a throw-away release key. Needs cc, libcurl, libsodium, openssl,
# python3.
#
#   sh package/nuubos/nuubos-update/tests/sources.sh
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
T="$(mktemp -d)"
trap 'kill $PA $PB 2>/dev/null; rm -rf "$T"' EXIT
FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; else echo "[FAIL] $1"; FAIL=1; fi; }

export NUUBOS_RELEASE_PUBKEY="$T/key.pub"
PHRASE="$(python3 "$ROOT/tools/ota/release-key.py" new | sed -n 's/^    //p' | tr -d '\n')"
export NUUBOS_RELEASE_PASSPHRASE="$PHRASE"
mkdir -p "$T/a" "$T/b" "$T/dev/update" "$T/dev/data"
echo 100 > "$T/dev/serial"
cc -O1 -o "$T/updatectl" "$ROOT/package/nuubos/nuubos-update/src/updatectl.c" \
    -DUPDATE_PUBKEY_HEX="\"$(tr -d ' \n' < "$T/key.pub")\"" -DSOURCE_SCHEME='"http://"' \
    -DUPDATE_URL='"http://127.0.0.1:18602"' -DUPDATE_DIR="\"$T/dev/data\"" \
    -DCHANNEL_FILE="\"$T/dev/update/channel\"" -DAUTO_FILE="\"$T/dev/update/auto-off\"" \
    -DSOURCES_FILE="\"$T/dev/update/sources\"" -DUPDATE_LOCK="\"$T/dev/lock\"" \
    -DBUILD_SERIAL_FILE="\"$T/dev/serial\"" -DBUILD_ID_FILE="\"$T/dev/build\"" -DOS_RELEASE="\"$T/dev/os\"" \
    -lcurl -lsodium -lz || exit 1
echo rc > "$T/dev/update/channel"

# Host $1 publishes serial $2 on rc with sources $3 and the payload.
publish() {
    mkdir -p "$1/channel-rc" "$1/v9"
    head -c 300000 /dev/urandom > "$1/v9/p.img.gz"
    printf 'format=2\nchannel=rc\nversion=9\nserial=%s\nbuild=x\npayload=v9/p.img.gz\nsize=%s\nsha256=%s\nimage_size=1\nimage_sha256=0\nnotes=\nsources=%s\n' \
        "$2" "$(stat -c %s "$1/v9/p.img.gz")" "$(sha256sum < "$1/v9/p.img.gz" | cut -d' ' -f1)" "$3" > "$1/channel-rc/nuubos-update.manifest"
    python3 "$ROOT/tools/ota/release-key.py" sign "$1/channel-rc/nuubos-update.manifest" "$1/channel-rc/nuubos-update.manifest.sig"
}
serve() { python3 -m http.server -b 127.0.0.1 -d "$1" "$2" >> "$3" 2>&1 & }
serve "$T/b" 18602 "$T/b.log"; PB=$!
serve "$T/a" 18601 "$T/a.log"; PA=$!
sleep 1

publish "$T/b" 200 "http://127.0.0.1:18601"
cp "$T/b/v9/p.img.gz" "$T/b/v9/keep"
check "GitHub only: update found" '"$T/updatectl" check | grep -q "OK available 9"'
check "sources= saved" 'grep -qx "http://127.0.0.1:18601" "$T/dev/update/sources"'

publish "$T/a" 201 "http://127.0.0.1:18601"
: > "$T/a.log"
check "the listed source answers first" '"$T/updatectl" check >/dev/null && grep -q "serial=201" "$T/dev/data/manifest"'
check "payload from the listed source" '"$T/updatectl" download | grep -q downloaded && grep -q "GET /v9/p.img.gz" "$T/a.log"'

rm -f "$T/dev/data/payload"; kill $PA; wait $PA 2>/dev/null
check "listed source down: GitHub's manifest" '"$T/updatectl" check | grep -q "OK available" && grep -q "serial=200" "$T/dev/data/manifest"'
check "listed source down: GitHub's payload" '"$T/updatectl" download | grep -q downloaded'

serve "$T/a" 18601 "$T/a.log"; PA=$!; sleep 1
echo "serial=999" >> "$T/a/channel-rc/nuubos-update.manifest"
check "tampered manifest on a source is skipped" '"$T/updatectl" check >/dev/null && grep -q "serial=200" "$T/dev/data/manifest"'

publish "$T/b" 202 ""
rm -rf "$T/a/channel-rc"
check "a manifest without sources= clears the list" '"$T/updatectl" check >/dev/null && [ ! -e "$T/dev/update/sources" ]'
check "https only from a signed list" 'publish "$T/b" 203 "ftp://x http://127.0.0.1:18601" && "$T/updatectl" check >/dev/null && [ "$(cat "$T/dev/update/sources")" = "http://127.0.0.1:18601" ]'
[ $FAIL = 0 ] && echo "[PASS] update sources" || echo "[FAIL] update sources"
