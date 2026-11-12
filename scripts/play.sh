#!/bin/sh
# Play on the device with its own controls (from Git Bash on the PC):
#   sh scripts/play.sh [MINUTES [VAR=VALUE...]]        default 30 minutes; e.g. PS2PORT_PAD_SWAP_FACE=0 for the labels (runner/pad_sdl.cpp)
# Uploads the runner when it changed, stops EmulationStation, starts the game on the panel and starts EmulationStation again
# afterwards. Under device_guard.sh: clock pinned (default 1.416 GHz), stopped above 85 C (the Phase 7 soak reached that after about
# 13 minutes), clock and governor restored. Quit on the device: Menu + Start, or L1 + R1 + Select + Start, held for a second.
# Clock switches (heat experiments, docs/PHASE_9_HEAT.md): CPU_KHZ=1200000 (one of 480000 792000 1008000 1200000 1320000 1416000
# 1512000; the guard's ceiling for all four cores), CPU_GOV=schedutil (governor under that ceiling), GPU_MIN_HZ=0 (GPU governor
# left alone; default 648000000). Everything else goes to the runner.
here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$here")
minutes=${1:-30}
[ "$#" -ge 1 ] && shift
khz=${DEVICE_KHZ:-1416000}
n=$#
while [ "$n" -gt 0 ]; do
    a=$1
    shift
    n=$((n - 1))
    case $a in
        CPU_KHZ=*) khz=${a#CPU_KHZ=} ;;
        *) set -- "$@" "$a" ;;
    esac
done
case $khz in
    480000 | 792000 | 1008000 | 1200000 | 1320000 | 1416000 | 1512000) ;;
    *) echo "CPU_KHZ=$khz is not one of the device's clocks (480000 792000 1008000 1200000 1320000 1416000 1512000)"; exit 2 ;;
esac
DEVICE_KHZ=$khz DEVICE_TIMEOUT=$((minutes * 60)) exec sh "$here/device_detach.sh" -g "$repo/private/third_party/build-m2/aarch64/runner-aarch64" \
    "$repo/runner/device_play.sh" -- sh device_play.sh "$@"
