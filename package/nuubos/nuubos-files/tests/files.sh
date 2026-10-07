#!/bin/sh
# Host test: nuubos-filesctl allowlist and operations, nuubos-backupctl
# round trip and archive validation, nuubos-sharesctl input checks.
# Run: sh package/nuubos/nuubos-files/tests/files.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
T="$(mktemp -d)"
trap 'rm -rf "${T:?}"' EXIT
FAIL=0
ck() { if eval "$2"; then :; else echo "[FAIL] $1"; FAIL=1; fi; }

U="$T/userdata"; M="$T/media"; S="$T/shares"
mkdir -p "$U/roms/snes" "$U/.nuubos-upload" "$M" "$S" "$T/outside"
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -DUSERDATA_ROOT="\"$U\"" -DMEDIA_ROOT="\"$M\"" \
    -DSHARES_ROOT="\"$S\"" "$SRC/filesctl.c" -o "$T/filesctl" || exit 1
F="$T/filesctl"
echo rom > "$U/roms/snes/a.sfc"; echo secret > "$T/outside/x"; ln -s "$T/outside" "$U/escape"
head -c 3000000 /dev/urandom > "$U/big.bin"

ck "list userdata" '$F list "$U" | grep -q "entry=d.*roms"'
ck "internal folder hidden" '! $F list "$U" | grep -q nuubos-upload'
ck "internal folder refused" '$F list "$U/.nuubos-upload" | grep -q "ERR path"'
ck "outside refused" '$F list "$T/outside" | grep -q "ERR path"'
ck "symlink escape refused" '$F list "$U/escape" | grep -q "ERR path" && $F delete "$U/escape/x" | grep -q "ERR path" && [ -f "$T/outside/x" ]'
ck "dotdot refused" '$F list "$U/roms/../../outside" | grep -q "ERR path"'
ck "root delete refused" '$F delete "$U" | grep -q "ERR path"'
ck "mkdir" '$F mkdir "$U/New Folder" | grep -q OK && [ -d "$U/New Folder" ]'
ck "bad name refused" '$F mkdir "$U/.hidden" | grep -q ERR'
ck "rename" '$F rename "$U/New Folder" "Games" | grep -q OK && [ -d "$U/Games" ]'
ck "rename refuses slash" '$F rename "$U/Games" "a/b" | grep -q ERR'
ck "copy file" '$F copy "$U/roms/snes/a.sfc" "$U/Games" | grep -q OK && [ "$(cat "$U/Games/a.sfc")" = rom ]'
ck "copy never overwrites" '$F copy "$U/roms/snes/a.sfc" "$U/Games" | grep -q "a (2).sfc" && [ -f "$U/Games/a (2).sfc" ]'
OUT="$(NUUBOS_JOB_ID=1 $F copy "$U/big.bin" "$U/Games")"
ck "big copy progress + result" 'echo "$OUT" | grep -q "@progress 100" && echo "$OUT" | grep -q "@result"'
ck "big copy identical" 'cmp -s "$U/big.bin" "$U/Games/big.bin"'
ck "no partial files" '! find "$U" -name "*.part" | grep -q .'
ck "copy into itself refused" '$F copy "$U/Games" "$U/Games" | grep -q "ERR inside"'
ck "move same fs" '$F move "$U/Games/a (2).sfc" "$U/roms" | grep -q OK && [ -f "$U/roms/a (2).sfc" ] && [ ! -e "$U/Games/a (2).sfc" ]'
ck "copy folder" '$F copy "$U/roms" "$U/Games" | grep -q OK && [ -f "$U/Games/roms/snes/a.sfc" ]'
ck "delete folder" '$F delete "$U/Games" | grep -q OK && [ ! -e "$U/Games" ]'
ck "info size" '[ "$($F info "$U/roms" | sed -n "s/^size=//p")" -ge 8 ]'
ck "unmounted media folder not a root" '! $F roots | grep -q "^root=media"'

# Backup & restore
R="$T/r"; ID=11111111-1111-1111-1111-111111111111; ID2=22222222-2222-2222-2222-222222222222
for i in $ID $ID2; do
    mkdir -p "$R/state/users/$i/secrets" "$R/userdata/users/$i/saves/snes" "$R/userdata/users/$i/library"
    printf 'NUUBOS_USER_PROFILE_VERSION=1\nUSER_ID=%s\nUSER_NAME=Name %s\n' "$i" "${i%%-*}" > "$R/state/users/$i/profile.conf"
