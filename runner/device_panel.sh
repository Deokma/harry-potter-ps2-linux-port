#!/bin/sh
# Runs on the device, under device_guard.sh: the game on the panel, GLES backend, pad scripted, no audio.
#   device_panel.sh SECONDS PAD PANEL_AT [VAR=VALUE...]
# PANEL_AT is when (seconds, space-separated) to copy /dev/fb0, which is what the panel really shows, to panel-T.raw. Screenshots every 120 vsyncs.
# Results in panel-TIME/: log.txt, frames/, panel-*.raw, fb.txt (the framebuffer's geometry), and the profile when PS2PORT_SAMPLE=samples.bin
# is given (scripts/sample_threads.py). Prints the log's [m5] lines.
secs=$1
pad=$2
at=$3
[ "$#" -ge 3 ] && shift 3 || shift "$#"
cd "$(dirname "$0")" || exit 1
# GPU_MIN_HZ=N and CPU_GOV=NAME among the VAR=VALUE are this script's (as in device_play.sh), not the runner's
ghz=${GPU_MIN_HZ:-648000000}
gov=${CPU_GOV:-}
n=$#
while [ "$n" -gt 0 ]; do
    a=$1
    shift
    n=$((n - 1))
    case $a in
        GPU_MIN_HZ=*) ghz=${a#GPU_MIN_HZ=} ;;
        CPU_GOV=*) gov=${a#CPU_GOV=} ;;
        *) set -- "$@" "$a" ;;
    esac
done
[ -n "$gov" ] && echo "$gov" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
# The GPU's devfreq governor (simple_ondemand) stays at 240 MHz under this load: raise the minimum to the maximum for the run and
# put it back after (GPU_MIN_HZ=0 leaves it alone). At 240 MHz the load-game screen's fog took 58 ms of GPU a frame.
gdev=/sys/class/devfreq/gpu
gold=$(cat $gdev/min_freq 2>/dev/null)
[ -n "$gold" ] && [ "$ghz" != 0 ] && { echo "$ghz" > $gdev/min_freq; trap 'echo "$gold" > $gdev/min_freq' EXIT; trap 'exit 143' TERM INT HUP; }
d=panel-$(date +%H%M%S)
mkdir -p "$d/frames"
chmod +x runner-aarch64
echo "$d" > last-boot.txt
cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/stride > "$d/fb.txt" 2>/dev/null
for t in $at; do
    (sleep "$t"; cat /dev/fb0 > "$d/panel-$t.raw"; cat /sys/class/graphics/fb0/pan >> "$d/fb.txt" 2>/dev/null) &
done
start=$(date +%s)
# The CPU zone has passive trips at 60 and 70 C (power_allocator): above them the kernel lowers the clock whatever is pinned. Every 2 s:
# seconds, CPU kHz, cooling state of the CPU (0 = none, 8 = most), CPU zone temperature, then the cpu0-3 lines of /proc/stat joined by |
# (per-core load: scripts/core_load.py).
# After the temperature: the runner's RssAnon and RssFile and the resident size of its Mali mappings (/dev/mali*), in kB.
# The Mali figure (a walk of smaps, tens of ms on the A53) only every 30 s; in between the line repeats the last (2026-11-12).
( k=0; mali=0; while :; do p=$(pidof runner-aarch64 | cut -d' ' -f1); m="0 0 0"; [ -n "$p" ] && { [ $((k % 15)) = 0 ] && mali=$(awk '/^[0-9a-f]+-/ {k = ($6 ~ /mali/)} k && /^Rss:/ {s += $2} END {print s + 0}' /proc/$p/smaps); m="$(awk '/^RssAnon|^RssFile/ {printf "%s ", $2}' /proc/$p/status) $mali"; }; k=$((k + 1)); echo "$(($(date +%s) - start)) $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) $(cat /sys/class/thermal/cooling_device0/cur_state) $(cat /sys/class/thermal/thermal_zone0/temp) $m $(grep '^cpu[0-9]' /proc/stat | tr '\n' '|')"; sleep 2; done ) > "$d/freq.txt" 2>/dev/null &
fpid=$!
# Memory: every 30 s the runner's eight largest mappings by resident size (maps.txt); above RSS_LIMIT_KB of anonymous memory
# (default 420000; the device has 973 MB and the kernel's OOM killer took the runner at 480 MB) the runner is stopped first.
( n=0; while :; do sleep 2; n=$((n + 2)); p=$(pidof runner-aarch64 | cut -d' ' -f1); [ -z "$p" ] && continue
  a=$(awk '/^RssAnon/ {print $2}' /proc/$p/status)
  if [ $((n % 30)) = 0 ] || [ "${a:-0}" -gt "${RSS_LIMIT_KB:-420000}" ]; then
    echo "t=$n anon=$a"; awk '/^[0-9a-f]+-[0-9a-f]+ / {r = $1; f = $6} /^Rss:/ && $2 > 2048 {print $2, r, f}' /proc/$p/smaps | sort -rn | head -8
  fi
  if [ "${a:-0}" -gt "${RSS_LIMIT_KB:-420000}" ]; then echo "RSS LIMIT: anon $a kB, stopping"; kill -TERM "$p"; sleep 3; kill -KILL "$p" 2>/dev/null; fi
done ) > "$d/maps.txt" 2>/dev/null &
mpid=$!
env PS2PORT_FRAME_LOG="$d/frames.txt" PS2PORT_DISPLAY=1 PS2PORT_M5_STATS=1 PS2PORT_NATIVE_STATS=1 PS2PORT_HEADLESS=1 PS2PORT_RUN_SECONDS="$secs" PS2PORT_PAD="$pad" PS2PORT_FRAME_DIR="$d/frames" "$@" \
    ./runner-aarch64 /userdata/ps2bench/game/SLUS_208.26 > "$d/log.txt" 2>&1
rc=$?
kill "$fpid" "$mpid" 2>/dev/null
grep -a 'RSS LIMIT' "$d/maps.txt"
wait
awk '{ n++; f += $2; c += $3; if ($2 < lo || lo == 0) lo = $2 } END { if (n) printf "clock: avg %.0f kHz, min %d, avg cooling state %.1f (freq.txt)\n", f / n, lo, c / n }' "$d/freq.txt"
echo "exit $rc after $(($(date +%s) - start)) s, $(ls "$d/frames" | wc -l) screenshots: $d"
grep -a '\[native\]\|\[display\]\|\[gles\]\|^gl:' "$d/log.txt" | cut -c1-120
grep -a '^\[m5\]' "$d/log.txt" | cut -c1-240
for f in samples.bin samples.bin.maps samples.bin.threads samples.bin.lr samples.bin.ee; do [ -f "$f" ] && [ "$f" -nt last-boot.txt ] && mv "$f" "$d/"; done
[ -f "$d/samples.bin" ] && echo "samples: $d/samples.bin (PS2PORT_SAMPLE=samples.bin)"
tail -2 "$d/log.txt" | cut -c1-120
