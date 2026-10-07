#!/bin/sh
# Host test: nuubos-themectl validation, install/update/remove, selection.
# Run: sh package/nuubos/nuubos-themes/tests/themes.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
CTL="$HERE/../src/nuubos-themectl"
T="$(mktemp -d)"
trap 'rm -rf "${T:?}"' EXIT
FAIL=0
ck() { if eval "$2"; then :; else echo "[FAIL] $1"; FAIL=1; fi; }
R="$T/r"; U=11111111-1111-1111-1111-111111111111
mkdir -p "$R/usr/share/nuubos" "$R/state/users/$U" "$R/userdata"
cp -r "$HERE/../src/themes" "$R/usr/share/nuubos/themes"
printf 'NUUBOS_USER_SETTINGS_VERSION=1\n' > "$R/state/users/$U/settings.conf"
c() { NUUBOS_ROOT="$R" sh "$CTL" "$@"; }

ck "built-ins valid and listed" '[ "$(c list | grep -c "^theme=")" = 4 ]'
ck "default selected when none" 'c selected $U | grep -qx "id=nuubos"'
mk() { mkdir -p "$T/$1"; printf "%s" "$2" > "$T/$1/theme.conf"; }
GOOD='FORMAT=1
ID=neon
NAME=Neon
AUTHOR=Someone
VERSION=1.0
MIN_NUUBUI=0.5
accent=#ff00ff
'
mk neon "$GOOD"
ck "validate good" '[ "$(c validate "$T/neon")" = OK ]'
mk bad1 "$(printf '%s' "$GOOD" | sed s/FORMAT=1/FORMAT=2/)"; ck "format rejected" 'c validate "$T/bad1" | grep -q "ERR format"'
mk bad2 "$(printf '%s\nexec=rm -rf /\n' "$GOOD")"; ck "unknown key rejected" 'c validate "$T/bad2" | grep -q "ERR token exec"'
mk bad3 "$(printf '%s' "$GOOD" | sed 's/#ff00ff/red/')"; ck "bad color rejected" 'c validate "$T/bad3" | grep -q "ERR token accent"'
mk bad4 "$(printf '%s' "$GOOD" | sed 's/MIN_NUUBUI=0.5/MIN_NUUBUI=9.0/')"; ck "incompatible rejected" 'c validate "$T/bad4" | grep -q "ERR incompatible"'
mk bad5 "$(printf '%s' "$GOOD" | sed 's/ID=neon/ID=..\/x/')"; ck "bad id rejected" 'c validate "$T/bad5" | grep -q "ERR id"'
ck "install folder" 'c install "$T/neon" | grep -qx "OK neon" && [ -f "$R/userdata/themes/neon/theme.conf" ]'
ck "installed listed" 'c list | grep -q "^theme=neon	Neon	Someone	1.0	0"'
(cd "$T" && mkdir -p pkg/neon && printf '%s' "$GOOD" | sed 's/VERSION=1.0/VERSION=2.0/' > pkg/neon/theme.conf && tar -C pkg -cf neon2.tar neon)
ck "update from tar" 'c install "$T/neon2.tar" | grep -qx "OK neon" && grep -q "VERSION=2.0" "$R/userdata/themes/neon/theme.conf"'
ck "invalid update keeps old" '! c install "$T/bad3" >/dev/null; grep -q "VERSION=2.0" "$R/userdata/themes/neon/theme.conf"'
ck "builtin id refused" 'mkdir -p "$T/fake" && sed "s/ID=neon/ID=nuubos/" "$T/neon/theme.conf" > "$T/fake/theme.conf" && c install "$T/fake" | grep -q "ERR builtin"'
ck "select" '[ "$(c select $U neon)" = OK ] && c selected $U | grep -qx "id=neon"'
ck "select unknown refused" 'c select $U nope | grep -q ERR'
ck "remove builtin refused" 'c remove nuubos | grep -q "ERR builtin"'
ck "remove resets users" '[ "$(c remove neon)" = OK ] && c selected $U | grep -qx "id=nuubos" && grep -qx "THEME=nuubos" "$R/state/users/$U/settings.conf"'
ck "broken selection falls back" 'echo "THEME=ghost" >> "$R/state/users/$U/settings.conf" && c selected $U | grep -qx "id=nuubos"'
ck "no staging left" '! ls -a "$R/userdata/themes" | grep -q "^\.stage"'
[ "$FAIL" = 0 ] && echo "[PASS] themes"
exit "$FAIL"
