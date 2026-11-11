#!/bin/sh
# Phase 3 trial, step 1: recompile SLUS_208.26 with PS2Recomp three ways.
#   A: PS2Recomp's own analyzer and function discovery
#   B: A plus our function map (export_function_map.py)
#   C: B with the jalr change (a jalr is a call, not an unknown jump);
#      needs private/third_party/trial/ps2recomp-trial.patch applied
#      to the PS2Recomp checkout and ps2_recomp rebuilt
#
# Run inside WSL from the repo root. Everything it writes is derived from
# the game and stays under private/third_party/.
#
# Step 2 (runtime, boot): build PS2Recomp's runner from one run's output
# and boot the game with it.
#
# Usage: scripts/ps2recomp_trial.sh [setup|a|b|c|all|runtime [a|b|c]|boot [SECONDS [PAD]]]
set -eu

root=$(pwd)
tp=$root/private/third_party
trial=$tp/trial
elf=$root/private/extract/SLUS-20826/files/SLUS_208.26
commit=e5636981a1c9de368f6b71e819c3dc188dfedbf4
recomp=$tp/build-recomp/ps2xRecomp/ps2_recomp
analyzer=$tp/build-recomp/ps2xAnalyzer/ps2_analyzer

setup() {
    mkdir -p "$trial"
    if [ ! -d "$tp/PS2Recomp" ]; then
        git clone https://github.com/ran-j/PS2Recomp.git "$tp/PS2Recomp"
        git -C "$tp/PS2Recomp" checkout "$commit"
    fi
    cmake -S "$tp/PS2Recomp" -B "$tp/build-recomp" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF \
        -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
    cmake --build "$tp/build-recomp" -j "$(nproc)"
    "$analyzer" "$elf" "$trial/analyzer.toml" > "$trial/analyzer.log" 2>&1
    python3 scripts/export_function_map.py "$elf" "$trial/function_map.csv"
}

# config NAME MAP: analyzer config with our paths, output out-NAME/
config() {
    sed -e "s#^input = .*#input = \"$elf\"#" \
        -e "s#^ghidra_output = .*#ghidra_output = \"$2\"#" \
        -e "s#^output = .*#output = \"$trial/out-$1/\"#" \
        "$trial/analyzer.toml" > "$trial/config_$1.toml"
}

run() {
    name=$1; shift
    start=$(date +%s)
    env "$@" "$recomp" "$trial/config_$name.toml" > "$trial/recomp_$name.log" 2>&1
    echo "== run $name: $(($(date +%s) - start)) s, $(du -sh "$trial/out-$name" | cut -f1)"
    grep -E '^(Functions processed|Additional entrypoints|Unhandled|Warnings)' \
        "$trial/recomp_$name.log"
    # the report counts each fallback function twice; count unique ones
    echo "functions with per-instruction fallback: $(grep 'unresolved JR' \
        "$trial/recomp_$name.log" | sed -E 's/.*function=([^ ]*).*/\1/' \
        | sort -u | wc -l)"
}

