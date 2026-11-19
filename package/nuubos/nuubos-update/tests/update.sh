#!/bin/sh
# Host test: OTA verification (real updatectl, OpenSSL-signed manifest,
# local HTTP server laid out like GitHub Releases) and the A/B slot flow
# (install, trial, confirm, U-Boot fallback with STATE restore) on files.
# Run: sh package/nuubos/nuubos-update/tests/update.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
PARTS="$HERE/../../../../board/nuubos/common/rootfs-overlay/usr/lib/nuubos/partitions.sh"
T="$(mktemp -d)"
trap 'kill "$HTTP" 2>/dev/null; rm -rf "${T:?}"' EXIT
FAIL=0
ck() { if eval "$2"; then :; else echo "[FAIL] $1"; FAIL=1; fi; }
R="$T/r"; U="$R/userdata/.nuubos-update"; W="$T/www"
mkdir -p "$R/etc" "$R/usr/share/nuubos" "$R/state/update" "$R/userdata" "$T/slots" "$T/boot" "$W"
echo 'VERSION_ID=0.5' > "$R/etc/os-release"
echo 100 > "$R/usr/share/nuubos/build-serial"

# Release: system image, payload (gzip of it) in its tag, signed manifest
# in the channel release.
head -c 3000000 /dev/urandom > "$T/system.img"
mkdir -p "$W/v0.6-200" "$W/channel-nightly" "$W/channel-rc"
gzip -c "$T/system.img" > "$W/v0.6-200/nuubos-system-0.6.img.gz"
P="$W/v0.6-200/nuubos-system-0.6.img.gz"
SHA="$(sha256sum < "$P" | cut -d' ' -f1)"; SIZE="$(wc -c < "$P")"
ISHA="$(sha256sum < "$T/system.img" | cut -d' ' -f1)"; ISIZE="$(wc -c < "$T/system.img")"
openssl genpkey -algorithm ed25519 -out "$T/key.pem" 2>/dev/null
manifest() # CHANNEL SERIAL FILE
{
    printf 'format=2\nchannel=%s\nversion=0.6\nserial=%s\nbuild=test\npayload=v0.6-200/nuubos-system-0.6.img.gz\nsize=%s\nsha256=%s\nimage_size=%s\nimage_sha256=%s\nnotes=- First item\\n- Second \\\\ item\n' \
        "$1" "$2" "$SIZE" "$SHA" "$ISIZE" "$ISHA" > "$3"
    openssl pkeyutl -sign -inkey "$T/key.pem" -rawin -in "$3" -out "$3.sig"
}
manifest nightly 200 "$W/channel-nightly/nuubos-update.manifest"
manifest rc 300 "$W/channel-rc/nuubos-update.manifest"
PUB="$(openssl pkey -in "$T/key.pem" -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')"
PORT=$((20000 + $$ % 20000))
(cd "$W" && exec python3 "$HERE/range_server.py" "$PORT" >/dev/null 2>&1) & HTTP=$!
sleep 1
cc_updatectl() # OUTPUT [EXTRA...]
{
    out="$1"; shift
    gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-format-truncation \
        -DUPDATE_SOURCES='"http://127.0.0.1:9"' -DSOURCE_SCHEME='"http://"' -DSOURCES_FILE="\"$R/state/update/sources\"" \
        -DUPDATE_URL="\"http://127.0.0.1:$PORT\"" -DUPDATE_DIR="\"$U\"" -DUPDATE_LOCK="\"$T/update.lock\"" -DOS_RELEASE="\"$R/etc/os-release\"" \
        -DBUILD_ID_FILE="\"$R/usr/share/nuubos/build-id\"" -DBUILD_SERIAL_FILE="\"$R/usr/share/nuubos/build-serial\"" \
        -DCHANNEL_FILE="\"$R/state/update/channel\"" -DAUTO_FILE="\"$R/state/update/auto-off\"" -DUPDATE_DEFAULT_CHANNEL='"nightly"' "$@" \
        "$SRC/updatectl.c" -o "$out" -lcurl -lsodium -lz
}
cc_updatectl "$T/updatectl" -DUPDATE_PUBKEY_HEX="\"$PUB\"" || exit 1
cc_updatectl "$T/updatectl-nokey" || exit 1
C="$T/updatectl"

