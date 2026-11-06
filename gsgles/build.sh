#!/bin/sh
# Builds gsplay. Run in WSL from the repo root.
#   sh gsgles/build.sh cpu            x86, software GS: the PC reference
#   sh gsgles/build.sh gles           x86, GLES through Mesa (needs a display: WSLg)
#   sh gsgles/build.sh gles-aarch64   the device build
# Output: private/third_party/trial/gsplay/gsplay-TARGET. Objects are kept per target.
set -eu
src=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$src/.." && pwd)
tp=$repo/private/third_party
rt=$tp/PS2Recomp/ps2xRuntime
target=${1:-cpu}
out=$tp/trial/gsplay
obj=$out/obj-$target
mkdir -p "$obj"

case $target in
    cpu|gles) cxx=g++; cc=gcc; arch="-msse4.1"; link="-ldl -lpthread" ;;
    gles-aarch64)
        cxx=aarch64-linux-gnu-g++; cc=aarch64-linux-gnu-gcc
        arch="-march=armv8-a+fp+simd+crc -mtune=cortex-a53 -DUSE_SSE2NEON -I$tp/sse2neon -fno-pie"
        # the device's libstdc++ is older; SDL and GLES still come from the firmware through dlopen.
        # no-pie: fixed addresses, so that a profile's samples map to symbols
        link="-static-libstdc++ -static-libgcc -no-pie -ldl -lpthread" ;;
    *) echo "target: cpu, gles or gles-aarch64" >&2; exit 2 ;;
esac

flags="-std=c++20 ${GSPLAY_OPT:--O3} -ffp-contract=off -w $arch -I$src -I$rt/include -I$rt/src/lib -I$rt/src/lib/Kernel -I$tp/PS2Recomp/ps2xIOP/include -I$tp/raylib-5.5/src"
runtime="$rt/src/lib/gs/gs_frontend.cpp $rt/src/lib/gs/gs_cpu_backend.cpp $rt/src/lib/gs/ps2_gs_memory.cpp"
own="$src/gsplay.cpp"
cobjs=""

case $target in
    gles*)
        flags="$flags -DGSPLAY_GLES -I$repo/bench/src -I$obj/khr"
        mkdir -p "$obj/khr"
        for d in GLES2 GLES3 KHR; do [ -e "$obj/khr/$d" ] || cp -r "/usr/include/$d" "$obj/khr/$d"; done
        own="$own $src/gs_gles_backend.cpp $src/gsplay_gl.cpp"
        for c in gl_dyn sdl_dyn; do
            o=$obj/$c.o
            if [ ! -f "$o" ] || [ "$repo/bench/src/$c.c" -nt "$o" ] || [ "$repo/bench/src/gl_funcs.h" -nt "$o" ]; then
                $cc -std=c11 -D_GNU_SOURCE -O2 -w $arch -I"$obj/khr" -I"$repo/bench/src" -c "$repo/bench/src/$c.c" -o "$o"
            fi
            cobjs="$cobjs $o"
        done
        ;;
esac

objs=""
for f in $runtime $own; do
    o=$obj/$(basename "$f" .cpp).o
    # the runtime's objects only change when their source does; ours also on our headers
    case $f in
        "$src"/*) rebuild=$(find "$src" -name '*.h' -newer "$o" 2>/dev/null | head -1) ;;
        *) rebuild="" ;;
    esac
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$rebuild" ]; then
        echo "$cxx $flags -c $f -o $o" >> "$obj/jobs.txt.new"
    fi
    objs="$objs $o"
done
if [ -f "$obj/jobs.txt.new" ]; then
    xargs -P "$(nproc)" -I{} sh -c '{}' < "$obj/jobs.txt.new" 2>&1 | grep -E 'error|Error' | cut -c1-200 | head -30 || true
    rm -f "$obj/jobs.txt.new"
fi
for o in $objs; do [ -f "$o" ] || { echo "missing object $o" >&2; exit 1; }; done
$cxx $objs $cobjs $link -o "$out/gsplay-$target" 2>&1 | grep -E 'undefined|error' | sed 's/.*undefined/undefined/' | sort -u | cut -c1-120 | head -20 || true
[ -f "$out/gsplay-$target" ] && echo "built $out/gsplay-$target"
