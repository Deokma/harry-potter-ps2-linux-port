#!/bin/sh
# Play on the device with its own controls (from Git Bash on the PC):
#   sh scripts/play.sh [MINUTES [VAR=VALUE...]]        default 30 minutes; e.g. PS2PORT_PAD_SWAP_FACE=0 for the labels (runner/pad_sdl.cpp)
# Uploads the runner when it changed, stops EmulationStation, starts the game on the panel and starts EmulationStation again
# afterwards. Under device_guard.sh: clock pinned to 1.416 GHz, stopped above 85 C (the Phase 7 soak reached that after about
# 13 minutes). Quit on the device: Menu + Start, or L1 + R1 + Select + Start, held for a second.
here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$here")
minutes=${1:-30}
[ "$#" -ge 1 ] && shift
DEVICE_TIMEOUT=$((minutes * 60)) exec sh "$here/device_detach.sh" -g "$repo/private/third_party/build-m2/aarch64/runner-aarch64" \
    "$repo/runner/device_play.sh" -- sh device_play.sh "$@"
