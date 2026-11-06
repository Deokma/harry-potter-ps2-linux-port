#!/bin/sh
# ps2port trial, milestone 1: recompile run B, build the whole-program
# reference (PS2Recomp's code as is, x86 counting build), run the gameplay
# frame to write the reference result, then build the quick-optimized
# variant (v3 + lean dispatch) for aarch64. Run in WSL from the repo root.
#   sh harness/frame_pipeline.sh REC_NAME
set -u
root=$(pwd)
s=$root/harness
h=$root/private/third_party/trial/harness
rec=$root/private/third_party/trial/rec/$1
step() { echo "[$(date +%H:%M:%S)] $*"; }

step "recompile run B"
sh scripts/ps2recomp_trial.sh b | grep -E 'Functions processed|fallback'

step "reference: generate v0 and build x86 count"
python3 -I "$s/make_harness.py" "$rec" private/third_party/trial/out-b "$h" v0 | tail -1
sh "$s/build.sh" x86 count frame-v0 2>&1 | grep -E " error|FAILED|undefined" | head -5
cp "$h/build/harness-x86-count-frame-v0" /tmp/ref

step "reference run"
rm -f "$rec.out"
(cd "$(dirname "$rec")" && HARNESS_VU0=1 HARNESS_WRITE_OUT=1 HARNESS_SKIP=0x2cd6a0 HARNESS_STOP_TIMER=2 HARNESS_CALLS="$rec.calls" \
    timeout 900 /tmp/ref "$(basename "$rec")" 1 > "$rec.ref.txt" 2>&1; echo "exit $?" >> "$rec.ref.txt")
grep -E "wrote|unexpected|exit|guest|hardware|syscalls|default|skipped" "$rec.ref.txt" | cut -c1-300
[ -f "$rec.out" ] || { step "no reference result; stopping"; exit 1; }

step "quick-optimized v4: generate v3 and build aarch64 time (lean dispatch)"
python3 -I "$s/make_harness.py" "$rec" private/third_party/trial/out-b "$h" v3 | tail -1
HARNESS_DEFS=-DHARNESS_LEAN_DISPATCH sh "$s/build.sh" aarch64 time frame-v4 2>&1 | grep -E " error|FAILED|undefined" | head -5
ls -la "$h/build/harness-aarch64-time-frame-v4"
step "done"