done
echo "LANGUAGE=it" > "$R/state/users/$ID/localization.conf"
echo "TOKEN" > "$R/state/users/$ID/secrets/key"
echo "save-v1" > "$R/userdata/users/$ID/saves/snes/mario.srm"
echo "h 1" > "$R/userdata/users/$ID/library/history.tsv"
echo "other" > "$R/userdata/users/$ID2/saves/snes/zelda.srm"
mkdir -p "$R/backups"
B() { NUUBOS_ROOT="$R" sh "$SRC/nuubos-backupctl" "$@"; }
OUT="$(B backup $ID "$R/backups")"
FILE="${OUT#OK }"
ck "backup created" '[ -f "$FILE" ]'
ck "no secrets in backup" '! tar -tf "$FILE" | grep -q secrets'
ck "manifest" 'tar -xOf "$FILE" MANIFEST | grep -q "user_id=$ID"'
ck "list finds it" 'B list "$R/backups" | grep -q "Name 11111111"'
echo "save-v2" > "$R/userdata/users/$ID/saves/snes/mario.srm"; echo "LANGUAGE=de" > "$R/state/users/$ID/localization.conf"
ck "restore" 'B restore "$FILE" $ID | grep -q "^OK"'
ck "save restored" '[ "$(cat "$R/userdata/users/$ID/saves/snes/mario.srm")" = save-v1 ]'
ck "settings restored" 'grep -q "LANGUAGE=it" "$R/state/users/$ID/localization.conf"'
ck "identity kept" 'grep -q "USER_ID=$ID" "$R/state/users/$ID/profile.conf"'
ck "other profile untouched" '[ "$(cat "$R/userdata/users/$ID2/saves/snes/zelda.srm")" = other ]'
ck "restore into another profile" 'B restore "$FILE" $ID2 | grep -q "^OK" && [ -f "$R/userdata/users/$ID2/saves/snes/mario.srm" ] && grep -q "USER_ID=$ID2" "$R/state/users/$ID2/profile.conf"'
# Corrupted archive: a file changed after SHA256SUMS.
mkdir "$T/bad" && tar -C "$T/bad" -xf "$FILE" && echo tampered > "$T/bad/userdata/saves/snes/mario.srm" && tar -C "$T/bad" -cf "$T/bad.tar" MANIFEST SHA256SUMS state userdata
echo "save-v3" > "$R/userdata/users/$ID/saves/snes/mario.srm"
ck "checksum mismatch refused" 'B restore "$T/bad.tar" $ID | grep -q "ERR checksum"'
ck "live data untouched after refusal" '[ "$(cat "$R/userdata/users/$ID/saves/snes/mario.srm")" = save-v3 ]'
mkdir "$T/evil" && echo x > "$T/evil/MANIFEST" && (cd "$T/evil" && tar -cf "$T/evil.tar" MANIFEST ../evil/MANIFEST 2>/dev/null)
ck "dotdot archive refused" 'B restore "$T/evil.tar" $ID | grep -q "ERR"'
ck "no staging left" '! ls -a "$R/userdata/users/$ID" | grep -q nuubos-restore'

# Shares input validation (no mount on the host)
SH() { NUUBOS_ROOT="$R" sh "$SRC/nuubos-sharesctl" "$@"; }
ck "share add" 'printf "pw\n" | SH add nas //192.168.1.2/games bob | grep -q OK'
ck "share creds root-only" '[ "$(stat -c %a "$R/state/shares/nas.conf")" = 600 ]'
ck "share bad url" 'printf "pw\n" | SH add x "//host/a,b" | grep -q "ERR url"'
ck "share comma password" 'printf "a,b\n" | SH add y //h/s u | grep -q "ERR password"'
ck "share bad name" 'printf "\n" | SH add "../x" //h/s | grep -q "ERR name"'
ck "share list" 'SH list | grep -q "share=nas	//192.168.1.2/games	bob	0"'

[ "$FAIL" = 0 ] && echo "[PASS] files"
exit "$FAIL"
