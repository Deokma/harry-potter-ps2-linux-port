#!/bin/sh
# Run a binary on the device under device_guard.sh (85 C and time limits).
#
#   scripts/device_run.sh [-g] FILE [FILE...] -- COMMAND [ARGS...]
#
# Copies each FILE whose content changed to $BENCH_DIR/run on the device and runs
# COMMAND there. -g: the command needs the display, so the frontend is stopped
# first and restarted after. Environment: DEVICE_KHZ (default 1416000, the 4-core
# sustained clock), DEVICE_TIMEOUT (seconds, default 120). A lost connection (a
# reboot, say) is reported and doesn't hang: exit status 255.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
host=${BENCH_HOST:?set BENCH_HOST in .env}
dir=${BENCH_DIR:?set BENCH_DIR in .env}/run
ssh_opts="${BENCH_SSH_OPTS:-} -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
scp_opts=$(echo "${BENCH_SSH_OPTS:-}" | sed 's/-p /-P /')

gl=0
if [ "${1:-}" = "-g" ]; then gl=1; shift; fi
files=""
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do files="$files $1"; shift; done
shift
# shellcheck disable=SC2086
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
status=0
# shellcheck disable=SC2086
ssh $ssh_opts "$host" "
cd '$dir'
$pre >/dev/null 2>&1 || true
sleep 1
${BENCH_ENV:-} sh device_guard.sh ${DEVICE_KHZ:-1416000} ${DEVICE_TIMEOUT:-120} -- $*
rc=\$?
$post
exit \$rc
" || status=$?
[ "$status" = 255 ] && echo "DEVICE LOST: the connection dropped (reboot?)"
exit "$status"
