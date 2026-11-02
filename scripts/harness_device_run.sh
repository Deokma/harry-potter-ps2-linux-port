#!/bin/sh
# Run a replay harness on the device at a pinned CPU clock (Phase 3, C).
#
#   scripts/harness_device_run.sh HARNESS RECORDING [ITERATIONS] [KHZ] [-- ENV=VAL ...]
#
# Copies the harness binary and RECORDING.in / .out / .vu0data / .vu0code
# (when present and not already there) to $BENCH_DIR/harness on the
# device, stops the frontend ($BENCH_PRE), sets the performance governor
# with scaling_max_freq = KHZ (default 1416000, the 4-core sustained
# clock), runs it with the given environment, then restores the governor
# and clock and restarts the frontend ($BENCH_POST). Settings from .env
# (scripts/env.sh). Recordings are game data: they go only to the user's
# own device.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
harness=$1
rec=$2
iters=${3:-20}
khz=${4:-1416000}
shift $(($# < 4 ? $# : 4))
[ "${1:-}" = "--" ] && shift
envs="$*"

host=${BENCH_HOST:?set BENCH_HOST in .env}
dir=${BENCH_DIR:?set BENCH_DIR in .env}/harness
ssh_opts=${BENCH_SSH_OPTS:-}
scp_opts=$(echo "$ssh_opts" | sed 's/-p /-P /')
name=$(basename "$rec")

# shellcheck disable=SC2086
ssh $ssh_opts "$host" "mkdir -p '$dir/rec'"
files=""
for suffix in .in .out .vu0data .vu0code; do
    [ -f "$rec$suffix" ] || continue
    size=$(wc -c < "$rec$suffix")
    # shellcheck disable=SC2086
    remote=$(ssh $ssh_opts "$host" "wc -c < '$dir/rec/$name$suffix' 2>/dev/null || echo 0")
    [ "$remote" = "$size" ] || files="$files $rec$suffix"
done
# shellcheck disable=SC2086
[ -z "$files" ] || scp $scp_opts $files "$host:$dir/rec/"
# shellcheck disable=SC2086
scp $scp_opts "$harness" "$host:$dir/"

# shellcheck disable=SC2086
ssh $ssh_opts "$host" "
cd '$dir'
${BENCH_PRE:-:} >/dev/null 2>&1 || true
sleep 2
cpu=/sys/devices/system/cpu/cpu0/cpufreq
old_gov=\$(cat \$cpu/scaling_governor); old_max=\$(cat \$cpu/scaling_max_freq)
echo performance > \$cpu/scaling_governor
echo $khz > \$cpu/scaling_max_freq
sleep 1
echo \"clock \$(cat \$cpu/scaling_cur_freq) kHz (governor \$(cat \$cpu/scaling_governor))\"
env $envs ./$(basename "$harness") rec/$name $iters || echo \"harness exit \$?\"
echo \"clock after \$(cat \$cpu/scaling_cur_freq) kHz\"
echo \$old_max > \$cpu/scaling_max_freq
echo \$old_gov > \$cpu/scaling_governor
${BENCH_POST:-:}
"
