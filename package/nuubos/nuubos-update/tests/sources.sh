#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Host test of the download sources (nuubos-updatectl): three local http
# hosts (A = the compiled backend, like SourceForge; B = the GitHub
# fallback; C = a second host a signed manifest can add), a throw-away
# release key. Needs cc, libcurl, libsodium, openssl, python3.
#
#   sh package/nuubos/nuubos-update/tests/sources.sh
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
T="$(mktemp -d)"
trap 'kill $PA $PB $PC 2>/dev/null; rm -rf "$T"' EXIT
FAIL=0
check() { if eval "$2"; then echo "[PASS] $1"; else echo "[FAIL] $1"; FAIL=1; fi; }

export NUUBOS_RELEASE_PUBKEY="$T/key.pub"
PHRASE="$(python3 "$ROOT/tools/ota/release-key.py" new | sed -n 's/^    //p' | tr -d '\n')"
export NUUBOS_RELEASE_PASSPHRASE="$PHRASE"
mkdir -p "$T/a" "$T/b" "$T/c" "$T/dev/update" "$T/dev/data"
echo 100 > "$T/dev/serial"
cc -O1 -o "$T/updatectl" "$ROOT/package/nuubos/nuubos-update/src/updatectl.c" \
    -DUPDATE_PUBKEY_HEX="\"$(tr -d ' \n' < "$T/key.pub")\"" -DSOURCE_SCHEME='"http://"' \
    -DUPDATE_SOURCES='"http://127.0.0.1:18601"' -DUPDATE_URL='"http://127.0.0.1:18602"' \
    -DUPDATE_DIR="\"$T/dev/data\"" \
    -DCHANNEL_FILE="\"$T/dev/update/channel\"" -DAUTO_FILE="\"$T/dev/update/auto-off\"" \
    -DSOURCES_FILE="\"$T/dev/update/sources\"" -DUPDATE_LOCK="\"$T/dev/lock\"" \
    -DBUILD_SERIAL_FILE="\"$T/dev/serial\"" -DBUILD_ID_FILE="\"$T/dev/build\"" -DOS_RELEASE="\"$T/dev/os\"" \
    -lcurl -lsodium -lz || exit 1
echo rc > "$T/dev/update/channel"

# Host $1 publishes serial $2 on rc with sources $3 and its payload.
publish() {
    mkdir -p "$1/channel-rc" "$1/v9"
    head -c 300000 /dev/urandom > "$1/v9/p.img.gz"
    printf 'format=2\nchannel=rc\nversion=9\nserial=%s\nbuild=x\npayload=v9/p.img.gz\nsize=%s\nsha256=%s\nimage_size=1\nimage_sha256=0\nnotes=\nsources=%s\n' \
        "$2" "$(stat -c %s "$1/v9/p.img.gz")" "$(sha256sum < "$1/v9/p.img.gz" | cut -d' ' -f1)" "$3" > "$1/channel-rc/nuubos-update.manifest"
    python3 "$ROOT/tools/ota/release-key.py" sign "$1/channel-rc/nuubos-update.manifest" "$1/channel-rc/nuubos-update.manifest.sig"
}
serve() { python3 -m http.server -b 127.0.0.1 -d "$1" "$2" >> "$3" 2>&1 & }
serve "$T/a" 18601 "$T/a.log"; PA=$!
serve "$T/b" 18602 "$T/b.log"; PB=$!
serve "$T/c" 18603 "$T/c.log"; PC=$!
sleep 1
U="$T/updatectl"

publish "$T/a" 200 ""
publish "$T/b" 150 ""
: > "$T/b.log"
check "backend answers: update found" '"$U" check | grep -q "OK available 9" && grep -q "serial=200" "$T/dev/data/manifest"'
check "backend answers: GitHub never asked" '[ ! -s "$T/b.log" ]'
check "payload from the backend" '"$U" download | grep -q downloaded && ! grep -q "GET" "$T/b.log"'

rm -rf "$T/a/channel-rc" "$T/dev/data/payload"
check "nothing on the backend (404): GitHub still not asked" '"$U" check >/dev/null; [ ! -s "$T/b.log" ]'

kill $PA; wait $PA 2>/dev/null
check "backend down: GitHub's manifest" '"$U" check | grep -q "OK available" && grep -q "serial=150" "$T/dev/data/manifest"'
check "backend down: GitHub's payload" '"$U" download | grep -q downloaded'

serve "$T/a" 18601 "$T/a.log"; PA=$!; sleep 1
publish "$T/a" 201 "http://127.0.0.1:18601 http://127.0.0.1:18603"
check "sources= saved" '"$U" check >/dev/null && [ "$(wc -l < "$T/dev/update/sources")" = 2 ]'
publish "$T/c" 202 "http://127.0.0.1:18601 http://127.0.0.1:18603"
check "the newest manifest of all sources wins" '"$U" check >/dev/null && grep -q "serial=202" "$T/dev/data/manifest"'
echo "serial=999" >> "$T/c/channel-rc/nuubos-update.manifest"
check "a tampered manifest is skipped" '"$U" check >/dev/null && grep -q "serial=201" "$T/dev/data/manifest"'
rm -f "$T/a/v9/p.img.gz" "$T/dev/data/payload"
publish "$T/c" 203 "http://127.0.0.1:18601 http://127.0.0.1:18603"
check "payload missing on one source: the next one" '"$U" check >/dev/null && "$U" download | grep -q downloaded'

publish "$T/a" 204 ""; rm -rf "$T/c/channel-rc"
check "a manifest without sources= goes back to the compiled backend" '"$U" check >/dev/null && [ ! -e "$T/dev/update/sources" ]'
check "https only from a signed list" 'publish "$T/a" 205 "ftp://x http://127.0.0.1:18603" && "$U" check >/dev/null && [ "$(cat "$T/dev/update/sources")" = "http://127.0.0.1:18603" ]'
[ $FAIL = 0 ] && echo "[PASS] update sources" || echo "[FAIL] update sources"
