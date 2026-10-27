#!/usr/bin/env bash
# Build bench/ natively and for aarch64, run the self-tests (aarch64 under
# qemu-user) and check that both builds produce the same checksums.
# Run inside WSL / Linux from the repo root:
#   scripts/build_bench.sh            # builds + self-tests + quick CPU runs
#   scripts/build_bench.sh --gl-smoke # also a quick bench_gl run in a window
# qemu-user and desktop numbers are NOT device timings.
set -euo pipefail
cd "$(dirname "$0")/.."

gl_smoke=0
[ "${1:-}" = "--gl-smoke" ] && gl_smoke=1

gen=()
command -v ninja >/dev/null && gen=(-G Ninja)

cmake -S bench -B bench/build/native "${gen[@]}" -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build/native
cmake -S bench -B bench/build/aarch64 "${gen[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/bench/cmake/aarch64-linux-gnu.cmake"
cmake --build bench/build/aarch64

echo "== self-test, native"
bench/build/native/bench_cpu --selftest | tee bench/build/native/selftest.txt
echo "== self-test, aarch64 under qemu-user"
qemu-aarch64 bench/build/aarch64/bench_cpu --selftest | tee bench/build/aarch64/selftest.txt

# x86-64 and aarch64 must agree bit for bit (T5 tables, T6 PS2 FPU mode,
# T7 transform); NEON-vs-scalar is checked inside the aarch64 run.
if diff <(grep checksum bench/build/native/selftest.txt) \
        <(grep checksum bench/build/aarch64/selftest.txt); then
    echo "checksums: native == aarch64"
else
    echo "checksums DIFFER between native and aarch64" >&2
    exit 1
fi

echo "== bench_gl (aarch64): dynamic dependencies and glibc floor"
aarch64-linux-gnu-readelf -d bench/build/aarch64/bench_gl | grep NEEDED || true
aarch64-linux-gnu-objdump -T bench/build/aarch64/bench_gl | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 \
    | sed 's/^/needs at least /'

echo "== quick CPU runs (correctness of the harness only; timings meaningless)"
mkdir -p bench/build/smoke
bench/build/native/bench_cpu --quick --out bench/build/smoke/native
qemu-aarch64 bench/build/aarch64/bench_cpu --quick --tests T5,T6,T7 --out bench/build/smoke/aarch64

if [ "$gl_smoke" = 1 ]; then
    echo "== bench_gl --quick under the desktop GL (WSLg / Mesa): code paths only"
    bench/build/native/bench_gl --quick --windowed --out bench/build/smoke/native-gl
fi
echo "done"
