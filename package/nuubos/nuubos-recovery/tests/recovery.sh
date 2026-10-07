#!/bin/sh
# Host test for nuubos-recoveryctl scopes and nuubos-ui-supervisor policy.
# Run: sh package/nuubos/nuubos-recovery/tests/recovery.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
CTL="$HERE/../src/nuubos-recoveryctl"
SUP="$HERE/../src/nuubos-ui-supervisor"
T="$(mktemp -d)"
trap 'rm -rf "${T:?}"' EXIT
FAIL=0
U1=11111111-1111-1111-1111-111111111111
U2=22222222-2222-2222-2222-222222222222
ck() { if eval "$2"; then :; else echo "[FAIL] $1"; FAIL=1; fi; }

mk_root()
{
    rm -rf "${T:?}/r"; R="$T/r"
    for u in $U1 $U2; do
        mkdir -p "$R/state/users/$u/secrets" "$R/state/users/$u/controllers" \
                 "$R/userdata/users/$u/appdata" "$R/userdata/users/$u/saves"
        printf 'NUUBOS_USER_PROFILE_VERSION=1\nUSER_ID=%s\nUSER_NAME=x\n' "$u" > "$R/state/users/$u/profile.conf"
        printf 'NUUBOS_USER_SETTINGS_VERSION=1\nFOO=1\n' > "$R/state/users/$u/settings.conf"
        echo c > "$R/state/users/$u/controllers/a.conf"
        echo lang=it > "$R/state/users/$u/localization.conf"
        echo cfg > "$R/userdata/users/$u/appdata/ra.cfg"
        echo save > "$R/userdata/users/$u/saves/game.srm"
    done
    mkdir -p "$R/state/config" "$R/state/network" "$R/state/ssh/dropbear" "$R/state/ssh/root" \
             "$R/userdata/roms/snes" "$R/userdata/bios" "$R/userdata/library" "$R/run/nuubos/user"
    printf 'NUUBOS_CONFIG_VERSION=6\nSETUP_COMPLETE=1\n' > "$R/state/config/nuubos.conf"
    echo psk > "$R/state/network/wpa_supplicant.conf"
    echo hostkey > "$R/state/ssh/dropbear/key"
    echo rom > "$R/userdata/roms/snes/g.sfc"; echo bios > "$R/userdata/bios/b.bin"
    echo $U1 > "$R/run/nuubos/user/active"
}
ctl() { NUUBOS_ROOT="$R" sh "$CTL" "$@"; }

# reset-user: only that user's preferences and appdata.
mk_root
OUT="$(ctl reset-user $U1)"
ck "reset-user answers restart-required for the active user" '[ "$OUT" = "OK restart-required" ]'
ck "reset-user clears controllers" '[ ! -e "$R/state/users/$U1/controllers" ]'
ck "reset-user clears localization" '[ ! -e "$R/state/users/$U1/localization.conf" ]'
ck "reset-user resets settings" '! grep -q FOO "$R/state/users/$U1/settings.conf"'
ck "reset-user clears appdata" '[ -z "$(ls "$R/userdata/users/$U1/appdata")" ]'
ck "reset-user keeps saves" '[ -f "$R/userdata/users/$U1/saves/game.srm" ]'
ck "reset-user keeps profile" '[ -f "$R/state/users/$U1/profile.conf" ]'
ck "reset-user leaves other user" '[ -f "$R/state/users/$U2/localization.conf" ] && [ -f "$R/userdata/users/$U2/appdata/ra.cfg" ]'
ck "reset-user rejects junk id" '! ctl reset-user ../../etc >/dev/null'
ck "reset-user rejects unknown" '! ctl reset-user 33333333-3333-3333-3333-333333333333 >/dev/null'

# repair
mk_root
printf 'SETUP_COMPLETE=1\nSETUP_COMPLETE=0\n' > "$R/state/config/nuubos.conf"
echo broken > "$R/state/users/$U2/profile.conf"
rm "$R/state/users/$U1/settings.conf"
touch "$R/state/config/nuubos.conf.tmp.1"
ctl repair >/dev/null
ck "repair dedups keeping last value" '[ "$(grep -c "^SETUP_COMPLETE=" "$R/state/config/nuubos.conf")" = 1 ] && grep -qx SETUP_COMPLETE=0 "$R/state/config/nuubos.conf"'
ck "repair adds missing keys" 'grep -qx STORAGE_MODE=AUTO "$R/state/config/nuubos.conf"'
ck "repair quarantines damaged profile" '[ ! -d "$R/state/users/$U2" ] && ls "$R/state/recovery/quarantine" | grep -q "$U2"'
ck "repair recreates settings" 'grep -qx NUUBOS_USER_SETTINGS_VERSION=1 "$R/state/users/$U1/settings.conf"'
ck "repair removes temp files" '[ ! -e "$R/state/config/nuubos.conf.tmp.1" ]'

