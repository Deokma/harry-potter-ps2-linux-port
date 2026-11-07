#!/bin/sh
# Generates what the runner build compiles beyond PS2Recomp's output, into private/third_party/build-m2/gen/ (game-derived, never
# commit). Run in WSL from the repo root, after scripts/ps2recomp_trial.sh runtime b and the two fix scripts.
#   sh runner/gen.sh [FLOATS]    FLOATS=groups: eegen's float groups (needs the mode switches, not there yet)
set -eu
root=$(pwd)
tp=$root/private/third_party
t=$tp/trial
out=$tp/build-m2/gen
elf=$root/private/extract/SLUS-20826/files/SLUS_208.26
mkdir -p "$out"

# every function start in the map, and what must stay native: the stubs PS2Recomp's output calls
awk -F, 'NR > 1 { print $2 }' "$t/function_map.csv" > "$out/functions.txt"
{
    cat "$t/rec/native_stubs.txt"
    grep -l 'ps2_stubs::\|ps2_syscalls::' "$t"/out-b/FUN_*.cpp | sed -E 's/.*FUN_([0-9a-f]{8})_.*/0x\1/'
} | awk '{ print $1 }' | sort -u > "$out/native.txt"

# the movie stub replaces these in PS2Recomp's output (scripts/ps2recomp_movie_stub.py)
skip=0x2dd8e0,0x2dc868,0x2dcc98,0x2de1e8,0x2dc738
floats="--no-float-groups"
[ "${1:-}" = groups ] && floats=""
rm -rf "$out/eg.new"
python3 -I eegen/eegen.py "$elf" "$t/function_map.csv" "$out/eg.new" --functions "$out/functions.txt" \
    --ps2recomp-table "$t/out-b/register_functions.cpp" --native "$out/native.txt" --skip "$skip" $floats --runtime-count --checkpoints | tail -3 | cut -c1-240
# keeps unchanged files' times, so that the build recompiles only the batches that changed
rsync -rc --delete "$out/eg.new/" "$out/eg/"
rm -rf "$out/eg.new"

python3 -I eegen/vu0gen.py "$t/rec/frame-ntsc02.vu0code" "$out/vu0_programs.inc" \
    --entries 0x1,0x4,0x7,0xa,0xd,0x10,0x13,0x16,0x19,0x1c,0x1f,0x22,0x25,0x28,0x2b,0x2e,0x31,0x34 | tail -1
python3 -I eegen/vu1gen.py private/vu1/slot2/micro.bin "$out/vu1_gen.inc" --reached "$out/vu1_reached.inc" | tail -1
python3 -I - "$t/rec/frame-ntsc02.vu0code" private/vu1/slot2/micro.bin "$out/micro_images.inc" <<'EOF'
import sys
out = open(sys.argv[3], "w", newline="\n")
for name, path in (("g_vu0Image", sys.argv[1]), ("g_vu1Image", sys.argv[2])):
    data = open(path, "rb").read()
    out.write(f"static const unsigned char {name}[{len(data)}] = {{\n")
    for i in range(0, len(data), 32):
        out.write(",".join(str(b) for b in data[i:i + 32]) + ",\n")
    out.write("};\n")
EOF
echo "generated in $out"
