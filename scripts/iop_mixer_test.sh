#!/bin/sh
# Mixer unit test, in WSL from the repo root: sh scripts/iop_mixer_test.sh [BASE_REV]
# Builds scripts/iop_mixer_test.cpp against the sandbox PS2Recomp's iop_memory.cpp as it is now and as it was at BASE_REV
# (default sandbox-base), runs both and compares the output byte for byte.
set -eu
rev=${1:-sandbox-base}
iop=private/third_party/PS2Recomp/ps2xIOP
tmp=$(mktemp -d)
git -C private/third_party/PS2Recomp show "$rev:ps2xIOP/src/emulator/core/iop_memory.cpp" > "$tmp/old_iop_memory.cpp"
flags="-std=c++20 -O2 -ffp-contract=off -fno-math-errno -w -I$iop/src -I$iop/include"
# the old file includes "iop_memory.h" from its own directory: give it the current header (the class layout is unchanged)
g++ $flags -I"$iop/src/emulator/core" scripts/iop_mixer_test.cpp "$tmp/old_iop_memory.cpp" -o "$tmp/old"
g++ $flags scripts/iop_mixer_test.cpp "$iop/src/emulator/core/iop_memory.cpp" -o "$tmp/new"
"$tmp/old" "$tmp/old.bin" >/dev/null 2>"$tmp/old.err"
"$tmp/new" "$tmp/new.bin" >/dev/null 2>"$tmp/new.err"
ls -l "$tmp/old.bin" "$tmp/new.bin" | awk '{print $5, $9}'
if cmp -s "$tmp/old.bin" "$tmp/new.bin"; then echo "IDENTICAL"; else echo "DIFFERENT"; cmp "$tmp/old.bin" "$tmp/new.bin" | head -1; fi
md5sum "$tmp/old.bin" "$tmp/new.bin" | awk '{print $1}'
rm -rf "$tmp"
