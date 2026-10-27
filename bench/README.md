# bench: Phase 2 device benchmarks

The tests in [docs/PHASE_2_DEVICE_BENCH.md](../docs/PHASE_2_DEVICE_BENCH.md),
for the RG35XX (H700: 4× Cortex-A53, Mali-G31 MP2). C11 + CMake. Every
result file records its run conditions.

| Binary | Tests | Linking |
|---|---|---|
| `bench_cpu` | T5 CPU path (de-swizzle + CLUT), T6 (recompiled-EE kernels), T7 (VU1-style NEON transform), T8 memory / SD / thermal | static: runs on any aarch64 Linux, and under qemu-user |
| `bench_gl` | T1 draw rate, T2 fill / blend, T3 render targets / readback, T4 upload, T5 GPU palette path, T8 audio | dynamic against libc only; loads the firmware's `libSDL2-2.0.so.0` with `dlopen` and gets GLES 3.0 entry points through `SDL_GL_GetProcAddress` |

## Build (WSL / Linux)

The build host needs these packages:

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config gcc-aarch64-linux-gnu qemu-user libsdl2-dev libgles-dev libegl-dev mesa-utils
```

Then build:

```bash
scripts/build_bench.sh
```

This builds `bench/build/native` and `bench/build/aarch64` and runs both
self-tests (aarch64 under qemu). It also checks that the checksums match
across the two builds, and reports the glibc version `bench_gl` needs.
Add `--gl-smoke` to also run `bench_gl --quick` in a desktop window.

**Timings from qemu or the desktop are not device timings.** Those runs
only show that the code paths work and give correct results.

## Run on the device

The device needs SSH key login. The scripts use `BatchMode`, so they never
ask for a password.

```bash
BENCH_HOST=root@<device-ip> scripts/deploy.sh
```

```bash
BENCH_HOST=root@<device-ip> BENCH_GOVERNOR=performance scripts/run_remote.sh
```

`run_remote.sh` runs `run_day1.sh` on the device, in the day-one order of
the spec. It then copies `results/` back into
`bench/results/<date>-<host>/`.

Before `bench_gl` can take the display, the firmware's frontend usually
has to be stopped. Pass the stop and restart commands as `BENCH_PRE=...`
and `BENCH_POST=...`. For a first sanity pass, use `BENCH_QUICK=1`.

## Options

| Option | Effect |
|---|---|
| `--quick` | small sizes, 1 rep, no warm-up |
| `--reps N` | repetitions per measurement (default 3: median, min, max) |
| `--warmup S` | warm-up before the first test (default 10 s) |
| `--out DIR` | where results go (default `results`) |
| `--tests` | comma-separated list of tests to run |
| `--selftest` | `bench_cpu` only: correctness checks and checksums |
| `--thermal S` | `bench_cpu` only: S seconds of 4-core load, sampling clock and temperature |
| `--windowed` | `bench_gl` only: a 640×480 window instead of fullscreen |
| `--audio-s S` | `bench_gl` only: length of each audio run |

Results are written as `<date>-<firmware>-<cpu|gl|thermal>.json`, plus a
CSV of the same rows.
