#!/bin/sh
# CPU samples (1 per ms of CPU time) of gsplay's GS parsing and recording functions over LOOPS replays of a dump, for two
# settings in turn: a work measure that other load on the machine disturbs less than wall time. PC numbers.
# WSL, repo root:  sh scripts/gs_gsplay_prof.sh DUMP LOOPS "A_VARS" "B_VARS"
set -eu
dump=$1; loops=$2
g=private/third_party/trial/gsplay
rx='writeRegisterPacked|vertexKick|appendPrimitive|SubmitSameState|SubmitPackedStrip|processGIFPacket|::Submit\(|beginRun|buildDrawBatch|ps2portStripTaken|writeRegisterUnlocked|sameState|updatePreferredDisplaySource'
for vars in "$3" "$4"; do
    env $vars LP_NUM_THREADS=0 GSPLAY_SAMPLE=$g/gsw/prof.samples "$g/gsplay-gles" "/mnt/d/ps2port-data/raw/$dump.gs" --loops "$loops" > /dev/null 2>&1
    printf '%s: ' "${vars:-default}"
    python3 -I scripts/gs_sample_syms.py $g/gsw/prof.samples $g/gsplay-gles --top 0 --match "$rx" | tail -1
done
