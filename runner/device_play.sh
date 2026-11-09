#!/bin/sh
# Runs on the device, under device_guard.sh: the game on the panel with the device's own controls (runner/pad_sdl.cpp), no audio.
#   device_play.sh [VAR=VALUE...]
# Quit: Menu + Start, or L1 + R1 + Select + Start, held for a second. Log and frame times in play-TIME/.
cd "$(dirname "$0")" || exit 1
d=play-$(date +%H%M%S)
mkdir -p "$d"
chmod +x runner-aarch64
# A memory stop before the kernel's OOM killer (which took the runner at 480 MB anonymous in the first play sessions)
( while sleep 2; do p=$(pidof runner-aarch64 | cut -d' ' -f1); [ -z "$p" ] && continue
  a=$(awk '/^RssAnon/ {print $2}' /proc/$p/status)
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