ck "one update command at a time" 'flock "$T/update.lock" $C check | grep -q "ERR busy" && flock "$T/update.lock" $C download | grep -q "ERR busy"'
ck "status while busy" 'flock "$T/update.lock" $C status | grep -q "^channel="'
ck "no key: unavailable" '"$T/updatectl-nokey" check | grep -q "ERR unavailable"'
ck "default channel nightly" '[ "$($C channel)" = nightly ] && $C status | grep -q "channel=nightly"'
# Nightly sees nightly, rc and stable; the newest wins (rc, serial 300).
ck "check finds 0.6" '$C check | grep -q "OK available 0.6" && grep -q "serial=300" "$U/manifest"'
ck "status with escaped release notes" '$C status | grep -q "available=1" && $C status | grep -qxF "notes=- First item\\n- Second \\\\ item"'
echo 300 > "$R/usr/share/nuubos/build-serial"
ck "older serial never offered" '$C check | grep -q "OK current" && $C status | grep -q "available=0" && $C download | grep -q "ERR current"'
echo 100 > "$R/usr/share/nuubos/build-serial"
# A manifest published under another channel's name is ignored there.
mv "$W/channel-rc" "$T/rc.away"
manifest rc 300 "$W/channel-nightly/nuubos-update.manifest"
ck "other channel's manifest not offered" '$C check | grep -q "OK none"'
manifest nightly 200 "$W/channel-nightly/nuubos-update.manifest"
mv "$T/rc.away" "$W/channel-rc"
# RC sees rc and stable only, stable only stable.
ck "rc does not see nightly" '[ "$($C channel rc)" = OK ] && mv "$W/channel-rc" "$T/rc.away" && [ "$($C check)" = "OK none" ]; R1=$?; mv "$T/rc.away" "$W/channel-rc"; [ "$($C channel nightly)" = OK ] && [ $R1 = 0 ]'
mkdir -p "$W/channel-stable"; manifest stable 250 "$W/channel-stable/nuubos-update.manifest"
ck "stable sees only stable" '[ "$($C channel stable)" = OK ] && $C check | grep -q "OK available" && grep -q "channel=stable" "$U/manifest" && [ "$($C channel)" = stable ]'
ck "nightly: newest of nightly, rc, stable" '[ "$($C channel nightly)" = OK ] && $C check | grep -q "OK available" && grep -q "channel=rc" "$U/manifest" && [ "$($C channel)" = nightly ]'
rm -rf "$W/channel-stable"
ck "invalid channel refused" '$C channel beta | grep -q "ERR channel"'
ck "channel switch" '[ "$($C channel rc)" = OK ] && [ "$($C channel)" = rc ] && $C check | grep -q "OK available 0.6"'
ck "channel switch forgets the offer" '[ "$($C channel nightly)" = OK ] && $C status | grep -q "available=0"'
ck "nothing published on the channel (404): OK none, not a network error" '[ "$($C channel stable)" = OK ] && [ "$($C check)" = "OK none" ] && $C status | grep -q "available=0" && [ "$($C channel nightly)" = OK ]'
echo alpha > "$R/state/update/channel"
ck "old name alpha reads as nightly" '[ "$($C channel)" = nightly ] && $C check | grep -q "OK available 0.6"'
echo beta > "$R/state/update/channel"
ck "old name beta reads as rc" '[ "$($C channel)" = rc ]'
[ "$($C channel nightly)" = OK ] || echo "[FAIL] back to nightly"
$C check >/dev/null

