#!/bin/sh
# Builds the runner without CMake or raylib: PS2Recomp's runtime and generated code, eegen's functions over them, translated VU0 and VU1.
# Run in WSL from the repo root, after runner/gen.sh.
#   sh runner/build.sh x86|aarch64
# Output: private/third_party/build-m2/TARGET/runner. Objects are kept and rebuilt when their source changes; the generated code's
# batches rebuild only when their object is missing. A full build takes a long time: run it in the background.
set -eu
src=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$src/.." && pwd)
tp=$repo/private/third_party
rt=$tp/PS2Recomp/ps2xRuntime
iop=$tp/PS2Recomp/ps2xIOP
gen=$tp/build-m2/gen
target=${1:-x86}
out=$tp/build-m2/$target
mkdir -p "$out"
case $target in
    x86) cxx=g++; arch="-msse4.1"; link="-ldl -lpthread" ;;
    aarch64)
        cxx=aarch64-linux-gnu-g++
        arch="-march=armv8-a+fp+simd+crypto+crc -mtune=cortex-a53 -DUSE_SSE2NEON -I$tp/sse2neon -include $src/host/glibc_compat.h"
        # the device's libstdc++ is older, as for gsplay
        link="-static-libstdc++ -static-libgcc -ldl -lpthread" ;;
    *) echo "target: x86 or aarch64" >&2; exit 2 ;;
esac
common="-std=c++20 -ffp-contract=off -fno-math-errno -w $arch -DPS2X_HAS_FFMPEG=0 -DPS2X_ENABLE_IOP_RPC_TRACE=1 -I$src/host -I$rt/include -I$rt/src/lib -I$rt/src/lib/Kernel -I$iop/include -I$iop/src -I$src/../eegen -I$src/../vu1 -I$gen"

rm -f "$out/jobs.txt"
# job FLAGS SOURCE OBJECT: compile if the object is missing or older
job() {
    [ -f "$3" ] && [ ! "$2" -nt "$3" ] && [ -z "${4:-}" ] && return 0
    echo "$cxx $common $1 -c $2 -o $3" >> "$out/jobs.txt"
}

mkdir -p "$out/lib" "$out/iop" "$out/fun" "$out/eg" "$out/own"
for f in game_overrides gs/ps2_gif_arbiter ps2_audio ps2_audio_vag gs/ps2_gs_memory gs/gs_frontend gs/gs_cpu_backend ps2_iop_host ps2_memory ps2_pad ps2_rom_device ps2_vfs ps2_runtime ps2_vif1_interpreter vu/ps2_vu1_core vu/ps2_vu1_upper vu/ps2_vu1_lower games_database; do
    job "-O2" "$rt/src/lib/$f.cpp" "$out/lib/$(basename "$f").o"
done
for f in $(cd "$rt/src/lib/Kernel" && find . -name '*.cpp' | sed 's|^\./||'); do
    job "-O2" "$rt/src/lib/Kernel/$f" "$out/lib/kernel_$(echo "$f" | tr '/' '_' | sed 's/.cpp$//').o"
done
for f in ps2_path iop_module_manager iop_subsystem emulator/iop_emulator emulator/core/iop_cpu emulator/core/iop_kernel emulator/core/iop_memory emulator/services/iop_module_loader emulator/services/iop_rpc emulator/imports/iop_cdvd emulator/imports/iop_heaplib emulator/imports/iop_imports emulator/imports/iop_intrman emulator/imports/iop_ioman emulator/imports/iop_loadcore emulator/imports/iop_stdio emulator/imports/iop_sysclib emulator/imports/iop_sysmem emulator/imports/iop_timrman emulator/imports/iop_vblank modules/dbcman modules/libsd modules/mcserv; do
    job "-O2" "$iop/src/$f.cpp" "$out/iop/$(basename "$f").o"
done
job "-O2" "$rt/src/main.cpp" "$out/own/main.o"
job "-O1" "$rt/src/runner/register_functions.cpp" "$out/own/register_functions.o"
job "-O3" "$src/native_hooks.cpp" "$out/own/native_hooks.o" "$(find "$src" "$src/../vu1" "$src/../eegen" "$gen" -maxdepth 1 \( -name '*.h' -o -name '*.inc' -o -name 'vu1_native.cpp' \) -newer "$out/own/native_hooks.o" 2>/dev/null | head -1)"

# PS2Recomp's generated code: it takes re-entries and whatever eegen refused, so -O1 is enough
if ! ls "$out"/fun/list_*.cpp >/dev/null 2>&1; then
    ls "$rt"/src/runner/FUN_*.cpp | split -l 32 - "$out/fun/list_"
    for l in "$out"/fun/list_*; do
        while read -r f; do echo "#include \"$f\""; done < "$l" > "$l.cpp"
        rm "$l"
    done
fi
for u in "$out"/fun/list_*.cpp; do job "-O1" "$u" "${u%.cpp}.o"; done
# eegen's
if ! ls "$out"/eg/list_*.cpp >/dev/null 2>&1; then
    ls "$gen"/eg/EG_*.cpp | split -l 16 - "$out/eg/list_"
    for l in "$out"/eg/list_*; do
        while read -r f; do echo "#include \"$f\""; done < "$l" > "$l.cpp"
        rm "$l"
    done
fi
for u in "$out"/eg/list_*.cpp; do job "-O2" "$u" "${u%.cpp}.o"; done
job "-O1" "$gen/eg/eg_table.cpp" "$out/eg/eg_table.o"

if [ -f "$out/jobs.txt" ]; then
    echo "compiling $(wc -l < "$out/jobs.txt") units"
    xargs -P "${JOBS:-$(nproc)}" -I{} sh -c '{} || echo FAILED: {}' < "$out/jobs.txt" 2>&1 | grep -E 'error|FAILED' | cut -c1-200 | head -30 || true
fi
objs=$(find "$out/lib" "$out/iop" "$out/own" "$out/fun" "$out/eg" -name '*.o')
$cxx $objs $link -o "$out/runner" 2>&1 | grep -E 'undefined|multiple definition|error' | sed 's/.*undefined reference/undefined reference/; s/.*multiple definition/multiple definition/' | sort -u | cut -c1-140 | head -30 || true
[ -f "$out/runner" ] && echo "built $out/runner" && ls -la "$out/runner" | cut -c24-60
