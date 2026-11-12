#!/bin/sh
# Compares the frames of two scripts/gs_gsplay_set.sh tags, picture by picture, in numbers.
# WSL, repo root:  sh scripts/gs_gsplay_cmp.sh TAG_A TAG_B
set -u
g=private/third_party/trial/gsplay/gsw
for a in "$g/$1"/*.ppm; do
    n=$(basename "$a" .ppm)
    printf '%s: ' "$n"
    python3 -I scripts/gs_ppm_diff.py "$a" "$g/$2/$n.ppm"
done
