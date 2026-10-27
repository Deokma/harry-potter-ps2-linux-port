# Phase 2: device benchmarks

**Part A done (2026-10-28):**
- the benchmark suite of [PHASE_2_DEVICE_BENCH.md](PHASE_2_DEVICE_BENCH.md)
  is built, as `bench/`;
- it is cross-compiled for the RG35XX;
- it is checked for correctness off the device.

**Part B (the on-device run) waits for the device.** Part B is the actual
measurement. Phase 2 is finished only when Part B has run and its results
have been checked against the thresholds. Any line that misses by more
than 2× reopens Part C of the Phase 1 verdict.

**Nothing in this document is a device measurement.** The qemu and
desktop runs below show only that the code paths work and give correct
results. Their timings are discarded.

## A1. What exists

| Path | What |
|---|---|
| `bench/CMakeLists.txt`, `bench/cmake/aarch64-linux-gnu.cmake` | C11 build: native, or cross-compiled to aarch64 (`-march=armv8-a -mtune=cortex-a53`) |
| `bench/src/main_cpu.c` → **`bench_cpu`** (static) | T5 CPU path, T6, T7, T8 memory / SD / thermal, `--selftest` |
| `bench/src/main_gl.c` → **`bench_gl`** | T1, T2, T3, T4, T5 GPU path, T8 audio |
| `bench/src/t5_cpu.c` | PSMT8 / PSMT4 + CLUT (CSM1) → RGBA8. Three variants: scalar block tables, NEON TBL de-swizzle with a TBL CLUT, and NEON de-swizzle with a scalar CLUT. 1 and 2 threads |
| `bench/src/t6_ee.c`, `t6_kernels.inc` | recompiler-style EE kernels: integer/branch object loop, scalar FPU in PS2 mode, `lq`/`sq` moves, dependent loads through address translation (DRAM and scratchpad). Each comes in register-struct and register-local builds, plus a counting build |
| `bench/src/t7_vu.c` | VU1-style vertex kernel: 4×4 transform, clip flags, divide, viewport, 3 lights. Packs GS PACKED ST / RGBAQ / XYZ2, as scalar and NEON, on 1 and 2 cores |
| `bench/src/t1_draw.c` … `t8_audio.c` | the GLES tests, per the spec |
| `bench/src/sdl_min.h`, `sdl_dyn.c`, `sdl_check.c` | hand-declared SDL2 ABI, loaded with `dlopen`; checked against the real headers at compile time |
| `bench/src/gl_dyn.*`, `gl_funcs.h` | GLES 3.0 entry points through `SDL_GL_GetProcAddress` |
| `bench/run_day1.sh` | device side (POSIX sh): runs the tests in the spec's day-one order, logs, optional governor |
| `scripts/build_bench.sh` | WSL: both builds, both self-tests, checksum comparison, glibc report, quick runs |
| `scripts/deploy.sh`, `scripts/run_remote.sh` | SSH key only (`BatchMode`): copy over `tar`, run, fetch `bench/results/<date>-<host>/` |
| `bench/README.md` | build and run reference |

The bench is about 4,400 lines of C.

## A2. Design decisions

- **D2.1 `bench_cpu` is static.**
  - It runs on any aarch64 Linux, whatever the firmware's glibc.
  - qemu-user runs it without a sysroot.
- **D2.2 `bench_gl` links only against libc.**
  - At run time it loads the firmware's `libSDL2-2.0.so.0` with `dlopen`,
    and takes every GL entry point from `SDL_GL_GetProcAddress`.
  - Types and enums come from the official Khronos headers on the build
    host, with prototypes switched off. They are staged into the build
    tree, so the cross build never sees the host's `/usr/include`.
  - The slice of SDL2 used is declared by hand. `sdl_check.c` asserts
    every value and struct layout against the real SDL 2.32 headers.
  - Result: no device libraries are needed at build time, and the same
    binary works whether the firmware uses the Mali blob or Panfrost.
- **D2.3 glibc floor: `bench_gl` needs glibc 2.34 or newer.**
  - It needs `GLIBC_2.17` and `GLIBC_2.34`, the latter for
    `__libc_start_main` and the pthread symbols.
  - `_GNU_SOURCE` is confined to `linux_gnu.c`. Elsewhere it would
    redirect `strtol` and friends to `__isoc23_*@GLIBC_2.38`.
  - If the firmware's glibc is older, build `bench_gl` against libraries
    copied off the device, or in an older-glibc container.
  - `deploy.sh` prints the device's glibc version.
- **D2.4 No fused multiply-add anywhere** (`-ffp-contract=off`).
  - T6 builds with `-frounding-math`.
  - The PS2 FPU mode (round toward zero, flush to zero) is switched by
    `noinline` asm with memory clobbers.
  - The self-test caught GCC moving a division across `_mm_setcsr`. A
    recompiler's FPU-mode handling will need the same care.
- **D2.5 T6 measures guest instructions exactly.**
  - Each kernel is one C statement per R5900 instruction, with the guest
    code in the comments.
  - A counting build tallies executed instructions per basic block. The
    timed and counted runs start from the same memory snapshot and must
    end in the same state.
  - Guest instructions per second therefore have no estimate in them.
  - The register-local build models a register-allocating recompiler. The
    self-test caught a real recompiler bug class there: one guest register
    (t0) had two homes, a 64-bit copy and a 128-bit `lq`/`sq` copy.
