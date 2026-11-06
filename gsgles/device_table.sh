#!/bin/sh
# Runs on the device, under device_guard.sh: cost per dump, a captured frame per dump, then a sustained run.
#   device_table.sh LOOPS SUSTAIN_DUMP DUMP...
# The GPU's minimum clock is raised to its maximum for the run and restored after. simple_ondemand otherwise
# stays at 240 MHz on this driver, even with the GPU busy. Timing runs don't swap: on this fbdev driver a swap
# paces everything to the display whatever the swap interval says, so glFinish would measure the flip.
loops=$1; sustain=$2; shift 2
dev=/sys/class/devfreq/gpu
old=$(cat $dev/min_freq)
trap 'echo "$old" > $dev/min_freq' EXIT
echo 648000000 > $dev/min_freq
sleep 0.3
temp() { awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp; }
echo "gpu now $(cat $dev/cur_freq), temp $(temp) C"
printf '%-10s %8s %8s %8s %8s %8s %6s\n' dump vsyncs gif_ms cmd_ms gpu_ms vram_ms temp
for d in "$@"; do
    GSPLAY_FINISH=1 GSPLAY_NOSWAP=1 ./gsplay-gles-aarch64 "$d.gs" --warm --loops "$loops" > "$d.log" 2>&1
    rc=$?
    v=$(grep -m1 '^dump:' "$d.log" | sed 's/.* \([0-9]*\) vsyncs.*/\1/')
    set -- $(grep '^last loop' "$d.log" | sed 's/[^0-9. ]//g')
    vram=$(grep '^gles: ms' "$d.log" | sed 's/.*vram \([0-9.]*\).*/\1/')
    awk -v d="$d" -v v="$v" -v g="$2" -v c="$3" -v f="$4" -v vr="$vram" -v l="$loops" -v t="$(temp)" -v rc="$rc" \
        'BEGIN { printf "%-10s %8s %8.1f %8.1f %8.1f %8.1f %6s%s\n", d, v, g, c, f, vr/l, t, (rc ? " rc=" rc : "") }'
    # a frame to compare, cold, through the ordinary path
    ./gsplay-gles-aarch64 "$d.gs" --out "$d.ppm" > "$d.cap" 2>&1
done
echo "== sustained, with swaps, as the display paces it: $sustain"
start=$(date +%s)
GSPLAY_FINISH=1 ./gsplay-gles-aarch64 "$sustain.gs" --warm --loops 12 > sustain.log 2>&1
echo "rc=$? $(($(date +%s) - start)) s, temp $(temp) C"
grep -E '^loops|^last loop' sustain.log | cut -c1-130
echo "$old" > $dev/min_freq
trap - EXIT
echo "gpu min_freq back to $(cat $dev/min_freq)"
