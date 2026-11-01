#!/usr/bin/env bash
# Run the day-one benchmarks on the device and fetch the results into
# bench/results/<date>-<host>/.
#   BENCH_HOST=root@192.168.1.50 scripts/run_remote.sh
# Optional:
#   BENCH_PRE='<cmd>'    run on the device first (e.g. stop the frontend so
#                        bench_gl can take the display)
#   BENCH_POST='<cmd>'   run on the device afterwards (e.g. restart it)
#   BENCH_ENV='SDL_VIDEODRIVER=kmsdrm'  environment for the run
#   BENCH_GOVERNOR=performance, BENCH_THERMAL_S=600, BENCH_QUICK=1,
#   BENCH_SD_FILE=path   passed through to run_day1.sh
# Needs SSH key login (BatchMode: this never prompts for a password).
# Settings can also come from ./.env (see .env.example).
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/env.sh
: "${BENCH_HOST:?set BENCH_HOST=user@device}"
dir="${BENCH_DIR:-ps2bench}"
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=30 ${BENCH_SSH_OPTS:-})

pass="BENCH_GOVERNOR='${BENCH_GOVERNOR:-}' BENCH_THERMAL_S='${BENCH_THERMAL_S:-600}'"
pass="$pass BENCH_QUICK='${BENCH_QUICK:-0}' BENCH_SD_FILE='${BENCH_SD_FILE:-}' ${BENCH_ENV:-}"
remote="cd '$dir' || exit 1"
[ -n "${BENCH_PRE:-}" ] && remote="$remote; ${BENCH_PRE}"
remote="$remote; env $pass sh ./run_day1.sh; rc=\$?"
[ -n "${BENCH_POST:-}" ] && remote="$remote; ${BENCH_POST}"
remote="$remote; exit \$rc"

echo "running on $BENCH_HOST (keep this connection open; the full run takes ~30-40 min)"
set +e
ssh "${ssh_opts[@]}" "$BENCH_HOST" "$remote"
rc=$?
set -e

dest="bench/results/$(date +%Y%m%d-%H%M%S)-${BENCH_HOST#*@}"
mkdir -p "$dest"
ssh "${ssh_opts[@]}" "$BENCH_HOST" "cd '$dir/results' && tar -cf - ." | tar -C "$dest" -xf -
echo "results in $dest (run exit code $rc)"
exit "$rc"
