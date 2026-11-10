#!/bin/sh
# Runs on the device, under device_guard.sh: one GS dump replayed at each GPU clock given (Hz), no swaps, glFinish per vsync.
#   [GSPLAY_ENV="VAR=VALUE ..."] device_gpuclock.sh LOOPS DUMP HZ...
# Prints gsplay's "last loop" line per clock (ms per vsync: GIF parse, GL commands, GPU). The governor's minimum is put back after.
loops=$1; dump=$2; shift 2
dev=/sys/class/devfreq/gpu
old=$(cat $dev/min_freq)
trap 'echo "$old" > $dev/min_freq' EXIT
for hz in "$@"; do
    echo "$hz" > $dev/min_freq
    sleep 0.5
    env $GSPLAY_ENV GSPLAY_FINISH=1 GSPLAY_NOSWAP=1 ./gsplay-gles-aarch64 "$dump.gs" --warm --loops "$loops" > "$dump.$hz.log" 2>&1
    echo "min $hz, clock $(cat $dev/cur_freq): $(grep '^last loop' "$dump.$hz.log" | cut -c1-150)"
done
# OUT_TAG=name: also one captured frame, cold, to DUMP.name.ppm (to compare two settings' pictures)
[ -n "${OUT_TAG:-}" ] && env $GSPLAY_ENV ./gsplay-gles-aarch64 "$dump.gs" --out "$dump.$OUT_TAG.ppm" > "$dump.$OUT_TAG.cap" 2>&1
exit 0