# Resume: start with a correct prefix of the payload.
head -c 100 "$P" > "$U/payload.part"
OUT="$(NUUBOS_JOB_ID=1 $C download)"
ck "download (resumed) verified" 'echo "$OUT" | grep -q "@result downloaded" && cmp -s "$U/payload" "$P"'
ck "no clean marker left after the download" '[ ! -f "$U/payload.part.clean" ]'
# After a clean stop the part is trusted up to the size it recorded.
rm -f "$U/payload"; head -c 100 "$P" > "$U/payload.part"; echo 100 > "$U/payload.part.clean"
ck "clean stop: resumed from its size" '$C download | grep -q "OK downloaded" && cmp -s "$U/payload" "$P"'
# After a crash (no marker) the end of the part may be garbage: dropped.
rm -f "$U/payload"; { head -c 100 "$P"; echo garbage; } > "$U/payload.part"
ck "crash: the part's tail dropped, download verified" '$C download | grep -q "OK downloaded" && cmp -s "$U/payload" "$P"'
# The marker is what the resume trusts: a wrong part with it is resumed.
rm -f "$U/payload"; echo garbage > "$U/payload.part"; echo 8 > "$U/payload.part.clean"
ck "clean marker: the part kept, so a bad one fails its checksum" '$C download | grep -q "ERR checksum" && [ ! -f "$U/payload.part" ]'
$C download >/dev/null
ck "verify" '[ "$($C verify)" = OK ]'
# Tampered manifest is refused, the good one kept.
cp "$W/channel-nightly/nuubos-update.manifest" "$T/good.manifest"
sed -i 's/serial=200/serial=999/' "$W/channel-nightly/nuubos-update.manifest"
mv "$W/channel-rc" "$T/rc.away"
ck "tampered manifest rejected" '$C check | grep -q "ERR signature" && grep -q "serial=300" "$U/manifest"'
mv "$T/rc.away" "$W/channel-rc"
ck "a bad channel does not hide another" '$C check | grep -q "OK available" && grep -q "channel=rc" "$U/manifest"'
cp "$T/good.manifest" "$W/channel-nightly/nuubos-update.manifest"
# Corrupt payload part is not kept.
rm -f "$U/payload"; echo garbage > "$U/payload.part"
cp "$P" "$T/real.gz"; echo corrupt >> "$P"
ck "corrupt download rejected" '$C download | grep -q "ERR" && [ ! -f "$U/payload" ] && [ ! -f "$U/payload.part" ]'
cp "$T/real.gz" "$P"; $C download >/dev/null

# Slots: A running, B inactive.
: > "$T/slots/a.img"; : > "$T/slots/b.img"
echo "root=/dev/mmcblk0p1 nuubos.slot=a rootwait" > "$T/cmdline"
printf '#!/bin/sh\necho "$@" >> "%s/notified.log"\n' "$T" > "$T/notifyctl"; chmod +x "$T/notifyctl"
# SD2 backup stand-in: progress, a marker, then success or the failure in sd2.fail.
printf '#!/bin/sh\n[ "$1 $2" = "run update" ] || exit 2\necho "@progress 50"\n[ -f "%s/sd2.fail" ] && { echo "@error card"; exit 1; }\ndate > "%s/sd2.backup"\necho "@progress 100"\n' "$T" "$T" > "$T/sd2backup"; chmod +x "$T/sd2backup"
mkdir -p "$T/ps/axp20x-battery"; echo 1 > "$T/ps/axp20x-battery/present"; echo 80 > "$T/ps/axp20x-battery/capacity"
A() { NUUBOS_ROOT="$R" NUUBOS_UPDATECTL="$C" NUUBOS_SD2_BACKUP="$T/sd2backup" NUUBOS_BATTERY_DIR="$T/ps" NUUBOS_NOTIFYCTL="$T/notifyctl" NUUBOS_PARTITIONS="$PARTS" NUUBOS_CMDLINE="$T/cmdline" \
      NUUBOS_TEST_SLOTS="$T/slots" NUUBOS_TEST_BOOTPART="$T/boot" sh "$SRC/nuubos-update-apply" "$@"; }
