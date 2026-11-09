#!/bin/sh
# Runs on the device: one command at a pinned CPU clock, under a time limit and
# a temperature limit. Every device run goes through this (stop rule: over 85 C).
#
#   sh device_guard.sh KHZ SECONDS -- COMMAND [ARGS...]
#
# Refuses to start above the limit, checks once a second while the command runs
# and kills it above the limit. The clock and governor are restored on the way
# out. Exit status: the command's; 3 for a thermal stop; 124 when out of time.
limit=${GUARD_LIMIT_MC:-85000}
khz=$1
seconds=$2
shift 2
[ "$1" = "--" ] && shift

hottest() {
    m=0
    for z in /sys/class/thermal/thermal_zone*/temp; do
        v=$(cat "$z" 2>/dev/null || echo 0)
        [ "$v" -gt "$m" ] && m=$v
    done
    echo "$m"
}
celsius() { echo "$(($1 / 1000)).$((($1 % 1000) / 100))"; }

start=$(hottest)
if [ "$start" -gt "$limit" ]; then
    echo "THERMAL STOP: $(celsius "$start") C before starting, limit $(celsius "$limit") C"
    exit 3
fi

cpu=/sys/devices/system/cpu/cpu0/cpufreq
old_gov=$(cat $cpu/scaling_governor)
old_max=$(cat $cpu/scaling_max_freq)
echo performance > $cpu/scaling_governor
echo "$khz" > $cpu/scaling_max_freq
sleep 1
echo "clock $(cat $cpu/scaling_cur_freq) kHz (governor $(cat $cpu/scaling_governor))"

# In a process group of its own, so that a stop reaches every process the command started: killing the shell alone
# left the runner going (Phase 7 soak). TERM first (SDL turns it into a quit), KILL 3 s later. No "--" in kill: the
# device's sh is dash, whose kill rejects it ("Illegal number: -"), so the stop silently did nothing (found 2026-11-09).
setsid timeout "$seconds" "$@" &
pid=$!
stopgroup() {
    kill -TERM "-$pid" 2>/dev/null
    sleep 3
    kill -KILL "-$pid" 2>/dev/null
}
peak=$start
stopped=0
while kill -0 "$pid" 2>/dev/null; do
    t=$(hottest)
    [ "$t" -gt "$peak" ] && peak=$t
    if [ "$t" -gt "$limit" ] && [ "$stopped" = 0 ]; then
        echo "THERMAL STOP: $(celsius "$t") C while running, limit $(celsius "$limit") C"
        stopgroup
        stopped=1
    fi
    sleep 1
done
wait "$pid"
status=$?
# whatever outlived the command (timeout stops only its own child)
kill -KILL "-$pid" 2>/dev/null

echo "clock after $(cat $cpu/scaling_cur_freq) kHz"
echo "$old_max" > $cpu/scaling_max_freq
echo "$old_gov" > $cpu/scaling_governor
echo "temperature: start $(celsius "$start") C, peak $(celsius "$peak") C, end $(celsius "$(hottest)") C"
[ "$stopped" = 1 ] && exit 3
[ "$status" = 124 ] && echo "TIME LIMIT: stopped after $seconds s"
exit "$status"
