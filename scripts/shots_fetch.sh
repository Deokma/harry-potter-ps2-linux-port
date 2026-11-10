#!/bin/sh
# Copies the runner's screenshots (Menu + L1 on the device: /userdata/screenshots/ps2port-*.png) to private/screenshots/device/
# (game material: private/ is never tracked). Run from Git Bash in the repo root:  sh scripts/shots_fetch.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/env.sh"
out="$here/../private/screenshots/device"
mkdir -p "$out"
scp_opts=$(echo "${BENCH_SSH_OPTS:-}" | sed 's/-p /-P /')
# shellcheck disable=SC2086
scp -q $scp_opts "${BENCH_HOST:?set BENCH_HOST in .env}:/userdata/screenshots/ps2port-*.png" "$out/" && ls "$out" | wc -l | awk '{print $1 " files in private/screenshots/device"}'
