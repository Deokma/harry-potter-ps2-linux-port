#!/bin/sh
# ps2port milestone 1: the whole program through eegen (every function it
# translates; the rest stays PS2Recomp's), checked on x86 against the
# PS2Recomp reference and built for aarch64. Run in WSL from the repo root:
#   [EG_ARGS="--borrow private/third_party/trial/out-b"] sh harness/eg_pipeline.sh REC_NAME FUNCTION_LIST TAG
# Reuses the PS2Recomp objects of the frame-v0 (x86 count) and frame-v4
# (aarch64 time, lean dispatch) builds; binaries end in -eg-TAG.
set -u
root=$(pwd)
s=$root/harness
h=$root/private/third_party/trial/harness
t=$root/private/third_party/trial
rec=$1
list=$2
tag=$3
step() { echo "[$(date +%H:%M:%S)] $*"; }

step "eegen ($list)"
python3 -I eegen/eegen.py private/extract/SLUS-20826/files/SLUS_208.26 "$t/function_map.csv" "$h/gen_eg_$tag" \
    --functions "$list" --ps2recomp-table "$t/out-b/register_functions.cpp" \
    --native "$t/rec/native_stubs.txt" --skip 0x2cd6a0 ${EG_ARGS:-} | tail -3

step "x86 count build"
REUSE=1 EG=1 EG_DIR="$h/gen_eg_$tag" sh "$s/build.sh" x86 count frame-v0 2>&1 | grep -E " error|FAILED|undefined" | head -5
cp "$h/build/harness-x86-count-frame-v0-eg" "$h/build/harness-x86-count-frame-v0-eg-$tag"
step "x86 check"
(cd "$t/rec" && HARNESS_VU0=3 HARNESS_SKIP=0x2cd6a0 HARNESS_STOP_TIMER=2 timeout 900 "$h/build/harness-x86-count-frame-v0-eg-$tag" "$rec" 1 2>&1 |
    grep -E "eegen functions|check vs|guest instructions per|unexpected")

step "aarch64 time build"
REUSE=1 EG=1 EG_DIR="$h/gen_eg_$tag" HARNESS_DEFS=-DHARNESS_LEAN_DISPATCH sh "$s/build.sh" aarch64 time frame-v4 2>&1 |
    grep -E " error|FAILED|undefined" | head -5
cp "$h/build/harness-aarch64-time-frame-v4-eg" "$h/build/harness-aarch64-time-frame-v4-eg-$tag"
ls -la "$h/build/harness-aarch64-time-frame-v4-eg-$tag" | cut -c25-
step "done"
