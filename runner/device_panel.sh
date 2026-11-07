#!/bin/sh
# Runs on the device, under device_guard.sh: the game on the panel, GLES backend, pad scripted, no audio.
#   device_panel.sh SECONDS PAD PANEL_AT [VAR=VALUE...]
# PANEL_AT is when (seconds, space-separated) to copy /dev/fb0, which is what the panel really shows, to panel-T.raw. Screenshots every 120 vsyncs.
# Results in panel-TIME/: log.txt, frames/, panel-*.raw, fb.txt (the framebuffer's geometry), and the profile when PS2PORT_SAMPLE=samples.bin
# is given (scripts/sample_threads.py). Prints the log's [m5] lines.
secs=$1
pad=$2
at=$3
[ "$#" -ge 3 ] && shift 3 || shift "$#"
cd "$(dirname "$0")" || exit 1
d=panel-$(date +%H%M%S)
mkdir -p "$d/frames"
chmod +x runner-aarch64
echo "$d" > last-boot.txt
cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/stride > "$d/fb.txt" 2>/dev/null
for t in $at; do
    (sleep "$t"; cat /dev/fb0 > "$d/panel-$t.raw"; cat /sys/class/graphics/fb0/pan >> "$d/fb.txt" 2>/dev/null) &
done
start=$(date +%s)
env PS2PORT_DISPLAY=1 PS2PORT_M5_STATS=1 PS2PORT_NATIVE_STATS=1 PS2PORT_HEADLESS=1 PS2PORT_RUN_SECONDS="$secs" PS2PORT_PAD="$pad" PS2PORT_FRAME_DIR="$d/frames" "$@" \
    ./runner-aarch64 /userdata/ps2bench/game/SLUS_208.26 > "$d/log.txt" 2>&1
rc=$?
wait
echo "exit $rc after $(($(date +%s) - start)) s, $(ls "$d/frames" | wc -l) screenshots: $d"
grep -a '\[native\]\|\[display\]\|\[gles\]\|^gl:' "$d/log.txt" | cut -c1-120
grep -a '^\[m5\]' "$d/log.txt" | cut -c1-240
for f in samples.bin samples.bin.maps samples.bin.threads; do [ -f "$f" ] && [ "$f" -nt last-boot.txt ] && mv "$f" "$d/"; done
[ -f "$d/samples.bin" ] && echo "samples: $d/samples.bin (PS2PORT_SAMPLE=samples.bin)"
tail -2 "$d/log.txt" | cut -c1-120
