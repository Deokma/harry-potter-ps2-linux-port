#!/bin/sh
# Copies play sessions' logs from the device to private/play/ (log.txt, freq.txt; game-derived output: private/ is never
# tracked). Run from Git Bash in the repo root:
#   sh scripts/play_fetch.sh                 the newest session
#   sh scripts/play_fetch.sh play-120126 play-121055
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
host=${BENCH_HOST:?set BENCH_HOST in .env}
dir=${BENCH_DIR:?set BENCH_DIR in .env}/run
ssh_opts=${BENCH_SSH_OPTS:-}
scp_opts=$(echo "$ssh_opts" | sed 's/-p /-P /')
out="$here/../private/play"
mkdir -p "$out"
if [ $# -eq 0 ]; then
    # shellcheck disable=SC2086,SC2029
    set -- "$(ssh $ssh_opts "$host" "cd '$dir' && ls -dt play-* | head -1")"
fi
for d in "$@"; do
    mkdir -p "$out/$d"
    # shellcheck disable=SC2086
    scp -q $scp_opts "$host:$dir/$d/*.txt" "$out/$d/" && echo "$d: $(ls "$out/$d" | tr '\n' ' ')"
done