ck "write rejects a corrupt payload" 'cp "$U/payload" "$T/p"; echo x >> "$U/payload"; $C write "$T/slots/b.img" | grep -q "ERR checksum"; R1=$?; cp "$T/p" "$U/payload"; [ $R1 = 0 ]'
echo 30 > "$T/ps/axp20x-battery/capacity"
ck "battery at 30%: refused, nothing written" 'A status | grep -qx "battery=low" && NUUBOS_JOB_ID=1 A install | grep -qx "@error battery" && [ ! -s "$T/slots/b.img" ] && [ ! -f "$T/sd2.backup" ]'
echo 31 > "$T/ps/axp20x-battery/capacity"
ck "battery above 30%: allowed" 'A status | grep -qx "battery=ok"'
echo 0 > "$T/ps/axp20x-battery/present"
ck "no battery: allowed, reported absent" 'A status | grep -qx "battery=absent"'
echo 1 > "$T/ps/axp20x-battery/present"
OUT="$(NUUBOS_JOB_ID=1 A install)"
ck "install writes slot B" 'cmp -s "$T/slots/b.img" "$T/system.img" && echo "$OUT" | grep -q "@result restart-required" && echo "$OUT" | grep -q "@progress 100"'
ck "SD2 backup first, its progress on 0..15" '[ -f "$T/sd2.backup" ] && echo "$OUT" | grep -qx "@progress 7" && echo "$OUT" | grep -qx "@progress 15" && ! echo "$OUT" | grep -q "@progress 50"'
ck "running slot untouched" '[ ! -s "$T/slots/a.img" ]'
ck "pending" 'A status | grep -q "pending=1" && A status | grep -q "slot=a"'
ck "sleep never applies" 'A apply sleep; A status | grep -q "pending=1" && [ ! -f "$T/boot/nuubos-slot.cfg" ]'
echo old > "$R/state/settings"
A apply restart
ck "apply: B on trial" 'grep -qx "nuubos_slot=a" "$T/boot/nuubos-slot.cfg" && grep -qx "nuubos_try=b" "$T/boot/nuubos-slot.cfg" && grep -qx "nuubos_tries=3" "$T/boot/nuubos-slot.cfg" && A status | grep -q "trying=b"'
ck "STATE copied on slot A" 'tar -tf "$R/var/lib/nuubos-update/state.tar" | grep -q settings'
# Trial boot of B: nothing to do until nuubUI confirms.
echo "root=/dev/mmcblk0p5 nuubos.slot=b rootwait" > "$T/cmdline"
echo migrated > "$R/state/settings"
A boot
ck "trial boot keeps STATE" '[ "$(cat "$R/state/settings")" = migrated ] && A status | grep -q "trying=b"'
ck "confirm keeps B" '[ "$(A confirm)" = OK ] && grep -qx "nuubos_slot=b" "$T/boot/nuubos-slot.cfg" && grep -qx "nuubos_try=none" "$T/boot/nuubos-slot.cfg" && A status | grep -q "result=ok" && [ ! -f "$U/payload" ]'
ck "installed announced once" 'grep -qx "post update.result update.installed version=0.5" "$T/notified.log" && [ "$(wc -l < "$T/notified.log")" = 1 ]'
ck "confirm again is a no-op" '[ "$(A confirm)" = OK ] && [ "$(wc -l < "$T/notified.log")" = 1 ]'
# Bootloader: written to SD1 (a file here, behind p1-like data) by confirm
# when it differs, never past its own bytes, not with a low battery.
mkdir -p "$R/usr/share/nuubos/bootloader"; head -c 5000 /dev/urandom > "$R/usr/share/nuubos/bootloader/u-boot-sunxi-with-spl.bin"
head -c 1100000 /dev/zero | tr '\0' 'x' > "$T/disk.img"; cp "$T/disk.img" "$T/disk.orig"
AB() { NUUBOS_TEST_DISK="$T/disk.img" A "$@"; }
echo 20 > "$T/ps/axp20x-battery/capacity"
ck "bootloader: battery at 20% postpones" '[ "$(AB confirm)" = OK ] && cmp -s "$T/disk.img" "$T/disk.orig"'
echo 21 > "$T/ps/axp20x-battery/capacity"
ck "bootloader written at 8 KiB (battery 21%)" '[ "$(AB confirm)" = OK ] && dd if="$T/disk.img" bs=1024 skip=8 2>/dev/null | head -c 5000 | cmp -s - "$R/usr/share/nuubos/bootloader/u-boot-sunxi-with-spl.bin" && cmp -s -n 8192 "$T/disk.img" "$T/disk.orig" && [ "$(tail -c +13193 "$T/disk.img" | head -c 100)" = "$(tail -c +13193 "$T/disk.orig" | head -c 100)" ] && [ -s "$R/state/update/bootloader" ]'
ck "bootloader: same one not rewritten" 'cp "$T/disk.img" "$T/disk.ok"; rm -f "$R/state/update/bootloader"; [ "$(AB confirm)" = OK ] && cmp -s "$T/disk.img" "$T/disk.ok" && [ -s "$R/state/update/bootloader" ]'
echo 80 > "$T/ps/axp20x-battery/capacity"
rm -rf "$R/usr/share/nuubos/bootloader" "$R/state/update/bootloader"