# factory reset: scheduled, then applied
mk_root
ck "factory-reset rejects unknown scope" '! ctl factory-reset everything >/dev/null'
ck "factory-reset schedules" '[ "$(ctl factory-reset settings)" = "OK reboot-required" ] && [ -f "$R/state/recovery/pending-reset" ]'
ck "nothing erased before reboot" '[ -f "$R/state/config/nuubos.conf" ]'
ctl apply-pending state; ctl apply-pending userdata
ck "settings scope wipes STATE config/users/network" '[ ! -e "$R/state/config" ] && [ ! -e "$R/state/users" ] && [ ! -e "$R/state/network" ]'
ck "settings scope keeps SSH host key" '[ -f "$R/state/ssh/dropbear/key" ]'
ck "settings scope keeps USERDATA" '[ -f "$R/userdata/users/$U1/saves/game.srm" ] && [ -f "$R/userdata/roms/snes/g.sfc" ]'
ck "pending flag consumed" '[ ! -e "$R/state/recovery/pending-reset" ]'
mk_root
ctl factory-reset userdata >/dev/null; ctl apply-pending state
ck "userdata kept until its phase" '[ -f "$R/userdata/users/$U1/saves/game.srm" ]'
ctl apply-pending userdata
ck "userdata scope removes per-user data" '[ ! -e "$R/userdata/users/$U1/saves" ] && [ -d "$R/userdata/users" ]'
ck "userdata scope keeps ROMs and BIOS" '[ -f "$R/userdata/roms/snes/g.sfc" ] && [ -f "$R/userdata/bios/b.bin" ]'
ck "userdata scope keeps SSH host key" '[ -f "$R/state/ssh/dropbear/key" ]'

# supervisor
mk_root; mkdir -p "$R/state/recovery"
cat > "$T/crash" <<'EOC'
#!/bin/sh
echo "$NUUBOS_SAFE_MODE" >> "$NUUBOS_TEST_LOG"
exit 7
EOC
chmod +x "$T/crash"
NUUBOS_COMPOSITOR=sh NUUBOS_TEST_LOG="$T/runs" NUUBOS_ROOT="$R" NUUBOS_UI_BIN="$T/crash" sh "$SUP" 2>/dev/null; RC=$?
ck "supervisor gives up (exit 1)" '[ "$RC" = 1 ]'
ck "bounded: 3 normal + 3 safe runs" '[ "$(wc -l < "$T/runs")" = 6 ]'
ck "first 3 runs normal, last 3 safe" '[ "$(head -n3 "$T/runs" | tr -d "\n")" = "" ] && [ "$(tail -n3 "$T/runs" | tr -d "\n")" = "111" ]'
ck "safe flag set, failed marker written" '[ -f "$R/state/recovery/safe-mode" ] && [ -f "$R/run/nuubos/ui-failed" ]'
ctl safe-mode clear >/dev/null
ck "safe-mode clear" '[ ! -f "$R/state/recovery/safe-mode" ] && [ ! -f "$R/run/nuubos/ui-failed" ]'
NUUBOS_COMPOSITOR=no-such-compositor NUUBOS_TEST_LOG="$T/runs2" NUUBOS_ROOT="$R" NUUBOS_UI_BIN="$T/crash" sh "$SUP" 2>/dev/null
ck "compositor gone: error exit is not a crash" '[ $? = 0 ] && [ ! -f "$R/state/recovery/safe-mode" ] && [ "$(wc -l < "$T/runs2")" = 1 ]'
printf '#!/bin/sh\nexit 0\n' > "$T/ok"; chmod +x "$T/ok"
NUUBOS_ROOT="$R" NUUBOS_UI_BIN="$T/ok" sh "$SUP"
ck "clean exit ends supervisor without safe mode" '[ $? = 0 ] && [ ! -f "$R/state/recovery/safe-mode" ]'

[ "$FAIL" = 0 ] && echo "[PASS] recovery"
exit "$FAIL"
