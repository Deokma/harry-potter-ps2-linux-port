#!/bin/sh
# Runs on the device, under device_guard.sh: one headless boot of the runner.
#   device_boot.sh SECONDS PAD [DRAW] [VAR=VALUE...]
# DRAW is PS2PORT_GS_DRAW_WINDOW, ON/PERIOD seconds: the software GS is only there for reference pictures until M5, and it is slow here.
# Game files in /userdata/ps2bench/game (the user's own disc, extracted). Results in boot-TIME/: log.txt, gif.log, frames/. Prints a summary.
secs=$1
pad=$2
draw=${3:-2/20}
[ "$#" -ge 3 ] && shift 3 || shift "$#"
cd "$(dirname "$0")" || exit 1
d=boot-$(date +%H%M%S)
mkdir -p "$d/frames"
chmod +x runner-aarch64
echo "$d" > last-boot.txt
start=$(date +%s)
env PS2PORT_NATIVE_STATS=1 PS2PORT_STATS=1 PS2PORT_HEADLESS=1 PS2PORT_RUN_SECONDS="$secs" PS2PORT_PAD="$pad" PS2PORT_FRAME_DIR="$d/frames" \
    PS2PORT_GIF_LOG="$d/gif.log" PS2PORT_GS_DRAW_WINDOW="$draw" "$@" ./runner-aarch64 /userdata/ps2bench/game/SLUS_208.26 > "$d/log.txt" 2>&1
rc=$?
echo "exit $rc after $(($(date +%s) - start)) s, $(ls "$d/frames" | wc -l) frames, $(wc -l < "$d/log.txt") log lines: $d"
grep -a '\[native\]' "$d/log.txt" | cut -c1-110
grep -a '^\[ps2port\] present' "$d/log.txt" | tail -1 | cut -c1-90
grep -a '^\[game\]' "$d/log.txt" | tail -1 | sed 's/\[game\] //' | cut -c1-120
grep -a '^\[stats\]' "$d/log.txt" | tail -1 | sed 's/\[stats\] //' | cut -c1-100
tail -3 "$d/log.txt" | cut -c1-130
