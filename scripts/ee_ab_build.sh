#!/bin/sh
# A/B runner binaries for the EE worker (WSL, from a sandbox worktree's root; docs/PHASE_9_EE.md):
#   sh scripts/ee_ab_build.sh OUTDIR
# Builds OUTDIR/runner-base (eegen as before: loop-head resumes only, calls through dispatchGuestBranch) and OUTDIR/runner-new
# (runner/gen.sh's defaults), with the same runtime. The current gen/eg and x86/eg trees are set aside and put back for the second
# build, so only the first rebuilds eegen's batches (about 20 minutes each way on the shared PC with JOBS=6).
set -eu
out=$1
mkdir -p "$out"
tp=private/third_party/build-m2
rm -rf "$tp/gen/eg.keep" "$tp/x86/eg.keep"
cp -a "$tp/gen/eg" "$tp/gen/eg.keep"
cp -a "$tp/x86/eg" "$tp/x86/eg.keep"
PS2PORT_EG_RETURN_RESUMES=0 PS2PORT_EG_FAST_DISPATCH=0 sh runner/gen.sh | sed -n 2p | cut -c1-120
JOBS=${JOBS:-6} sh runner/build.sh x86 2>&1 | grep -a "error\|FAILED\|undefined\|built" | head -5 | cut -c1-200
cp "$tp/x86/runner" "$out/runner-base"
rm -rf "$tp/gen/eg" "$tp/x86/eg"
mv "$tp/gen/eg.keep" "$tp/gen/eg"
mv "$tp/x86/eg.keep" "$tp/x86/eg"
JOBS=${JOBS:-6} sh runner/build.sh x86 2>&1 | grep -a "error\|FAILED\|undefined\|built" | head -5 | cut -c1-200
cp "$tp/x86/runner" "$out/runner-new"
echo "AB_DONE"
