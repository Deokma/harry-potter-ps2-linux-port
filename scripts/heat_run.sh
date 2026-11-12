#!/bin/sh
# A PC run at the device's pace with per-thread CPU and wakeups measured (heat work, docs/PHASE_9_HEAT.md). WSL, repo root:
#   sh scripts/heat_run.sh SECONDS NAME WINDOWS [VAR=VALUE...]
# Route: New Game with no input after the menu (as runner/try_sound.sh), PS2PORT_SLOWDOWN=60, no GIF log.
# WINDOWS: FROM-TO pairs in seconds since the runner started (scripts/heat_threads.py), e.g. 40-70,225-265.
# DISPLAY_ON=1: the GLES display of the device (runner/display.cpp) on SDL's offscreen driver and llvmpipe with no worker threads,
# so the main thread runs the device's present loop; the [m5*] and [lag] lines are logged. GL itself is llvmpipe on the PC.
# Output: /mnt/d/ps2port-data/heat/NAME.threads (the table per window) and NAME.log (the run's [m5*], [lag], [watch] lines).
# PC numbers only: never device numbers.
set -eu
secs=$1
name=$2
windows=$3
shift 3
out=/mnt/d/ps2port-data/heat
mkdir -p "$out"
rm -f "$out/$name.threads" "$out/$name.log"
disp=""
if [ "${DISPLAY_ON:-0}" = 1 ]; then
    disp="PS2PORT_DISPLAY=1 PS2PORT_M5_STATS=1 SDL_VIDEODRIVER=offscreen LP_NUM_THREADS=0 PS2PORT_AUDIO=0"
fi
python3 -I scripts/heat_threads.py runner "$windows" --out "$out/$name.threads" --wait 120 > "$out/$name.sampler" 2>&1 &
sampler=$!
# shellcheck disable=SC2086
DRAW=${DRAW:-0/1} PS2PORT_GIF_LOG= sh runner/try.sh "$secs" "CROSS@5+0.3" PS2PORT_PAD_SEQ=9:START:60,2:CROSS:60 \
    PS2PORT_PRESENT_EVERY=${PRESENT_EVERY:-30} PS2PORT_SLOWDOWN=${SLOWDOWN:-60} PS2PORT_WATCH=0x38c194 \
    $disp "$@" > /dev/null 2>&1 || true
wait "$sampler" 2>/dev/null || true
d=$(ls -dt private/third_party/trial/boot-* | head -1)
grep -a '^\[m5\|^\[lag\]\|^\[watch\]\|^\[display\]\|^\[clock\]' "$d/log.txt" > "$out/$name.log" || true
echo "run: $d"
grep -a '^\[watch\]' "$d/log.txt" | tail -n +2 | tr '\n' ' ' | cut -c1-200
echo
cat "$out/$name.threads" 2>/dev/null
