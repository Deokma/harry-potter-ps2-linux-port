#!/bin/sh
# The EE worker's runtime change as a patch (WSL, from a worktree's root made by scripts/worker_sandbox.sh):
#   sh scripts/ee_runtime_patch.sh [OUT]      default OUT: /mnt/d/ps2port-data/workers/ee-runtime.patch
# Diff of the sandbox copy of PS2Recomp's ps2xRuntime against its sandbox-base tag; prints the changed files.
set -eu
out=${1:-/mnt/d/ps2port-data/workers/ee-runtime.patch}
mkdir -p "$(dirname "$out")"
repo=private/third_party/PS2Recomp
git -C "$repo" diff --ignore-cr-at-eol sandbox-base -- ps2xRuntime > "$out"
git -C "$repo" diff --ignore-cr-at-eol --stat sandbox-base -- ps2xRuntime | tail -5
echo "wrote $out ($(wc -c < "$out") bytes)"
