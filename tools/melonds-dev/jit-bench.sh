#!/bin/sh
# jit-bench.sh GAME SLOT ROUNDS VARIANT...  (bench builds: /tmp/melonds-V.so)
G=$1; SL=$2; N=$3; shift 3
nuubos-emuctl status | grep -q "^state=idle" || { echo BUSY; exit 1; }
U=/userdata/users/$(cat /run/nuubos/user/active)/; C=$U/appdata/retroarch/retroarch-core-options.cfg
. /tmp/opts.sh
opt_set $C melonds_frameskip disabled
opt_set $C melonds_accuracy fast
for kv in $OPTS; do opt_set $C ${kv%%=*} "$(echo "${kv#*=}" | tr _ ' ')"; done
LIB=/usr/lib/libretro/melonds_nuubos_libretro.so
r=1; while [ $r -le $N ]; do
  for x in "$@"; do
    v=${x%+*}; m=${x#*+}; [ "$m" = "$x" ] && m=none
    rm -f /tmp/melon-bench.txt
    mount --bind /tmp/melonds-$v.so $LIB
    nuubos-emuctl launch $G slot:$SL >/dev/null
    t=0; while [ $t -lt 40 ] && ! nuubos-emuctl status | grep -q "^state=running"; do sleep 1; t=$((t+1)); done
    sleep 1; nuubos-emuctl fast-forward >/dev/null
    case $m in iso*)
      t=0; while [ $t -lt 40 ] && [ "$(ls /proc/$(pidof retroarch)/task 2>/dev/null | wc -l)" -lt 8 ]; do sleep 1; t=$((t+1)); done
      sleep 1; P=$(pidof retroarch)
      /tmp/setaff 7 $(ls /proc/$P/task | grep -vx $P); /tmp/setaff 8 $P ;;
    rt*)
      t=0; while [ $t -lt 40 ] && [ "$(ls /proc/$(pidof retroarch)/task 2>/dev/null | wc -l)" -lt 8 ]; do sleep 1; t=$((t+1)); done
      sleep 1; P=$(pidof retroarch); chrt -f -p ${m#rt} $P ;;
    pri*)
      t=0; while [ $t -lt 40 ] && [ "$(ls /proc/$(pidof retroarch)/task 2>/dev/null | wc -l)" -lt 8 ]; do sleep 1; t=$((t+1)); done
      sleep 1; P=$(pidof retroarch); renice -n -${m#pri} -p $P >/dev/null ;;
    esac
    t=0; fq=0; nf=0; while [ $t -lt 120 ] && [ "$(wc -l < /tmp/melon-bench.txt 2>/dev/null)" != 15 ]; do sleep 2; t=$((t+2)); fq=$((fq+$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)/1000)); nf=$((nf+1)); done
    # kill, not quit: a quit save with slot rotation overwrites the next slot
    kill -9 $(pidof retroarch) 2>/dev/null
    for i in $(seq 1 25); do nuubos-emuctl status | grep -q "^state=idle" && break; sleep 1; done
    sleep 2; umount $LIB
    echo "$r $x mhz $((fq/(nf+1))) $(awk '{w+=$2; c+=$3} END {printf "wall %.3f cpu %.3f n %d", w/NR, c/NR, NR}' /tmp/melon-bench.txt) | $(awk '{printf "%s ", $2}' /tmp/melon-bench.txt)"
    cp /tmp/melon-bench.txt /tmp/raw-$r-$x.txt
  done
  r=$((r+1))
done
opt_restore $C
