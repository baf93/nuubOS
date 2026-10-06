#!/usr/bin/env bash
#
# Reproducible UI navigation benchmark on the development device.
#
# A virtual D-pad (tools/uinput-nav, /dev/uinput) replays a fixed pattern
# while per-thread CPU time of the UI and audio processes and the total
# system load are sampled. Use it before/after a change; both runs must use
# the same pattern, interval and screen.
#
# Usage: tools/ui-nav-bench.sh [-H root@host] [pattern] [repeat] [interval_ms]
#   pattern  R L U D (D-pad), A (south), B (east), M (Quick Menu), + - (volume),
#            any other char = pause
#            default RRRRLLLLDUDU, repeated 4 times, 250 ms apart
#
# Note: creating the virtual pad posts a "controller connected"
# notification, which is part of every run.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="root@192.168.5.36"
if [ "${1:-}" = "-H" ]; then HOST="$2"; shift 2; fi
PATTERN="${1:-RRRRLLLLDUDU}"
REPEAT="${2:-4}"
INTERVAL="${3:-250}"
BIN="$ROOT/output/uinput-nav"

if [ ! -x "$BIN" ] || [ "$ROOT/tools/uinput-nav/uinput-nav.c" -nt "$BIN" ]; then
    echo "[INFO] building uinput-nav"
    docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/workspace" -w /workspace nuubos-dev:0.5 \
        output/h700/host/bin/aarch64-buildroot-linux-gnu-gcc -O2 -Wall \
        -o output/uinput-nav tools/uinput-nav/uinput-nav.c || { echo "[FAIL] build"; exit 1; }
fi
scp -q "$BIN" "$HOST:/tmp/uinput-nav" || { echo "[FAIL] copy to $HOST"; exit 1; }

ssh "$HOST" "PAT='$PATTERN' REP='$REPEAT' IV='$INTERVAL' sh -s" <<'EOF'
snap() {
    for n in nuubui-home nuubos-quick-menu labwc nuubos-audiod nuubos-audio-wav-player pipewire wireplumber nuubos-inputd; do
        for p in $(pidof $n); do
            for t in /proc/$p/task/*; do
                set -- $(sed 's/.*) //' $t/stat)
                echo "$n/$(cat $t/comm) $(( ${12} + ${13} ))"
            done
        done
    done
}
load() { awk '/^cpu /{print $2+$3+$4+$7+$8, $2+$3+$4+$5+$6+$7+$8}' /proc/stat; }
snap > /tmp/nb0; set -- $(load); b0=$1; a0=$2
/tmp/uinput-nav "$PAT" "$IV" "$REP"
snap > /tmp/nb1; set -- $(load); b1=$1; a1=$2
echo "pattern=$PAT x$REP every ${IV} ms, $(( (a1-a0)/4 )) ticks wall (100/s)"
echo "system load: $(( 1000*(b1-b0)/(a1-a0) / 10 ))% of 4 cores"
awk 'NR==FNR{a[$1]+=$2; next}{b[$1]+=$2} END{for(k in b) if(b[k]-a[k]>0) printf "%6d ticks  %s\n", b[k]-a[k], k}' /tmp/nb0 /tmp/nb1 | sort -rn | head -12
rm -f /tmp/nb0 /tmp/nb1
EOF
