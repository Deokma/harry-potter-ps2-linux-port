#!/bin/sh
# ps2port trial, step 3: build the replay harness from gen/ (make_harness.py).
#   build.sh x86|aarch64 time|count [NAME]
# Output: build/harness-TARGET-MODE[-NAME] (NAME: the make_harness variant). aarch64 builds are static, so they run
# on the device whatever its libstdc++. Flags follow PS2Recomp's own builds:
# -O2 plus -msse4.1 on x86-64, -march=armv8-a+fp+simd+crypto+crc and
# sse2neon on aarch64 (the RG40XX H's A53 has crypto and crc32).
set -eu
src=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$src/.." && pwd)
tp=$repo/private/third_party
h=$tp/trial/harness
rt=$tp/PS2Recomp/ps2xRuntime
target=${1:-x86}
mode=${2:-time}
name=${3:+-$3}

case $target in
    x86) cxx=g++; arch="-msse4.1"; extra="" ;;
    aarch64)
        cxx=aarch64-linux-gnu-g++
        arch="-march=armv8-a+fp+simd+crypto+crc -mtune=cortex-a53 -DUSE_SSE2NEON"
        extra="-I$tp/sse2neon -static" ;;
    *) echo "target: x86 or aarch64" >&2; exit 2 ;;
esac
case $mode in
    time) defs="" ;;
    count) defs="-DHARNESS_COUNTING" ;;
    *) echo "mode: time or count" >&2; exit 2 ;;
esac
defs="$defs ${HARNESS_DEFS:-}"  # e.g. -DHARNESS_LEAN_DISPATCH (experiment V4)

out=$h/build/$target-$mode$name
mkdir -p "$out"
bin=$h/build/harness-$target-$mode$name
# EG=1: also eegen's output from gen_eg/ (rebuilt unless EG_REUSE=1, which
# keeps its objects when only the harness changed; it overrides PS2Recomp's
# entries for the functions it translated)
if [ "${EG:-0}" = 1 ] && [ "${EG_REUSE:-0}" = 1 ] && ls "$out"/eg_*.o >/dev/null 2>&1; then
    eg_reuse=1
else
    eg_reuse=0
    rm -f "$out"/eg_*
fi
[ "${EG:-0}" = 1 ] && bin=$bin-eg
# REUSE=1 keeps the generated-code objects (only the harness sources change)
if [ "${REUSE:-0}" = 1 ] && ls "$out"/list_*.o >/dev/null 2>&1; then
    reuse=1
else
    reuse=0
    rm -f "$out"/list_* "$out"/*.o
fi
flags="-std=c++20 -O2 -ffp-contract=off $arch $defs -w -I$rt/include -I$rt/src/lib -I$rt/src/lib/Kernel -I$tp/PS2Recomp/ps2xIOP/include -I$tp/raylib-5.5/src -I$src -I$h -I$repo/eegen"
# generated code: one unity file per 32, the harness count header forced in
if [ $reuse = 0 ]; then
    ls "$h"/gen/FUN_*.cpp | split -l 32 - "$out/list_"
    for l in "$out"/list_*; do
        while read -r f; do echo "#include \"$f\""; done < "$l" > "$l.cpp"; rm "$l"
    done
    units=$(ls "$out"/list_*.cpp)
else
    # recompile only batches whose object is missing
    units=$(for u in "$out"/list_*.cpp; do [ -f "${u%.cpp}.o" ] || echo "$u"; done)
fi
if [ "${EG:-0}" = 1 ] && [ $eg_reuse = 1 ]; then
    units="$units $(for u in "$out"/eg_*.cpp; do [ -f "${u%.cpp}.o" ] || echo "$u"; done)"
elif [ "${EG:-0}" = 1 ]; then
    ls "${EG_DIR:-$h/gen_eg}"/EG_*.cpp | split -l 16 - "$out/eg_"
    for l in "$out"/eg_*; do
        while read -r f; do echo "#include \"$f\""; done < "$l" > "$l.cpp"; rm "$l"
    done
    units="$units $(ls "$out"/eg_*.cpp) ${EG_DIR:-$h/gen_eg}/eg_table.cpp"
fi
# compile with at most nproc jobs (a whole program is ~180 batches)
ls $units "$h/gen/harness_table.cpp" "$h/gen/harness_stubs.cpp" "$src/shim.cpp" "$src/main.cpp"     "$rt/src/lib/Kernel/Stubs/LibC.cpp" "$rt"/src/lib/vu/ps2_vu1_*.cpp |
    xargs -P "$(nproc)" -I{} sh -c '"$0" $1 -include "$2/harness_count.h" $3 -c "$4" -o "$5/$(basename "$4" .cpp).o" || echo "FAILED $4"'         "$cxx" "$flags" "$src" "$extra" {} "$out"
objs=$(ls "$out"/*.o)
$cxx $flags $extra $objs -o "$bin"
echo "built $bin"
