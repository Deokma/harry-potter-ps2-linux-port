#!/bin/sh
# Runs on the device, under device_guard.sh: the game on the panel with the device's own controls (runner/pad_sdl.cpp), no audio.
#   device_play.sh [VAR=VALUE...]
# Quit: Menu + Start, or L1 + R1 + Select + Start, held for a second. Log and frame times in play-TIME/.
# Switches for this script (taken out of VAR=VALUE, not passed to the runner; docs/ENV_VARS.md):
#   GPU_MIN_HZ=N   the GPU's minimum clock for the session (default 648000000; 0 leaves the governor alone)
#   CPU_GOV=NAME   the CPU governor for the session under the guard's clock cap (default: the guard's performance; schedutil
#                  lets the clock drop when the cores idle). The CPU clock itself: CPU_KHZ=N on scripts/play.sh.
#   THROTTLE_MC=N  a soft step before the 85 C stop (default off): above N milli-C the CPU's ceiling goes down to THROTTLE_KHZ
#                  (default 1200000) and comes back 6 C below N; the session goes on slower instead of ending. The 85 C stop stays.
cd "$(dirname "$0")" || exit 1
ghz=${GPU_MIN_HZ:-648000000}
gov=${CPU_GOV:-}
thr=${THROTTLE_MC:-0}
thrkhz=${THROTTLE_KHZ:-1200000}
n=$#
while [ "$n" -gt 0 ]; do
    a=$1
    shift
    n=$((n - 1))
    case $a in
        GPU_MIN_HZ=*) ghz=${a#GPU_MIN_HZ=} ;;
        CPU_GOV=*) gov=${a#CPU_GOV=} ;;
        THROTTLE_MC=*) thr=${a#THROTTLE_MC=} ;;
        THROTTLE_KHZ=*) thrkhz=${a#THROTTLE_KHZ=} ;;
        *) set -- "$@" "$a" ;;
    esac
done
# The GPU's devfreq governor (simple_ondemand) stays at 240 MHz under this load: raise the minimum for the run and put it back
# after (GPU_MIN_HZ=0 leaves it alone). At 240 MHz the load-game screen's fog took 58 ms of GPU a frame.
gdev=/sys/class/devfreq/gpu
gold=$(cat $gdev/min_freq 2>/dev/null)
[ -n "$gold" ] && [ "$ghz" != 0 ] && { echo "$ghz" > $gdev/min_freq; trap 'echo "$gold" > $gdev/min_freq' EXIT; trap 'exit 143' TERM INT HUP; }
# The CPU governor (device_guard.sh pinned the clock's ceiling and set performance; it puts its own values back at the end)
cdev=/sys/devices/system/cpu/cpu0/cpufreq
[ -n "$gov" ] && echo "$gov" > $cdev/scaling_governor 2>/dev/null
d=play-$(date +%H%M%S)
mkdir -p "$d"
chmod +x runner-aarch64
echo "clocks: CPU $(cat $cdev/scaling_max_freq 2>/dev/null) kHz max, governor $(cat $cdev/scaling_governor 2>/dev/null); GPU min $(cat $gdev/min_freq 2>/dev/null) Hz" | tee "$d/clocks.txt"
# Every 2 s, with the shell's own `read` only (no process started but sleep): freq.txt in device_panel.sh's layout (seconds, CPU kHz,
# CPU cooling state, CPU zone milli-C, then GPU Hz and the runner's RssAnon kB, then the cpu0-3 lines of /proc/stat joined by |;
# scripts/core_load.py reads it), and a memory stop before the kernel's OOM killer (which took the runner at 480 MB anonymous in
# the first play sessions).
( p=""; t0=""; pin=""; read -r pin < $cdev/scaling_max_freq; stepped=0
  while sleep 2; do
    read -r up _ < /proc/uptime; up=${up%.*}; [ -z "$t0" ] && t0=$up
    if [ -z "$p" ] || [ ! -d "/proc/$p" ]; then p=$(pidof runner-aarch64 | cut -d' ' -f1); fi
    a=0
    if [ -n "$p" ]; then
      while read -r k v _; do [ "$k" = "RssAnon:" ] && { a=$v; break; }; done < "/proc/$p/status"
    fi
    read -r khz < $cdev/scaling_cur_freq; read -r cool < /sys/class/thermal/cooling_device0/cur_state
    read -r temp < /sys/class/thermal/thermal_zone0/temp; g=0; read -r g < $gdev/cur_freq 2>/dev/null
    s=""; while read -r c rest; do case $c in cpu[0-9]*) s="$s$c $rest|" ;; esac; done < /proc/stat
    echo "$((up - t0)) $khz $cool $temp $g ${a:-0} $s" >> "$d/freq.txt"
    if [ "$thr" -gt 0 ] && [ -n "$pin" ]; then
      if [ "$stepped" = 0 ] && [ "$temp" -gt "$thr" ]; then
        echo "$thrkhz" > $cdev/scaling_max_freq; stepped=1; echo "[heat] t=$((up - t0)) s $temp m-C: CPU ceiling $pin -> $thrkhz kHz" >> "$d/freq.log"
      elif [ "$stepped" = 1 ] && [ "$temp" -lt $((thr - 6000)) ]; then
        echo "$pin" > $cdev/scaling_max_freq; stepped=0; echo "[heat] t=$((up - t0)) s $temp m-C: CPU ceiling back to $pin kHz" >> "$d/freq.log"
      fi
    fi
    if [ "${a:-0}" -gt 420000 ]; then echo "MEMORY STOP: runner at $a kB anonymous"; kill -TERM "$p"; sleep 3; kill -KILL "$p" 2>/dev/null; fi
  done ) > "$d/memory.txt" 2>/dev/null &
mpid=$!
env PS2PORT_DISPLAY=1 PS2PORT_HEADLESS=1 PS2PORT_PAD_LOG=1 PS2PORT_MC_LOG=1 PS2PORT_LAG_LOG=1 PS2PORT_FRAME_LOG="$d/frames.txt" "$@" \
    ./runner-aarch64 /userdata/ps2bench/game/SLUS_208.26 > "$d/log.txt" 2>&1
rc=$?
kill "$mpid" 2>/dev/null
cat "$d/memory.txt"
echo "exit $rc after $(tail -1 "$d/frames.txt" 2>/dev/null | awk '{printf "%.0f", $1 / 1000}') s: $d"
grep -a '^\[pad\]' "$d/log.txt" | grep -av '\] t=' | cut -c1-200
echo "$(grep -a -c '^\[pad\] t=' "$d/log.txt") pad changes logged"
# memory card writes: open (0x02), write (0x06), mkdir (0x0b), and their results
grep -a '^\[mc\] cmd 0x0[26b]' "$d/log.txt" | sort | uniq -c | head -6
# heat: temperature and clock over the session, per-minute rows (scripts/core_load.py on freq.txt gives per-core load)
awk '{ m = int($1 / 60); if (!(m in seen)) { seen[m] = 1; printf "min %d: %.1f C, CPU %d kHz, cooling %d, GPU %d MHz\n", m, $4 / 1000, $2, $3, $5 / 1e6 } }' "$d/freq.txt" 2>/dev/null | tail -40
cat "$d/freq.log" 2>/dev/null | tail -20
grep -a '^\[lag\]\|^\[m5\]' "$d/log.txt" | tail -3 | cut -c1-240
