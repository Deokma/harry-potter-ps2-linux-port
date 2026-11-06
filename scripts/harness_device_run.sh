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
    # by content: a new reference has the same size as the old one
    sum=$(md5sum < "$rec$suffix" | cut -d' ' -f1)
    # shellcheck disable=SC2086
    remote=$(ssh $ssh_opts "$host" "md5sum < '$dir/rec/$name$suffix' 2>/dev/null | cut -d' ' -f1")
    [ "$remote" = "$sum" ] || files="$files $rec$suffix"
done
# shellcheck disable=SC2086
[ -z "$files" ] || scp $scp_opts $files "$host:$dir/rec/"
# shellcheck disable=SC2086
scp $scp_opts "$harness" "$host:$dir/"

# shellcheck disable=SC2086
ssh $ssh_opts "$host" "cat > '$dir/device_guard.sh'" < "$here/device_guard.sh"
status=0
# device_guard.sh: pinned clock, time limit, 85 C limit (exit 3)
# shellcheck disable=SC2086
ssh $ssh_opts -o ServerAliveInterval=5 -o ServerAliveCountMax=3 "$host" "
cd '$dir'
${BENCH_PRE:-:} >/dev/null 2>&1 || true
sleep 2
sh device_guard.sh $khz ${HARNESS_TIMEOUT:-120} -- env $envs ./$(basename "$harness") rec/$name $iters
rc=\$?
[ \$rc = 0 ] || echo \"harness exit \$rc\"
${BENCH_POST:-:}
exit \$rc
" || status=$?
[ "$status" = 255 ] && echo "DEVICE LOST: the connection dropped (reboot?)"
exit "$status"
