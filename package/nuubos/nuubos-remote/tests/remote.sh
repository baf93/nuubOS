#!/bin/sh
# Host test: nuubos-remotectl credential/policy and the Web API CGI.
# Run: sh package/nuubos/nuubos-remote/tests/remote.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
CTL="$HERE/../src/nuubos-remotectl"
CGI="$HERE/../src/web/api.cgi"
T="$(mktemp -d)"
trap 'rm -rf "${T:?}"' EXIT
FAIL=0
ck() { if eval "$2"; then :; else echo "[FAIL] $1"; FAIL=1; fi; }

# Fake tools: libraryctl, biosctl.
mkdir -p "$T/bin"
cat > "$T/bin/libraryctl" <<'EOC'
#!/bin/sh
echo "$*" >> "$LOG"
case "$1" in
  status) printf 'user=u\nscanning=0\ngames=2\nsystem=snes\tSuper Nintendo\t2\t1400\t#000\t\nend=1\n' ;;
  games) printf 'game=0123456789abcdef\tsnes\tMario "World"\t\t1400\t0\t0\t1\t0\nend=1\n' ;;
  delete) [ "$2" = 0123456789abcdef ] && [ "$3" = confirm ] && echo OK ;;
  scan) echo OK ;;
esac
EOC
printf '#!/bin/sh\nprintf "system=psx\\tPlayStation\\tpcsx\\tmissing\\t0\\t1\\nfile=psx\\tscph5501.bin\\t1\\tmissing\\tUS BIOS\\nend=1\\n"\n' > "$T/bin/biosctl"
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH" LOG="$T/calls"

R="$T/root"
mkdir -p "$R/etc" "$R/state/ssh/root"
printf 'root::::::::\ndaemon:*:::::::\n' > "$R/etc/shadow"
ctl() { NUUBOS_ROOT="$R" NUUBOS_REMOTE_NO_DAEMONS=1 sh "$CTL" "$@"; }

ctl start
PW="$(ctl credential show)"
ck "credential generated, formatted" 'echo "$PW" | grep -Eq "^[a-z2-9]{4}-[a-z2-9]{4}-[a-z2-9]{4}$"'
ck "credential root-only" '[ "$(stat -c %a "$R/state/remote/credential")" = 600 ]'
ck "/etc/shadow untouched (no login password)" '[ ! -L "$R/etc/shadow" ] && [ "$(head -n1 "$R/etc/shadow")" = "root::::::::" ]'
ck "web digest written" '[ "$(cut -d: -f1,2 "$R/state/remote/web.htdigest")" = "root:nuubOS" ]'
ck "digest value" '[ "$(cut -d: -f3 "$R/state/remote/web.htdigest")" = "$(printf "%s" "root:nuubOS:$PW" | md5sum | cut -d" " -f1)" ]'
ck "services disabled by default" 'ctl status | grep -q "^ssh=0" && ctl status | grep -q "^web=0" && ctl status | grep -q "^smb=0"'
ck "restart keeps the credential" 'ctl start; [ "$(ctl credential show)" = "$PW" ]'
ck "enable web" '[ "$(ctl set web 1)" = OK ] && ctl status | grep -q "^web=1"'
ck "bad service rejected" '! ctl set telnet 1 >/dev/null'
ctl credential regenerate >/dev/null
PW2="$(ctl credential show)"
ck "regenerate changes it" '[ "$PW2" != "$PW" ] && [ "$(cut -d: -f3 "$R/state/remote/web.htdigest")" = "$(printf "%s" "root:nuubOS:$PW2" | md5sum | cut -d" " -f1)" ]'

# A development card with an authorized SSH key keeps SSH on migration.
R2="$T/root2"; mkdir -p "$R2/etc" "$R2/state/ssh/root"
printf 'root::::::::\n' > "$R2/etc/shadow"; echo "ssh-ed25519 AAAA dev" > "$R2/state/ssh/root/authorized_keys"
NUUBOS_ROOT="$R2" NUUBOS_REMOTE_NO_DAEMONS=1 sh "$CTL" start
ck "dev card keeps SSH" 'NUUBOS_ROOT="$R2" NUUBOS_REMOTE_NO_DAEMONS=1 sh "$CTL" status | grep -q "^ssh=1"'
ck "two devices, two credentials" '[ "$(NUUBOS_ROOT="$R2" NUUBOS_REMOTE_NO_DAEMONS=1 sh "$CTL" credential show)" != "$PW2" ]'

