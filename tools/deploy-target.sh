#!/usr/bin/env bash
#
# Targeted deploy of built files from output/h700/target to the development
# device, without flashing.
#
# Usage:
#   tools/deploy-target.sh [-H root@host] [-s init-script ...] path [path ...]
#
#   path        absolute target path, e.g. /usr/bin/nuubui-home
#               (taken from output/h700/target/<path>)
#   -s script   init script basename to stop before / start after install,
#               e.g. -s S50nuubos-ui -s S43nuubos-audiod (stopped in the given
#               order, started in reverse order)
#
# The target has BusyBox only (no stat/pkill): keep remote commands minimal.
# Originals are backed up on the target in /tmp/nuubos-deploy/backup.
# Staged files are verified by md5 before anything is installed.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_ROOT="$ROOT/output/h700/target"
HOST="root@192.168.5.36"
SCRIPTS=()
PATHS=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        -H) HOST="$2"; shift 2 ;;
        -s) SCRIPTS+=("$2"); shift 2 ;;
        *) PATHS+=("$1"); shift ;;
    esac
done

if [ "${#PATHS[@]}" -eq 0 ]; then
    echo "[FAIL] no paths given"
    exit 2
fi

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "$HOST")
LOCAL_SUMS=""
for P in "${PATHS[@]}"; do
    if [ ! -f "$TARGET_ROOT$P" ]; then
        echo "[FAIL] missing build artifact $TARGET_ROOT$P"
        exit 1
    fi
    LOCAL_SUMS+="$(md5sum "$TARGET_ROOT$P" | awk '{print $1}')  $P"$'\n'
done

"${SSH[@]}" 'rm -rf /tmp/nuubos-deploy/stage && mkdir -p /tmp/nuubos-deploy/stage /tmp/nuubos-deploy/backup' || { echo "[FAIL] target unreachable"; exit 1; }

for P in "${PATHS[@]}"; do
    "${SSH[@]}" "mkdir -p /tmp/nuubos-deploy/stage$(dirname "$P") /tmp/nuubos-deploy/backup$(dirname "$P"); [ -f '$P' ] && cp -a '$P' '/tmp/nuubos-deploy/backup$P'; true"
    scp -q "$TARGET_ROOT$P" "$HOST:/tmp/nuubos-deploy/stage$P" || { echo "[FAIL] copy $P"; exit 1; }
done

REMOTE_SUMS="$("${SSH[@]}" "cd /tmp/nuubos-deploy/stage && for P in ${PATHS[*]}; do md5sum .\$P | sed 's#  \\.#  #'; done")"
if [ "$(printf '%s' "$LOCAL_SUMS" | sort)" != "$(printf '%s\n' "$REMOTE_SUMS" | sort)" ]; then
    echo "[FAIL] staged checksum mismatch"
    exit 1
fi
echo "[PASS] staged ${#PATHS[@]} file(s) verified"

STOP=""
for S in "${SCRIPTS[@]}"; do STOP+="/etc/init.d/$S stop >/dev/null 2>&1; "; done
START=""
for (( i=${#SCRIPTS[@]}-1; i>=0; i-- )); do START+="/etc/init.d/${SCRIPTS[$i]} start >/dev/null 2>&1; "; done
INSTALL=""
for P in "${PATHS[@]}"; do
    MODE="$(stat -c %a "$TARGET_ROOT$P")"
    INSTALL+="install -D -m $MODE /tmp/nuubos-deploy/stage$P $P && "
done

"${SSH[@]}" "$STOP if $INSTALL sync; then echo '[PASS] installed'; else echo '[FAIL] install failed'; fi; $START true"
echo "[INFO] backups on target: /tmp/nuubos-deploy/backup"
