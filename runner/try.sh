#!/bin/sh
# One boot of the x86 runner, in WSL from the repo root: sh runner/try.sh SECONDS [PAD] [VAR=VALUE...]
# Prints what it reached: which code ran, how far presents and packets got, and how far the loading bar got by the last frame's hash.
set -eu
secs=$1
pad=${2:-}
[ "$#" -ge 2 ] && shift 2 || shift 1
env PS2PORT_NATIVE_STATS=1 PS2PORT_GS_DRAW_WINDOW=${DRAW:-3/9} PS2PORT_RUNNER="$PWD/private/third_party/build-m2/x86/runner" PS2PORT_STATS=1 "$@" sh scripts/ps2recomp_trial.sh boot "$secs" "$pad" 2>&1 | tail -1 | cut -c1-100
d=$(ls -dt private/third_party/trial/boot-* | head -1)
grep -a '\[native\]' "$d/log.txt" | cut -c1-100
grep -a '^\[game\]' "$d/log.txt" | tail -1 | sed 's/\[game\] //' | cut -c1-120
grep -a '^\[stats\]' "$d/log.txt" | tail -1 | sed 's/\[stats\] //' | cut -c1-100
grep -a 'missing-target\|guest-branch' "$d/log.txt" | cut -c1-110 | sort -u | head -5
md5sum "$d"/frames/* | awk '{print $1}' | uniq -c | awk '{printf "%s ", $1}' | cut -c1-200
echo
echo "$d"
