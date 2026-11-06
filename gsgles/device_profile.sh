#!/bin/sh
# Runs on the device, under device_guard.sh: CPU cost and a profile of each dump, for each binary given.
#   device_profile.sh LOOPS "BIN..." DUMP...
# Writes DUMP-BIN.samples beside the dumps. GPU clock raised as in device_table.sh; no swaps, so the flip isn't measured.
loops=$1; bins=$2; shift 2
dev=/sys/class/devfreq/gpu
old=$(cat $dev/min_freq)
trap 'echo "$old" > $dev/min_freq' EXIT
echo 648000000 > $dev/min_freq
sleep 0.3
printf '%-12s %-10s %8s %8s %8s %8s\n' binary dump vsyncs gif_ms cmd_ms gpu_ms
for b in $bins; do
    chmod +x "$b"
    for d in "$@"; do
        GSPLAY_SAMPLE="$d-$b.samples" GSPLAY_FINISH=1 GSPLAY_NOSWAP=1 ./"$b" "$d.gs" --warm --loops "$loops" > "$d.log" 2>&1
        rc=$?
        v=$(grep -m1 '^dump:' "$d.log" | sed 's/.* \([0-9]*\) vsyncs.*/\1/')
        set -- "$@"
        line=$(grep '^last loop' "$d.log" | sed 's/[^0-9. ]//g')
        echo "$line" | awk -v b="$b" -v d="$d" -v v="$v" -v rc="$rc" '{ printf "%-12s %-10s %8s %8.1f %8.1f %8.1f%s\n", b, d, v, $2, $3, $4, (rc ? " rc=" rc : "") }'
    done
done
echo "$old" > $dev/min_freq
trap - EXIT
