#!/bin/sh
# A/B timing of gsplay's GIF packet phase (GS parsing and recording: stage B's work), alternating two settings so that load from
# other work on the machine falls on both. PC times, not device times.
# WSL, repo root:  sh scripts/gs_gsplay_ab.sh DUMP ROUNDS "A_VARS" "B_VARS"     e.g. "PS2PORT_GS_STRIP_RETRY=0" ""
# Prints the GIF packet ms of the last loop of each run, then the medians.
set -eu
dump=$1; rounds=$2; va=$3; vb=$4
g=private/third_party/trial/gsplay
a=""; b=""
i=0
while [ "$i" -lt "$rounds" ]; do
    ta=$(env $va LP_NUM_THREADS=0 "$g/gsplay-gles" "/mnt/d/ps2port-data/raw/$dump.gs" --loops 6 2>&1 | sed -n 's/.*GIF packets \([0-9.]*\).*/\1/p' | tail -1)
    tb=$(env $vb LP_NUM_THREADS=0 "$g/gsplay-gles" "/mnt/d/ps2port-data/raw/$dump.gs" --loops 6 2>&1 | sed -n 's/.*GIF packets \([0-9.]*\).*/\1/p' | tail -1)
    a="$a $ta"; b="$b $tb"
    i=$((i + 1))
done
med() { echo "$@" | tr ' ' '\n' | grep . | sort -n | awk '{v[NR]=$1} END {print v[int((NR+1)/2)]}'; }
echo "A ($va):$a  median $(med $a)"
echo "B ($vb):$b  median $(med $b)"