# Next update from B into A, which never starts: U-Boot falls back to B.
# A failed SD2 backup does not stop it, and its error is not the update's.
$C download >/dev/null
touch "$T/sd2.fail"
OUT="$(NUUBOS_JOB_ID=1 A install)"
ck "failed SD2 backup: update goes on" 'echo "$OUT" | grep -q "@result restart-required" && ! echo "$OUT" | grep -q "@error"'
rm -f "$T/sd2.fail"
ck "install writes slot A from B" 'cmp -s "$T/slots/a.img" "$T/system.img" && A status | grep -q "pending=1"'
SNAP="$R/var/lib/nuubos-update"
rm -f "$SNAP/state.tar"
echo before > "$R/state/settings"
A apply poweroff
ck "apply: A on trial" 'grep -qx "nuubos_slot=b" "$T/boot/nuubos-slot.cfg" && grep -qx "nuubos_try=a" "$T/boot/nuubos-slot.cfg"'
# The new system ran once and migrated STATE, then U-Boot gave up on it.
echo migrated > "$R/state/settings"; echo new > "$R/state/added-by-new"
A boot
ck "fallback restores STATE" '[ "$(cat "$R/state/settings")" = before ] && [ ! -e "$R/state/added-by-new" ]'
ck "fallback reported" 'A status | grep -q "result=rolled-back" && ! A status | grep -q "trying=a" && grep -qx "nuubos_try=none" "$T/boot/nuubos-slot.cfg" && grep -qx "nuubos_slot=b" "$T/boot/nuubos-slot.cfg"'
A confirm >/dev/null
ck "rollback announced once at nuubUI ready" 'grep -qx "post update.result update.rolledback" "$T/notified.log" && A confirm >/dev/null && [ "$(grep -c rolledback "$T/notified.log")" = 1 ]'
ck "fallback done once" 'echo later > "$R/state/settings"; A boot; [ "$(cat "$R/state/settings")" = later ]'
# A restore cut short by a power loss is finished at the next boot.
cp "$R/state/settings" "$T/s"; tar -C "$R/state" -cf "$SNAP/state.tar" .; touch "$SNAP/restoring"; rm -f "$R/state/settings"
A boot
ck "interrupted restore finished" '[ "$(cat "$R/state/settings")" = later ] && [ ! -f "$SNAP/restoring" ]'
# Daily automatic check: notifies a new release once, at most once a day.
: > "$T/notified.log"
mkdir -p "$R/run"; rm -f "$R/state/update/notified" "$R/state/update/last-check" "$U/manifest"
echo 100 > "$R/usr/share/nuubos/build-serial"
AU() { NUUBOS_ROOT="$R" NUUBOS_UPDATECTL="$C" NUUBOS_NOTIFYCTL="$T/notifyctl" sh "$SRC/nuubos-update-auto"; }
ck "auto on by default" '[ "$($C auto)" = on ] && $C status | grep -q "auto=1"'
AU
ck "auto check notifies" 'grep -q "post update.available update.available version=0.6" "$T/notified.log" && [ -f "$R/state/update/last-check" ]'
AU
ck "at most once a day" '[ "$(wc -l < "$T/notified.log")" = 1 ]'
echo 0 > "$R/state/update/last-check"; AU
ck "same release announced once" '[ "$(wc -l < "$T/notified.log")" = 1 ] && [ "$(cat "$R/state/update/last-check")" != 0 ]'
echo 0 > "$R/state/update/last-check"; rm -f "$R/state/update/notified"
ck "auto off: no check" '[ "$($C auto off)" = OK ] && AU; [ "$(cat "$R/state/update/last-check")" = 0 ] && [ "$(wc -l < "$T/notified.log")" = 1 ] && $C status | grep -q "auto=0"'
ck "auto on again" '[ "$($C auto on)" = OK ] && AU; [ "$(wc -l < "$T/notified.log")" = 2 ]'
[ "$FAIL" = 0 ] && echo "[PASS] update"
exit "$FAIL"
