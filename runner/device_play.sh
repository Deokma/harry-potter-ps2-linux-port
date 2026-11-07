#!/bin/sh
# Runs on the device, under device_guard.sh: the game on the panel with the device's own controls (runner/pad_sdl.cpp), no audio.
#   device_play.sh [VAR=VALUE...]
# Quit: Menu + Start, or L1 + R1 + Select + Start, held for a second. Log and frame times in play-TIME/.
cd "$(dirname "$0")" || exit 1
d=play-$(date +%H%M%S)
mkdir -p "$d"
chmod +x runner-aarch64
env PS2PORT_DISPLAY=1 PS2PORT_HEADLESS=1 PS2PORT_PAD_LOG=1 PS2PORT_FRAME_LOG="$d/frames.txt" "$@" \
    ./runner-aarch64 /userdata/ps2bench/game/SLUS_208.26 > "$d/log.txt" 2>&1
rc=$?
echo "exit $rc after $(tail -1 "$d/frames.txt" 2>/dev/null | awk '{printf "%.0f", $1 / 1000}') s: $d"
grep -a '^\[pad\]' "$d/log.txt" | grep -av '\] t=' | cut -c1-200
echo "$(grep -a -c '^\[pad\] t=' "$d/log.txt") pad changes logged"
