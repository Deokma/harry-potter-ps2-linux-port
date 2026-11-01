#!/bin/sh
# Compile-check PS2Recomp output: every generated .cpp, in unity batches
# (like PS2Recomp's own runner build), in parallel. Objects only, no link.
#
# Run inside WSL (Linux paths). The generated code is derived from the
# game, so OUT and OBJ must be under private/.
#
# Usage:
#   scripts/compile_generated.sh OUT_DIR OBJ_DIR [CXX] [extra flags...]
# Examples:
#   scripts/compile_generated.sh private/third_party/trial/out-b \
#       private/third_party/trial/obj-x86 g++ -msse4.1
#   scripts/compile_generated.sh private/third_party/trial/out-b \
#       private/third_party/trial/obj-a64 aarch64-linux-gnu-g++ \
#       -march=armv8-a+fp+simd -DUSE_SSE2NEON -I<sse2neon dir>
#
# Env: RECOMP (PS2Recomp checkout, default private/third_party/PS2Recomp),
#      BATCH (files per unity TU, default 32), JOBS (default nproc),
#      OPT (default -O2).
set -eu

out=$(realpath "$1"); obj=$2; shift 2
cxx=${1:-g++}; [ $# -gt 0 ] && shift
recomp=$(realpath "${RECOMP:-private/third_party/PS2Recomp}")
batch=${BATCH:-32}; jobs=${JOBS:-$(nproc)}; opt=${OPT:--O2}

mkdir -p "$obj/unity"
obj=$(realpath "$obj")
rm -f "$obj"/unity/*.cpp "$obj"/*.o "$obj"/*.err

# Unity TUs: register_functions.cpp plus every function file.
ls "$out" | grep '\.cpp$' | sort | split -l "$batch" - "$obj/unity/list_"
for l in "$obj"/unity/list_*; do
    while read -r f; do echo "#include \"$out/$f\""; done < "$l" > "$l.cpp"
    rm "$l"
done
n_tu=$(ls "$obj"/unity/*.cpp | wc -l)
n_src=$(ls "$out" | grep -c '\.cpp$')
echo "$n_src files in $n_tu unity TUs, $jobs jobs, $cxx $opt $*"

start=$(date +%s)
# shellcheck disable=SC2016
ls "$obj"/unity/*.cpp | xargs -P "$jobs" -I{} sh -c '
    tu=$1; shift; obj=$1; shift; cxx=$1; shift; recomp=$1; shift; out=$1; shift
    b=$(basename "$tu" .cpp)
    "$cxx" -std=c++20 "$@" -w \
        -I"$recomp/ps2xRuntime/include" -I"$recomp/ps2xRuntime/src/lib/Kernel" \
        -I"$recomp/ps2xIOP/include" -I"$out" \
        -c "$tu" -o "$obj/$b.o" 2> "$obj/$b.err" || echo "FAIL $b"
    [ -s "$obj/$b.err" ] || rm -f "$obj/$b.err"
' _ {} "$obj" "$cxx" "$recomp" "$out" $opt "$@"
end=$(date +%s)

n_ok=$(ls "$obj"/*.o 2>/dev/null | wc -l)
size=$(du -cb "$obj"/*.o 2>/dev/null | tail -1 | cut -f1)
echo "compiled $n_ok/$n_tu TUs in $((end - start)) s wall, objects $((size / 1048576)) MB"
if ls "$obj"/*.err >/dev/null 2>&1; then
    echo "errors in: $(ls "$obj"/*.err | wc -l) TUs; first lines:"
    cat "$obj"/*.err | grep -m 20 'error'
    exit 1
fi
