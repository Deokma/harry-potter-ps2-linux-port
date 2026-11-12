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
# Clocks for the session, put back when it ends (2026-11-12, heat work; docs/PHASE_9_HEAT.md): the CPU's ceiling CPU_KHZ (default
# 1416000, the clock every test ran at; the firmware's own ceiling is 1512000), the CPU governor CPU_GOV (default: left as it is),
# the GPU's minimum GPU_MIN_HZ (default 648000000; 0 leaves it). The log gets a line with temperature and clocks every 30 s.

GAME_DIR=${GAME_DIR:-/userdata/ps2bench/game}
PORT_DIR=$(cd "$(dirname "$0")/HarryPotterPS2" 2>/dev/null && pwd)
LIMIT_MC=${LIMIT_MC:-85000}
CPU_KHZ=${CPU_KHZ:-1416000}
CPU_GOV=${CPU_GOV:-}
GPU_MIN_HZ=${GPU_MIN_HZ:-648000000}

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

# the hottest zone, read with the shell's own `read` (no process per zone): HOT, in milli-C
hottest() {
    m=0
    for z in /sys/class/thermal/thermal_zone*/temp; do
        v=0
        read -r v 2>/dev/null < "$z" || :
        case $v in '' | *[!0-9]*) v=0 ;; esac
        [ "$v" -gt "$m" ] && m=$v
    done
    HOT=$m
}
hottest
if [ "$HOT" -gt "$LIMIT_MC" ]; then
    log "too hot to start: $HOT m-C"
    exit 3
fi

# Clocks for the session; restore() puts every one back, on any way out (EXIT also runs after the TERM/INT/HUP trap's exit).
gpu=/sys/class/devfreq/gpu
cpu=/sys/devices/system/cpu/cpu0/cpufreq
gpu_old=$(cat $gpu/min_freq 2>/dev/null)
cpu_old_max=$(cat $cpu/scaling_max_freq 2>/dev/null)
cpu_old_gov=$(cat $cpu/scaling_governor 2>/dev/null)
restore() {
    [ -n "$gpu_old" ] && [ "$GPU_MIN_HZ" != 0 ] && echo "$gpu_old" > $gpu/min_freq 2>/dev/null
    [ -n "$cpu_old_max" ] && echo "$cpu_old_max" > $cpu/scaling_max_freq 2>/dev/null
    [ -n "$cpu_old_gov" ] && [ -n "$CPU_GOV" ] && echo "$cpu_old_gov" > $cpu/scaling_governor 2>/dev/null
}
trap restore EXIT
trap 'exit 143' TERM INT HUP
[ -n "$gpu_old" ] && [ "$GPU_MIN_HZ" != 0 ] && echo "$GPU_MIN_HZ" > $gpu/min_freq 2>/dev/null
[ -n "$cpu_old_max" ] && echo "$CPU_KHZ" > $cpu/scaling_max_freq 2>/dev/null
[ -n "$CPU_GOV" ] && echo "$CPU_GOV" > $cpu/scaling_governor 2>/dev/null

cd "$PORT_DIR" || exit 1
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/var/run}
log "start $(date '+%F %T'), GAME_DIR $GAME_DIR, $HOT m-C; CPU max $(cat $cpu/scaling_max_freq 2>/dev/null) kHz, governor $(cat $cpu/scaling_governor 2>/dev/null); GPU min $(cat $gpu/min_freq 2>/dev/null) Hz"
# In a process group of its own, so that the heat stop reaches everything the runner started (TERM first: SDL turns it into a quit)
setsid env PS2PORT_DISPLAY=1 PS2PORT_HEADLESS=1 PS2PORT_LAG_LOG=1 PS2PORT_FRAME_LOG="$PORT_DIR/frames.txt" \
    ./runner-aarch64 "$GAME_DIR/SLUS_208.26" >> "$PORT_DIR/last-run.log" 2>&1 &
pid=$!
n=0
while kill -0 "$pid" 2>/dev/null; do
    hottest
    if [ "$HOT" -gt "$LIMIT_MC" ]; then
        log "THERMAL STOP: $HOT m-C"
        kill -TERM "-$pid" 2>/dev/null
        sleep 3
        kill -KILL "-$pid" 2>/dev/null
        break
    fi
    n=$((n + 1))
    if [ $((n % 30)) = 0 ]; then
        khz=0; g=0
        read -r khz 2>/dev/null < $cpu/scaling_cur_freq || :
        read -r g 2>/dev/null < $gpu/cur_freq || :
        log "[heat] t=$n s $HOT m-C, CPU $khz kHz, GPU $g Hz"
    fi
    sleep 1
done
wait "$pid" 2>/dev/null
log "end $(date '+%F %T'), status $?"
