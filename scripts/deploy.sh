#!/usr/bin/env bash
# Copy the aarch64 bench binaries and run_day1.sh to the device over SSH.
#   BENCH_HOST=root@192.168.1.50 scripts/deploy.sh
# Needs SSH key login (BatchMode: this never prompts for a password).
# Settings can also come from ./.env (see .env.example).
# BENCH_DIR on the device defaults to ps2bench (in the login's home).
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/env.sh
: "${BENCH_HOST:?set BENCH_HOST=user@device}"
dir="${BENCH_DIR:-ps2bench}"
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=10 ${BENCH_SSH_OPTS:-})

build=bench/build/aarch64
for f in "$build/bench_cpu" "$build/bench_gl"; do
    # -f, not -x: Git Bash on Windows does not see Linux ELFs as executable
    [ -f "$f" ] || { echo "missing $f: run scripts/build_bench.sh first" >&2; exit 1; }
done
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
cp "$build/bench_cpu" "$build/bench_gl" bench/run_day1.sh "$stage/"
chmod +x "$stage"/*
# tar over ssh: works with dropbear and busybox, no sftp-server needed;
# -o: do not restore ownership (fails on exFAT /userdata)
tar -C "$stage" -cf - . | ssh "${ssh_opts[@]}" "$BENCH_HOST" "mkdir -p '$dir' && tar -o -C '$dir' -xf -"
ssh "${ssh_opts[@]}" "$BENCH_HOST" "cd '$dir' && ls -l && (ldd --version 2>&1 | head -1 || true)"
echo "deployed to $BENCH_HOST:$dir"
