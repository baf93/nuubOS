#!/bin/sh
# Host test: a bundle built from a fake root must contain no known secret.
# Run: sh package/nuubos/nuubos-recovery/tests/diag-redaction.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
DIAG="$HERE/../src/nuubos-diag"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
FAIL=0

mkdir -p "$T/root/state/network" "$T/root/state/config" "$T/root/run/nuubos" \
         "$T/root/state/users/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa/secrets" "$T/root/proc" "$T/root/userdata"
cat > "$T/root/state/network/wpa_supplicant.conf" <<'EOC'
network={
    ssid="HomeNet"
    psk="SuperSecretPassphrase1"
    key_mgmt=WPA-PSK
}
network={
    ssid="Corp"
    password="hunter2hunter2"
    identity="alice"
}
EOC
printf 'SSH_PASSWORD=swordfish\nSETUP_COMPLETE=1\nTOKEN: abc123tokenvalue\n' > "$T/root/state/config/nuubos.conf"
printf 'pairing pin=4821 host\nAuthorization: Bearer zzzsecretzzz\nkey 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\npinctrl-sunxi: ok\n' > "$T/root/run/nuubos/streamd.log"
echo 'NEVER-IN-BUNDLE-SSHKEY' > "$T/root/state/users/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa/secrets/key"
printf 'NUUBOS_USER_PROFILE_VERSION=1\n' > "$T/root/state/users/aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa/profile.conf"

OUT="$(NUUBOS_ROOT="$T/root" sh "$DIAG" bundle "$T/out")" || { echo "[FAIL] bundle command"; exit 1; }
[ -f "$OUT" ] || { echo "[FAIL] no bundle at $OUT"; exit 1; }
mkdir "$T/x" && case "$OUT" in *.gz) gzip -dc "$OUT" | tar -C "$T/x" -xf - ;; *) tar -C "$T/x" -xf "$OUT" ;; esac

for s in SuperSecretPassphrase1 hunter2hunter2 swordfish abc123tokenvalue zzzsecretzzz \
         0123456789abcdef0123456789abcdef NEVER-IN-BUNDLE-SSHKEY 4821; do
    if grep -rqF "$s" "$T/x"; then echo "[FAIL] secret leaked: $s"; FAIL=1; fi
done
for k in HomeNet "SETUP_COMPLETE=1" "pinctrl-sunxi: ok"; do
    grep -rqF "$k" "$T/x" || { echo "[FAIL] useful data lost: $k"; FAIL=1; }
done
ls "$T/out" | grep -q '^\.stage' && { echo "[FAIL] staging left behind"; FAIL=1; }
[ "$FAIL" = 0 ] && echo "[PASS] diag redaction"
exit "$FAIL"