- **D2.6 T1's "mix" variant models the Part B state stream.** Per draw:
  - 82 % textured, and 90 % of those rebind a texture (CLUT changes);
  - 74 % blended (state changes only on transitions);
  - one uniform update (colour / CLUT selector);
  - program changes on 10 %;
  - about 6 render-target switches per frame.

  `mix_gs_t1` / `t2` add the conversion of GS PACKED vertices, on the
  render thread or on a producer thread one frame ahead. This is a model,
  not a replay of a GS dump (A5).
- **D2.7 T8 audio uses a proxy for underruns.**
  - SDL doesn't report device underruns, so a callback arriving more than
    1.5 periods late counts as a probable underrun.
  - The callback mixes 48 voices, like an SPU2 mixer. It writes the mix to
    a scratch buffer and plays silence.
- **D2.8 T5 uses PCSX2's GS layout tables.**
  - The self-test checks that they are permutations and that every fast
    path matches per-texel address arithmetic.
  - A byte-exact check against a GS dump's VRAM is a Phase 3 item.
- **D2.9 Device access is SSH key only.**
  - Copying uses `tar` over ssh, so dropbear / busybox are enough.
  - The governor changes only with `BENCH_GOVERNOR` and is restored on
    exit.
  - The SD test writes and deletes a 128 MB file, unless `--sd FILE` /
    `BENCH_SD_FILE` names an existing file.

## A3. Verification off the device (2026-10-28)

**Tools:** WSL2 Ubuntu 26.04, GCC 15.2 (native and `aarch64-linux-gnu`),
qemu-user 10.2, CMake 4.2, Ninja 1.13, SDL 2.32.10.

- **Builds:** native and aarch64, no warnings at `-Wall -Wextra`.
  - `bench_cpu`: static aarch64 ELF, 1.0 MB.
  - `bench_gl`: 148 KB; needs only `libc.so.6` and `ld-linux-aarch64.so.1`.
- **Self-tests:** 0 failures natively, and 0 under qemu.
  - The aarch64 run also checks NEON against scalar for T5 and T7.
  - All 8 checksums are identical on x86-64 and aarch64, covering T5
    tables / CLUT, five T6 kernels in PS2 FPU mode, and T7 unlit / lit.
  - Before the two fixes in D2.4 / D2.5, the self-tests failed in exactly
    those places.
- **Quick runs** (code paths only):

  | Binary | Where | What ran |
  |---|---|---|
  | `bench_cpu` | natively | T8, T5, T6, T7, thermal |
  | `bench_cpu` | under qemu | T5, T6, T7, thermal |
  | `bench_gl` | WSLg / Mesa llvmpipe, GLES 3.2 | T1–T5, T3, T8 audio |

  Every result file is valid JSON, and no GL errors were recorded. The
  framebuffer-fetch path and the GPU timer query both ran.
- **Not verified, and only the device can do it:**
  - anything about A53 or Mali-G31 performance;
  - the firmware's SDL video backend (KMSDRM / fbdev);
  - the Mali driver's extensions (timer query, framebuffer fetch);
  - user-space perf counters on the device kernel;
  - real audio underruns.

## A4. Part B: the device run

1. On the device: enable SSH and add this PC's public key. That is your
   step; I never type passwords.
2. In WSL: `scripts/build_bench.sh`.
3. `BENCH_HOST=root@<ip> scripts/deploy.sh`. It prints the device's glibc,
   which needs to be 2.34 or newer (D2.3).
4. Find the firmware's commands to stop and restart the frontend, so
   `bench_gl` can take the display. They depend on the firmware, so they
   aren't known yet.
5. A sanity pass, with the frontend commands in `BENCH_PRE` / `BENCH_POST`:
   `BENCH_QUICK=1 BENCH_PRE='…' BENCH_POST='…' scripts/run_remote.sh`.
6. The full run (about 30–40 min, including 10 min thermal and 2 min audio):
   `BENCH_GOVERNOR=performance BENCH_PRE='…' BENCH_POST='…' scripts/run_remote.sh`.
7. The results land in `bench/results/<date>-<host>/` and get committed. I
   then compare them with the thresholds in PHASE_2_DEVICE_BENCH and
   update PHASE_1 Part C.

## A5. Known limitations (carried into Phase 3)

- **T6 runs synthetic kernels only.** The spec's real game functions come
  from the Phase 3 prototype.
- **T7 measures a native NEON kernel**, which is the HLE route. The cost
  of translated VU1 microcode needs the Phase 3 translator.
- **T1's mix is a statistical model of the draw stream.** A renderer
  replaying a real GS dump (N02 / N03) would be the stronger test, and it
  belongs to the Phase 3 renderer prototype.
- **T8's audio check is a callback-timing proxy.** Real ALSA underrun
  counts would need device-specific access.
- **T2 doesn't emulate PS2 texture filtering of palettized textures.**
  GS bilinear filtering on CLUT textures needs four lookups. T5's GPU path
  measures nearest lookups only.
