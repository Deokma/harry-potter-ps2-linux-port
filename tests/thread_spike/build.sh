#!/bin/sh
# Thread spike: assemble the toy, translate it with eegen, build for x86 and the
# device. Run in WSL from the repo root. Output in tests/thread_spike/build/ (toy
# code only, no game data, but generated, so not committed).
set -eu
dir=tests/thread_spike
out=$dir/build
tp=private/third_party
rt=$tp/PS2Recomp/ps2xRuntime
mkdir -p "$out"
python3 -I $dir/toy_program.py "$out"
python3 -I eegen/eegen.py "$out/toy.elf" "$out/toy_map.csv" "$out/gen" --functions "$out/toy_funcs.txt" | tail -1
inc="-I$rt/include -I$rt/src/lib -I$rt/src/lib/Kernel -I$tp/PS2Recomp/ps2xIOP/include -I$tp/raylib-5.5/src -Ieegen -I$tp/trial/harness -I$out"
common="-std=c++20 -O2 -ffp-contract=off -w -include harness_count.h $inc"
units="$dir/toy_runtime.cpp $out/gen/EG_*.cpp $out/gen/eg_table.cpp"
# shellcheck disable=SC2086
g++ $common -msse4.1 $units -o "$out/toy-x86"
# shellcheck disable=SC2086
aarch64-linux-gnu-g++ $common -march=armv8-a+fp+simd+crypto+crc -mtune=cortex-a53 -DUSE_SSE2NEON -I$tp/sse2neon -static $units -o "$out/toy-aarch64"
ls -la "$out/toy-x86" "$out/toy-aarch64" | awk '{print $5, $NF}'
