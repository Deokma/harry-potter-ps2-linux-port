#!/bin/sh
# The x86 runner through the GLES backend (PS2PORT_DISPLAY=1, Mesa in WSLg): New Game to play with no input after the menu, with
# the display's 5-s statistics and a captured frame every 120 displayed frames, for before/after checks of gsgles/ (PHASE_9_GS).
# WSL, repo root:  sh scripts/gs_runner_gles.sh SECONDS [VAR=VALUE...]      (add PS2PORT_SLOWDOWN=60 for the device's pace)
# Writes the boot folder as runner/try.sh does (frames in its frames/). Prints the play mode's arrival time, the last [m5] lines,
# GL work per game frame from 240 s (scripts/gs_m5w.py), the strip counter (PS2PORT_GS_STRIP_LOG=1) and frame stats.
set -eu
secs=$1; shift
DRAW=0/1 PS2PORT_GIF_LOG= sh runner/try.sh "$secs" "CROSS@5+0.3" PS2PORT_PAD_SEQ=9:START:60,2:CROSS:60 PS2PORT_DISPLAY=1 PS2PORT_M5_STATS=1 \
    PS2PORT_WATCH=0x38c194 PS2PORT_FRAME_EVERY=120 "$@" > /dev/null || true
d=$(ls -dt private/third_party/trial/boot-* | head -1)
echo "$d"
grep -a '^\[watch\]' "$d/log.txt" | tr '\n' ' ' | grep -o '\[watch\] [0-9.]* f[0-9]* 5' | head -1 || echo "play mode not reached"
grep -a '^\[m5\]' "$d/log.txt" | tail -3 | cut -c1-260
python3 -I scripts/gs_m5w.py "$d/log.txt" 240
grep -a '^\[gs-strip\]' "$d/log.txt" | tail -1 || true
python3 -I scripts/frame_stats_ppm.py "$d/frames" | tail -3