# Web API
W="$T/w"; mkdir -p "$W/userdata/roms/snes" "$W/userdata/bios" "$W/usr/share/nuubos"
printf 'snes|Super Nintendo|snes|sfc,smc|1400|#000|\npsx|PlayStation|psx|cue,chd|1000|#000|\n' > "$W/usr/share/nuubos/systems.conf"
cgi() { # method query [header] < body
    env NUUBOS_ROOT="$W" NUUBOS_LIBRARYCTL="$T/bin/libraryctl" NUUBOS_BIOSCTL="$T/bin/biosctl" \
        REQUEST_METHOD="$1" QUERY_STRING="$2" HTTP_X_NUUBOS="${3:-}" CONTENT_LENGTH="${4:-}" sh "$CGI"
}
ck "status json" 'cgi GET "op=status" </dev/null | grep -q "\"games\":2"'
ck "systems json" 'cgi GET "op=systems" </dev/null | grep -q "\"id\":\"psx\""'
ck "games json escapes quotes" 'cgi GET "op=games&system=snes" </dev/null | grep -q "Mario \\\\\"World\\\\\""'
ck "bios json" 'cgi GET "op=bios" </dev/null | grep -q "\"path\":\"scph5501.bin\""'
ck "scan needs CSRF header" 'cgi POST "op=scan" </dev/null | grep -q "403"'
ck "scan with header" 'cgi POST "op=scan" 1 </dev/null | grep -q "ok"'
ck "scan needs POST" 'cgi GET "op=scan" 1 </dev/null | grep -q "405"'
printf 'ROMDATA' > "$T/rom"
ck "upload" 'cgi PUT "op=upload&root=roms&path=snes%2FNew%20Game.sfc" 1 7 < "$T/rom" | grep -q ok && [ "$(cat "$W/userdata/roms/snes/New Game.sfc")" = ROMDATA ]'
ck "upload triggers scan" 'grep -q "^scan" "$LOG"'
ck "traversal rejected" 'cgi PUT "op=upload&root=roms&path=..%2F..%2Fetc%2Fpasswd" 1 7 < "$T/rom" | grep -q "400" && [ ! -e "$W/etc/passwd" ]'
ck "absolute rejected" 'cgi PUT "op=upload&root=bios&path=%2Fetc%2Fx" 1 7 < "$T/rom" | grep -q "400"'
ck "hidden part rejected" 'cgi PUT "op=upload&root=roms&path=snes%2F.hidden" 1 7 < "$T/rom" | grep -q "400"'
ck "unknown root rejected" 'cgi PUT "op=upload&root=state&path=x" 1 7 < "$T/rom" | grep -q "400"'
ck "upload needs CSRF" 'cgi PUT "op=upload&root=bios&path=a.bin" "" 7 < "$T/rom" | grep -q "403" && [ ! -e "$W/userdata/bios/a.bin" ]'
ck "incomplete upload leaves nothing" 'cgi PUT "op=upload&root=bios&path=b.bin" 1 99 < "$T/rom" | grep -q "400" && [ -z "$(ls "$W/userdata/bios")" ]'
ck "bios upload" 'cgi PUT "op=upload&root=bios&path=scph5501.bin" 1 7 < "$T/rom" | grep -q ok'
ck "delete via libraryd" 'cgi POST "op=delete&game=0123456789abcdef" 1 </dev/null | grep -q ok && grep -q "delete 0123456789abcdef confirm" "$LOG"'
ck "delete bad id" 'cgi POST "op=delete&game=../x" 1 </dev/null | grep -q "400"'

[ "$FAIL" = 0 ] && echo "[PASS] remote"
exit "$FAIL"
