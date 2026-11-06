#!/bin/sh
# Builds vu1play. Run in WSL from the repo root.
#   sh vu1/build.sh x86|aarch64 [stub]
# stub: no translated code, for the interpreter alone. Otherwise private/vu1/vu1_gen.inc must exist (eegen/vu1gen.py).
# Output: private/vu1/vu1play-TARGET. Objects are kept per target.
set -eu
src=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$src/.." && pwd)
tp=$repo/private/third_party
rt=$tp/PS2Recomp/ps2xRuntime
target=${1:-x86}
out=$repo/private/vu1
obj=$out/obj-$target
mkdir -p "$obj"
case $target in
    x86) cxx=g++; arch="-msse4.1"; link="-lpthread" ;;
    aarch64)
        cxx=aarch64-linux-gnu-g++
        arch="-march=armv8-a+fp+simd+crc -mtune=cortex-a53 -DUSE_SSE2NEON -I$tp/sse2neon"
        link="-static -lpthread" ;;
    *) echo "target: x86 or aarch64" >&2; exit 2 ;;
esac
flags="-std=c++20 -O3 -ffp-contract=off -fno-math-errno -w $arch -I$src -I$out -I$repo/eegen -I$rt/include -I$rt/src/lib -I$rt/src/lib/Kernel -I$tp/PS2Recomp/ps2xIOP/include -I$tp/raylib-5.5/src -DPS2X_NO_RAYLIB"
runtime="$rt/src/lib/gs/gs_frontend.cpp $rt/src/lib/gs/gs_cpu_backend.cpp $rt/src/lib/gs/ps2_gs_memory.cpp $rt/src/lib/gs/ps2_gif_arbiter.cpp $rt/src/lib/ps2_memory.cpp $rt/src/lib/ps2_vif1_interpreter.cpp $rt/src/lib/vu/ps2_vu1_core.cpp $rt/src/lib/vu/ps2_vu1_lower.cpp $rt/src/lib/vu/ps2_vu1_upper.cpp"
own="$src/vu1play.cpp"
if [ "${2:-}" = stub ]; then own="$own $src/vu1_stub.cpp"; else own="$own $src/vu1_native.cpp"; fi
objs=""
rm -f "$obj/jobs.txt"
for f in $runtime $own; do
    o=$obj/$(basename "$f" .cpp).o
    case $f in
        "$src"/*) stale=$(find "$src" "$out" -maxdepth 1 \( -name '*.h' -o -name '*.inc' \) -newer "$o" 2>/dev/null | head -1) ;;
        *) stale="" ;;
    esac
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$stale" ]; then
        echo "$cxx $flags -c $f -o $o" >> "$obj/jobs.txt"
    fi
    objs="$objs $o"
done
if [ -f "$obj/jobs.txt" ]; then
    xargs -P "$(nproc)" -I{} sh -c '{}' < "$obj/jobs.txt" 2>&1 | grep -E 'error|Error' | cut -c1-210 | head -24 || true
fi
for o in $objs; do [ -f "$o" ] || { echo "missing object $o" >&2; exit 1; }; done
$cxx $objs $link -o "$out/vu1play-$target" 2>&1 | grep -E 'undefined reference|multiple definition' | sed 's/.*undefined reference/undefined reference/; s/.*multiple definition/multiple definition/' | sort -u | cut -c1-120 | head -24 || true
[ -f "$out/vu1play-$target" ] && echo "built $out/vu1play-$target"
