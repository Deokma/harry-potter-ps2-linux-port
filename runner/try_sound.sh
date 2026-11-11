#!/bin/sh
# A PC run with the sound mix captured, in WSL from the repo root: sh runner/try_sound.sh SECONDS NAME [VAR=VALUE...]
# Writes /mnt/d/ps2port-data/sound/NAME.raw (PS2PORT_SPU_PCM) and NAME.sizes (seconds and file size every 2 s), then prints
# scripts/pcm_stats.py's per-10-s mix rate and loudness. Pad: CROSS at 5 s, START at the title, CROSS in the main menu (New Game);
# override with PS2PORT_PAD / PS2PORT_PAD_SEQ. Other variables as for runner/try.sh.
set -eu
secs=$1
name=$2
shift 2
out=/mnt/d/ps2port-data/sound
mkdir -p "$out"
rm -f "$out/$name.raw" "$out/$name.sizes"
(
    start=$(date +%s.%N)
    while :; do
        sleep 2
        size=$(stat -c %s "$out/$name.raw" 2>/dev/null || echo 0)
        echo "$(echo "$(date +%s.%N) - $start" | bc) $size" >> "$out/$name.sizes"
    done
) &
sampler=$!
DRAW=${DRAW:-0/1} PS2PORT_GIF_LOG= sh runner/try.sh "$secs" "CROSS@5+0.3" PS2PORT_PAD_SEQ=9:START:60,2:CROSS:60 PS2PORT_PRESENT_EVERY=30 \
    PS2PORT_WATCH=0x38c194 PS2PORT_SPU_PCM="$out/$name.raw" "$@" | tail -1
kill $sampler 2>/dev/null || true
d=$(ls -dt private/third_party/trial/boot-* | head -1)
grep -a '^\[watch\]' "$d/log.txt" | tail -n +2 | tr '\n' ' ' | cut -c1-200
echo
python3 -I scripts/pcm_stats.py "$out/$name.raw" "$out/$name.sizes" 10
