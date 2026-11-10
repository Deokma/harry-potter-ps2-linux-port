#!/bin/sh
# Knulli "Ports" launcher: starts the port from the device's own menu, no PC needed. NOT YET RUN ON THE DEVICE (written
# 2026-11-10 without device access); scripts/install_port.sh puts it in place. See docs/RUNNING.md.
#
#   /userdata/roms/ports/HarryPotterPS2.sh           this file (appears under Ports after a gamelist refresh)
#   /userdata/roms/ports/HarryPotterPS2/             runner-aarch64, logs (last-run.log, frames.txt)
#   GAME_DIR (below)                                  the files of your own disc, SLUS_208.26 among them; saves in GAME_DIR/mc0/
#
# Controls: the device's pad, PS2 layout by position. Quit: hold Menu + Start (or L1 + R1 + Select + Start) for a second.
# Screenshot: Menu + L1 (to /userdata/screenshots/). The session stops at 85 C (the CPU zone), as every test run does.

GAME_DIR=${GAME_DIR:-/userdata/ps2bench/game}
PORT_DIR=$(cd "$(dirname "$0")/HarryPotterPS2" 2>/dev/null && pwd)
LIMIT_MC=${LIMIT_MC:-85000}

log() { echo "$*" >> "$PORT_DIR/last-run.log"; }
[ -n "$PORT_DIR" ] || exit 1
: > "$PORT_DIR/last-run.log"
if [ ! -x "$PORT_DIR/runner-aarch64" ]; then
    chmod +x "$PORT_DIR/runner-aarch64" 2>/dev/null || { log "no runner-aarch64 in $PORT_DIR"; exit 1; }
fi
if [ ! -f "$GAME_DIR/SLUS_208.26" ]; then
    log "no SLUS_208.26 in $GAME_DIR: set GAME_DIR at the top of this script to the folder with your disc's files"
    exit 1
fi

hottest() {
    m=0
    for z in /sys/class/thermal/thermal_zone*/temp; do
        v=$(cat "$z" 2>/dev/null || echo 0)
        [ "$v" -gt "$m" ] && m=$v
    done
    echo "$m"
}
if [ "$(hottest)" -gt "$LIMIT_MC" ]; then
    log "too hot to start: $(hottest) m-C"
    exit 3
fi

# The GPU's governor stays at 240 MHz under this load: its minimum goes to the maximum for the session and back after.
gpu=/sys/class/devfreq/gpu
gpu_old=$(cat $gpu/min_freq 2>/dev/null)
[ -n "$gpu_old" ] && echo 648000000 > $gpu/min_freq 2>/dev/null
restore() {
    [ -n "$gpu_old" ] && echo "$gpu_old" > $gpu/min_freq 2>/dev/null
}
trap restore EXIT
trap 'exit 143' TERM INT HUP

cd "$PORT_DIR" || exit 1
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/var/run}
log "start $(date '+%F %T'), GAME_DIR $GAME_DIR, $(hottest) m-C"
# In a process group of its own, so that the heat stop reaches everything the runner started (TERM first: SDL turns it into a quit)
setsid env PS2PORT_DISPLAY=1 PS2PORT_HEADLESS=1 PS2PORT_LAG_LOG=1 PS2PORT_FRAME_LOG="$PORT_DIR/frames.txt" \
    ./runner-aarch64 "$GAME_DIR/SLUS_208.26" >> "$PORT_DIR/last-run.log" 2>&1 &
pid=$!
while kill -0 "$pid" 2>/dev/null; do
    t=$(hottest)
    if [ "$t" -gt "$LIMIT_MC" ]; then
        log "THERMAL STOP: $t m-C"
        kill -TERM "-$pid" 2>/dev/null
        sleep 3
        kill -KILL "-$pid" 2>/dev/null
        break
    fi
    sleep 1
done
wait "$pid" 2>/dev/null
log "end $(date '+%F %T'), status $?"