# runtime NAME: build ps2EntryRunner (x86-64, software GS) from out-NAME/.
# FFmpeg off: movies become blank frames. The generated files are copied
# into the checkout's runner folder, as PS2Recomp's README says.
runtime() {
    rt=$tp/PS2Recomp/ps2xRuntime
    # PS2Recomp fetches raylib with full history (~500 MB); a shallow
    # clone of the same tag is enough
    [ -d "$tp/raylib-5.5" ] || git clone --depth 1 --branch 5.5 \
        https://github.com/raysan5/raylib.git "$tp/raylib-5.5"
    # vf0 is a constant on the VU, but PS2Recomp's macro code writes it (the
    # game's wait for a microprogram doubles vf0.w, PHASE_3 C6)
    grep -l 'ctx->vu0_vf\[0\] = PS2_VBLEND' "$trial/out-$1"/*.cpp 2>/dev/null | while read -r f; do
        sed -i -E 's/ctx->vu0_vf\[0\] = PS2_VBLEND\([^;]*\);//' "$f"
    done
    # sync by checksum: only changed files get new times, so ninja rebuilds
    # only the unity batches that hold them
    rsync -rc --delete --include='*.cpp' --exclude='*' \
        "$trial/out-$1/" "$rt/src/runner/"
    rsync -c "$trial/out-$1"/*.h "$rt/include/"
    # the runtime uses SSE4.1 intrinsics; its CMake only sets flags for
    # MSVC and ARM, so GCC on x86-64 needs -msse4.1
    cmake -S "$tp/PS2Recomp" -B "$tp/build-runtime" -G Ninja \
        -DCMAKE_CXX_FLAGS=-msse4.1 -DCMAKE_C_FLAGS=-msse4.1 \
        -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RECOMP=OFF \
        -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF \
        -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_FFMPEG=OFF \
        -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_SCCACHE=OFF \
        -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF \
        -DFETCHCONTENT_SOURCE_DIR_RAYLIB="$tp/raylib-5.5"
    start=$(date +%s)
    cmake --build "$tp/build-runtime" -j "$(nproc)"
    echo "runtime built in $(($(date +%s) - start)) s"
}

# boot SECONDS [PAD]: run the game in a WSLg window, with the trial hooks
# (ps2recomp-trial.patch): stop after SECONDS, scripted presses (PAD, see
# PS2PORT_PAD in ps2_pad.cpp), a PNG every 120 presents. Output goes to
# trial/boot-<time>/ (log + frames). Memory card files go to the ELF's
# folder (mc0/), as the runtime does by default.
boot() {
    dir=$trial/boot-$(date +%Y%m%d-%H%M%S)
    mkdir -p "$dir/frames"
    echo "secs=$1 pad=${2:-}" > "$dir/run.txt"
    start=$(date +%s)
    # PERF=1: sample the run with perf (cpu-clock) and write a symbol report.
    # perf can't write its data file onto /mnt/d, so it goes to /tmp first.
    perf=$(ls /usr/lib/linux-tools/*/bin/perf 2>/dev/null | head -1)
    # the GIF log (every packet, a file on /mnt/d) slows the GS side, and so the EE and the IOP: PS2PORT_GIF_LOG= (empty) turns
    # it off for timing and sound runs (runner/try_sound.sh does)
    prof=""
    if [ "${PERF:-0}" = 1 ]; then
        prof="$perf record -e cpu-clock -F 499 -o /tmp/ps2port-perf.data --"
    fi
    PS2PORT_HEADLESS=1 PS2PORT_RUN_SECONDS=$1 PS2PORT_PAD=${2:-} PS2PORT_FRAME_DIR=$dir/frames PS2PORT_GIF_LOG=${PS2PORT_GIF_LOG-$dir/gif.log} \
        timeout -k 10 $(($1 + 30)) $prof \
        "${PS2PORT_RUNNER:-$tp/build-runtime/ps2xRuntime/ps2EntryRunner}" "$elf" > "$dir/log.txt" 2>&1 \
        && rc=0 || rc=$?
    if [ -n "$prof" ]; then
        "$perf" report -i /tmp/ps2port-perf.data --stdio --sort symbol \
            > "$dir/perf_report.txt" 2>/dev/null || true
        mv /tmp/ps2port-perf.data "$dir/perf.data"
    fi
    echo "exit $rc after $(($(date +%s) - start)) s, $(ls "$dir/frames" | wc -l) frames, log $(wc -l < "$dir/log.txt") lines: $dir"
}

case ${1:-all} in
    setup) setup ;;
    runtime) runtime "${2:-b}" ;;
    boot) boot "${2:-120}" "${3:-}" ;;
    a) config a ""; run a ;;
    b) config b "$trial/function_map.csv"; run b ;;
    c) config c "$trial/function_map.csv"; run c PS2PORT_JALR_NO_FALLBACK=1 ;;
    all)
        setup
        config a ""; run a
        config b "$trial/function_map.csv"; run b
        ;;
    *) echo "usage: $0 [setup|a|b|c|all|runtime [a|b|c]|boot [SECONDS [PAD]]]" >&2; exit 2 ;;
esac
