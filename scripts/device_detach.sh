#!/bin/sh
# Like device_run.sh, for a run that must survive the connection dropping (the device's Wi-Fi does, under load).
#
#   scripts/device_detach.sh [-g] FILE [FILE...] -- COMMAND [ARGS...]
#
# Copies each changed FILE to $BENCH_DIR/run, starts COMMAND there with nohup under device_guard.sh, then polls until it
# has finished, riding out lost connections, and prints its log. -g: stops the frontend first and restarts it after.
# Environment: DEVICE_KHZ (default 1416000), DEVICE_TIMEOUT (seconds, default 120).
# After a poll that never gets through for 10 minutes it gives up with status 255: check uptime before calling it a reboot.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
host=${BENCH_HOST:?set BENCH_HOST in .env}
dir=${BENCH_DIR:?set BENCH_DIR in .env}/run
ssh_opts="${BENCH_SSH_OPTS:-} -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
scp_opts=$(echo "${BENCH_SSH_OPTS:-}" | sed 's/-p /-P /')

gl=0
if [ "${1:-}" = "-g" ]; then gl=1; shift; fi
files=""
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do files="$files $1"; shift; done
shift
# shellcheck disable=SC2086
# One run at a time: two runners do not fit in the device's memory, and the runs share detached.sh and detached.rc.
# shellcheck disable=SC2086
if ssh $ssh_opts "$host" "pidof runner-aarch64 >/dev/null || pgrep -f 'sh detache[d].sh' >/dev/null"; then
    echo "BUSY: a run is already going on the device (playing or testing); not starting another"
    exit 75
fi
ssh $ssh_opts "$host" "mkdir -p '$dir'"
# shellcheck disable=SC2086
ssh $ssh_opts "$host" "cat > '$dir/device_guard.sh'" < "$here/device_guard.sh"
for f in $files; do
    sum=$(md5sum < "$f" | cut -d' ' -f1)
    # shellcheck disable=SC2086
    remote=$(ssh $ssh_opts "$host" "[ -f '$dir/$(basename "$f")' ] && md5sum < '$dir/$(basename "$f")' | cut -d' ' -f1" || true)
    # shellcheck disable=SC2086
    [ "$remote" = "$sum" ] || scp -q $scp_opts "$f" "$host:$dir/"
done

pre=:
post=:
if [ "$gl" = 1 ]; then pre=${BENCH_PRE:-:}; post=${BENCH_POST:-:}; fi
# shellcheck disable=SC2086
ssh $ssh_opts "$host" "cat > '$dir/detached.sh'" <<EOF
cd '$dir'
rm -f detached.rc
$pre >/dev/null 2>&1 || true
sleep 1
${BENCH_ENV:-} sh device_guard.sh ${DEVICE_KHZ:-1416000} ${DEVICE_TIMEOUT:-120} -- $*
rc=\$?
$post >/dev/null 2>&1 || true
echo \$rc > detached.rc
EOF
# shellcheck disable=SC2086
ssh $ssh_opts "$host" "cd '$dir' && rm -f detached.rc && (nohup sh detached.sh > detached.log 2>&1 < /dev/null &) ; echo started" | grep -v 'post-quantum\|store now\|upgraded' || true

lost=0
while :; do
    sleep 10
    # shellcheck disable=SC2086
    if state=$(ssh $ssh_opts "$host" "cat '$dir/detached.rc' 2>/dev/null || echo running" 2>/dev/null); then
        lost=0
        case "$state" in
            *running*) ;;
            *) break ;;
        esac
    else
        lost=$((lost + 1))
        if [ "$lost" -ge 30 ]; then
            echo "DEVICE LOST: no answer for 10 minutes"
            exit 255
        fi
    fi
done
# shellcheck disable=SC2086
ssh $ssh_opts "$host" "cat '$dir/detached.log'" 2>/dev/null
exit "$(echo "$state" | tr -dc 0-9)"
