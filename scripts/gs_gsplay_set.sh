#!/bin/sh
# Runs gsplay over a set of GS dumps and keeps each frame and the counters, for before/after comparisons (PHASE_9_GS).
# WSL, repo root:  sh scripts/gs_gsplay_set.sh TAG cpu|gles [DUMP...]      (VAR=VALUE in the environment passes through)
# Output: private/third_party/trial/gsplay/gsw/TAG/{NAME.ppm,NAME.txt}. Compare two tags with scripts/gs_gsplay_cmp.sh.
set -eu
tag=$1; target=$2; shift 2
g=private/third_party/trial/gsplay
out=$g/gsw/$tag
mkdir -p "$out"
dumps=${*:-"N02_0 N02_1 N02_2 N02_3 I01_1 F01_1 N01_1 N03_1"}
for d in $dumps; do
    "$g/gsplay-$target" "/mnt/d/ps2port-data/raw/$d.gs" --out "$out/$d.ppm" --loops "${LOOPS:-1}" > "$out/$d.txt" 2>&1 || echo "$d: exit $?"
    printf '%s: ' "$d"; grep -E "^gles: [0-9]+ presents|merged" "$out/$d.txt" | tail -2 | tr '\n' ' '; echo
done
