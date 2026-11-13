#!/bin/sh
# Game-thread profile of play on the PC (EE worker, docs/PHASE_9_EE.md). WSL, from the repo root:
#   sh scripts/ee_profile_play.sh NAME [RUNNER] [VAR=VALUE...]
# New Game with no input after the menu (runner/try_sound.sh's pad route), 270 s at full PC speed, samples from 200 to 260 s
# (gameplay, mode 5, starts at ~183 s). Writes /mnt/d/ps2port-data/workers/NAME.{samples*,log} and prints the game frames in
# the window, GameThread samples per game frame and the top groups/functions. PC numbers: not device timings.
# One run at a time: the log is taken from the newest boot-* folder (two runs started together get the same one).
set -eu
name=$1
runner=${2:-$PWD/private/third_party/build-m2/x86/runner}
[ "$#" -ge 2 ] && shift 2 || shift 1
out=/mnt/d/ps2port-data/workers
mkdir -p "$out"
rm -f "$out/$name".samples*
DRAW=0/1 PS2PORT_GIF_LOG= env PS2PORT_NATIVE_STATS=1 PS2PORT_GS_DRAW_WINDOW=0/1 PS2PORT_RUNNER="$runner" PS2PORT_STATS=1 \
    PS2PORT_PAD_SEQ=9:START:60,2:CROSS:60 PS2PORT_PRESENT_EVERY=30 PS2PORT_WATCH=0x38c194 \
    PS2PORT_SAMPLE="$out/$name.samples" PS2PORT_SAMPLE_FROM=${FROM:-200} PS2PORT_SAMPLE_TO=${TO:-260} "$@" \
    sh scripts/ps2recomp_trial.sh boot "${SECS:-270}" "CROSS@5+0.3" > /dev/null 2>&1 || true
d=$(ls -dt private/third_party/trial/boot-* | head -1)
cp "$d/log.txt" "$out/$name.log"
grep -a '^\[watch\]' "$d/log.txt" | tail -n +2 | tr '\n' ' ' | cut -c1-200
echo
python3 -I scripts/ee_frames.py "$out/$name.log" "${FROM:-200}" "${TO:-260}" || true
python3 -I scripts/sample_threads.py "$out/$name.samples" "$runner" --top 25 --thread GameThread 2>/dev/null | c++filt | cut -c1-150 | head -45
