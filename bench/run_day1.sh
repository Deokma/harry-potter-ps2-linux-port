#!/bin/sh
# Day-one benchmark order on the device (docs/PHASE_2_DEVICE_BENCH.md):
#   T8 system -> T1 -> T2 -> T4 / T5 -> T3 -> T6 / T7 -> T8 audio -> T8 thermal
# POSIX sh (busybox ash is enough). Run from the directory holding the
# binaries; results go to ./results.
# Environment (all optional):
#   BENCH_GOVERNOR=performance  set this cpufreq governor for the run and
#                               restore the previous one afterwards
#   BENCH_THERMAL_S=600         length of the thermal test (0 = skip)
#   BENCH_QUICK=1               quick mode for a first sanity run
#   BENCH_SD_FILE=path          read this file for the SD test instead of
#                               writing a temporary one
set -u
cd "$(dirname "$0")"
out=results
mkdir -p "$out"
q=""
[ "${BENCH_QUICK:-0}" = 1 ] && q="--quick"
thermal="${BENCH_THERMAL_S:-600}"
sdarg=""
[ -n "${BENCH_SD_FILE:-}" ] && sdarg="--sd $BENCH_SD_FILE"

saved=""
if [ -n "${BENCH_GOVERNOR:-}" ]; then
    for g in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        [ -w "$g" ] || continue
        saved="$saved $g=$(cat "$g")"
        echo "$BENCH_GOVERNOR" > "$g" || echo "cannot set governor in $g"
    done
fi
restore() {
    for pair in $saved; do
        echo "${pair#*=}" > "${pair%%=*}" 2>/dev/null
    done
}
trap restore EXIT INT TERM

log="$out/run_day1.log"
echo "run_day1 $(date) quick=${BENCH_QUICK:-0} governor=${BENCH_GOVERNOR:-unchanged}" | tee -a "$log"
uname -a | tee -a "$log"
step() {
    echo "== $*" | tee -a "$log"
    "$@" 2>&1 | tee -a "$log"
}
step ./bench_cpu $q --out "$out" --tests T8 $sdarg
step ./bench_gl $q --out "$out" --tests T1,T2,T4,T5,T3
step ./bench_cpu $q --out "$out" --tests T5,T6,T7
step ./bench_gl $q --out "$out" --tests T8
if [ "$thermal" != 0 ] && [ -z "$q" ]; then
    step ./bench_cpu --out "$out" --thermal "$thermal"
fi
echo "run_day1 finished $(date)" | tee -a "$log"
