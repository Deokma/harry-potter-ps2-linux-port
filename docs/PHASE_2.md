# Phase 2: device benchmarks

**Part A done (2026-10-28):**
- the benchmark suite of [PHASE_2_DEVICE_BENCH.md](PHASE_2_DEVICE_BENCH.md)
  is built, as `bench/`;
- it is cross-compiled for the RG35XX;
- it is checked for correctness off the device.

**Part B done (2026-11-01), on an RG40XX H running Knulli.**
- Every condition line from PHASE_1 Part C passes on the device (B3).
- Two Mali driver traps were found and the bench fixed (B2).
- Renderer rules for Phase 3 are in B5.
- No line misses by more than 2× without a passing alternative, so
  PHASE_1 Part C gets an update (C8) rather than reopening.
- **Phase 2 is complete.** Reviewed; Phase 3 approved on 2026-11-01.

**Part A contains no device measurements.** Its qemu and desktop runs show
only that the code paths work and give correct results; their timings
are discarded. Part B is the device.

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

## Part B: the device run (2026-11-01)

**Everything in Part B ran on the device.** Results:
`bench/results/20261101-142702-192.168.0.165/` (full run: `quick=0`,
performance governor, 3 repetitions, 10 s warm-ups, 60 s of audio per
buffer size, 600 s heat test, 17 min in total). The quick pass that
checked the fixed bench is in `…-140958-…-quick/`, and the run that took
the device down is in `…-140000-quick-device-hang/`.

### B1. Device and setup

| | |
|---|---|
| Device | **Anbernic RG40XX H**: same H700 SoC as the RG35XX family the plan names, with a different case and cooling |
| SoC | Allwinner H616 family (`sun50iw9`), 4× Cortex-A53, **1.512 GHz** max at stock (1.6 / 1.7 GHz steps exist as overclock) |
| RAM | 973 MB; **825 MB available** with the frontend stopped |
| GPU | Mali-G31 MP2, ARM's closed driver (`mali_kbase`, `libmali` **r20p0**), devfreq 240–648 MHz (`simple_ondemand`, not pinned for the run) |
| Display | fbdev `/dev/fb0` 640×480, no DRM; SDL 2.30.12 with its `mali` video driver |
| GL | OpenGL ES 3.2, GLSL ES 3.20; has `GL_ARM_shader_framebuffer_fetch`, `GL_EXT_shader_pixel_local_storage`, `GL_KHR_blend_equation_advanced_coherent`, `GL_EXT_disjoint_timer_query`, `GL_EXT_buffer_storage`; **no** `GL_EXT_shader_framebuffer_fetch` |
| Firmware | **Knulli "Gladiator II"** (Batocera 2024.11 base, build 2025-08-12), kernel 4.9.170, glibc 2.40 |
| Audio | PipeWire, socket under `/var/run` |

How the scripts reach it (all in `.env`, see `.env.example`):
- **SSH key login.** Root's home on Knulli is on exFAT
  (`/userdata/system`), so dropbear refuses keys there.
  `scripts/device_ssh_setup.sh` starts a second dropbear on **port 2222**
  in a chroot mirror whose root home is a protected tmpfs folder. It runs
  once per boot, with the user typing the password.
  `BENCH_DIR=/userdata/ps2bench`.
- **Frontend.** `BENCH_PRE=/etc/init.d/S31emulationstation stop`.
  `BENCH_POST` restarts EmulationStation in the real root
  (`chroot /proc/1/root`, detached with `setsid`), so it keeps
  `HOME=/userdata/system`.
- **Audio.** `BENCH_ENV=XDG_RUNTIME_DIR=/var/run`.
- **Correctness on the device:** `bench_cpu --selftest` gives the same 8
  checksums as the x86-64 build (PS2-mode FPU, CLUT conversion, VU1
  kernels).

### B2. Two driver traps found on the device

The first quick passes exposed two behaviours of the Mali r20p0 driver.
Both made the bench measure the wrong thing, and one took the device
down. The bench was fixed (commit c90ff68) before the full run.

1. **Per-draw buffer sub-updates.** T1's original stream path did one
   64-byte `glBufferSubData` per draw into a 4 MB VBO ring the GPU was
   still reading.
   - That cost **~21 ms per draw**: an 877-draw frame took 18 s. Client
     arrays on the same run cost 6–11 µs per draw.
   - The device then stopped answering and rebooted. The likely cause is
     the driver copying or reallocating the whole buffer on each
     sub-update. It is inferred, not confirmed: Knulli kept no kernel log.
   - Fix: the frame's vertices go up in one orphaning `glBufferData`, then
     the draws use offsets. Streamed draws now cost the same as client
     arrays.
2. **GPU timer queries around framebuffer copies.**
   `GL_EXT_disjoint_timer_query` around frames that call
   `glCopyTexSubImage2D` raised `GL_OUT_OF_MEMORY`, and the frame's GPU
   work was dropped.
   - The first quick pass therefore reported 0.8–1.2 Gpx/s for the copy
     "ping-pong" blend, which was really aborted work.
   - Isolated by experiment with debug switches: ping-pong alone with
     timers fails, ping-pong alone without timers is clean, and the
     gameplay mix alone is clean either way. The errors in other variants
     were fallout from ping-pong runs before them.
   - Fix: ping-pong is timed by wall clock only, and T2 / T4 now record GL
     errors per variant (`"gl_error"` in the results). The full run has
     **no GL errors in any variant**.

### B3. Results against the thresholds

Medians of 3 repetitions at 1.512 GHz (performance governor). Thresholds
are from [PHASE_2_DEVICE_BENCH.md](PHASE_2_DEVICE_BENCH.md).

| Test | Threshold | Measured | Result |
|---|---|---|---|
| **T1** draws, Part B mix at 877 / frame | ≤ 11 µs per draw (≤ 10 ms of 33.4) | client 8.95 µs, stream 7.99 µs, static VBO 7.91 µs, stream + GS vertex conversion 8.09–8.30 µs; **7.0–7.9 ms per frame**. Flat up to 4,000 draws (9.1 µs) | **pass** |
| T1, PAL margin | ≤ 5.7 µs (if draws double) | 7.9–9.0 µs | **not met**: a doubled count needs ~1.5× batching |
| **T2** fill, gameplay mix on the 640×448 RT | ≥ 136 Mpx/s (≥ 221 for the NTSC max) | 1,148 Mpx/s wall / 1,468 GPU at ×8 layers; 305 / 438 at ×1. Gameplay p95 uses **8–9 % of GPU time** at realistic overdraw, 31 % in the ×1 bound | **pass** |
| T2 blending paths | — | fixed-function alpha blend 426–682 Mpx/s; **PS2 blend via framebuffer fetch 160–207 Mpx/s**; copy ping-pong **36–61 Mpx/s** | fbfetch viable, ping-pong not |
| **T3** RT switches + copies + readbacks | ≤ 0.5 ms per switch, ≤ 3 ms total | switch with invalidate / clear **0.10 ms**, keep contents 0.22 ms; 6 switches ≈ 0.6–1.3 ms. Full-frame copy: blit 0.42 ms, `glCopyTexSubImage2D` 1.19 ms. `glReadPixels` full frame 3.1 ms | **pass** (no readbacks in Part B) |
| **T4** upload | hash 1.63 MB + upload ≤ 1.6 MB RGBA8 in ≤ 2 ms | RGBA8 direct: 1.6 MB in 1.58 ms (256²) to 4.6 ms (64²). **R8**: 346–931 MB/s for 128²–512²: 0.4 MB of new indices in 0.4–1.2 ms | RGBA8: **marginal**. R8: **pass** |
| **T5** palette / de-swizzle | worst case in ≤ 2 ms (CPU), or GPU lookup within T2's margin | CPU NEON, 2 threads: PSMT8 6.3–9.7 ms, PSMT4 3.2–4.1 ms (**3–5× over**). **GPU lookup**: 11.6 Gpx/s vs 11.8 plain RGBA8 opaque; 3.2 vs 7.0 Gpx/s blended | CPU: **fail**. GPU: **pass**, picks the GPU path |
| **T6** EE-style kernels, one core | ≥ 295 M guest instr/s (no-go < 150) | Part B mix: **327 M/s** (register-struct), **470 M/s** (register-local); 0.38–0.54 core at 5.9 M instr / frame | **pass** (synthetic kernels) |
| **T7** VU1-style kernel, NEON | ≤ 126 ns / vertex (≤ 0.5 core); PAL margin ≤ 63 | **unlit 49 ns** (0.20 core), **lit 116 ns** (0.46 core); scalar lit 227 ns | **pass** with NEON; PAL margin only unlit |
| **T8** memory | ≥ 400 MB free | 825 MB | **pass** |
| T8 SD | ≫ 0.73 MB/s | 365 MB/s sequential, 19 MB/s random 32 KiB | **pass** |
| T8 audio under 4-core load | 0 underruns | 512-frame buffers: **0** late callbacks; 1024: 1 (33 ms gap) | **pass at 512** |
| T8 heat, 600 s of 4-core load | no throttling below 1.5 GHz | started at 84 °C; **1.416 GHz from 15 s on, steady to the end** at 82–88 °C | **miss by 6 %** |

**No line misses by more than 2×** without a passing alternative. The
biggest miss, T5's CPU path, has a GPU path that passes. So per the spec
nothing reopens Phase 1 Part C. It gets an update instead (PHASE_1 C8).

### B4. The budget with device numbers (gameplay p95, 30 fps)

At the sustained 1.416 GHz (device numbers × 1.07). Phase 1's estimates
are shown for comparison.

| Line | Measured basis | Core-equivalents | Phase 1 (central / pessimistic) |
|---|---|---|---|
| EE, recompiled | T6 mix, 3.3–5.9 M instr / frame | **0.22–0.58** | 0.26 / 0.71 |
| VU1 as native NEON (HLE) | T7, 132.5k vertices | **0.21 (unlit) – 0.49 (lit)** | 0.25 / 1.10 (translated) |
| GS front end + GL submission | T1, 877 draws incl. GS vertex conversion | **0.23–0.25** | 0.33 / 0.62 |
| Texture uploads (R8, 25 % new) | T4 | **0.01–0.04** | in the line above |
| IOP + SPU2 + OS | not measured (audio callback: 0.63 ms per 10.7 ms) | 0.10–0.25 (estimate) | 0.15 / 0.25 |
| **CPU total, of 4 cores** | | **≈ 0.8–1.6** | ≈ 1.0 / 2.7 |
| GPU, gameplay fill | T2 | **≈ 9 %** (31 % bound) | ≈ 25 % / 38 % |

### B5. Rules for the Phase 3 renderer (from the device)

1. **Never sub-update a buffer per draw.** Upload a frame's vertices in
   one orphaning call, or use client arrays (B2.1).
2. **No GPU timer queries around framebuffer copies** on this driver
   (B2.2). Profile those passes by wall clock.
3. **PS2 blend modes GLES can't express: use `GL_ARM_shader_framebuffer_fetch`**
   (160–207 Mpx/s). Copy ping-pong (36–61 Mpx/s) is a last resort for
   rare draws. Everything else uses fixed-function blending.
4. **Textures as R8 indices plus a CLUT texture, looked up in the
   shader.**
   - CPU conversion to RGBA8 is 3–5× over budget in the worst case.
   - A content-addressed texture cache is still needed. Its hash cost
     isn't measured yet.
   - Where the PS2 de-swizzle runs (shader or CPU, indices only) is a
     Phase 3 decision; it wasn't measured on its own.
5. **Prefer invalidating or clearing render targets on switch** (0.10 ms)
   over keeping contents (0.22 ms). Prefer `glBlitFramebuffer` over
   `glCopyTexSubImage2D` for copies.
6. **Never read the framebuffer back per frame** (3.1 ms stall).
7. **Audio with 512-frame buffers.**
8. **Budget the CPU at 1.416 GHz**, the sustained clock, not 1.512.
9. **Batching headroom for PAL:** 877 draws cost 7–8 ms. At the measured
   cost, about 1,100–1,250 draws fit in 10 ms. A doubled draw count in
   PAL scenes would need ~1.5× batching.

### B6. Limits: what this doesn't establish

- **T6 runs synthetic kernels, and T7 a native NEON kernel** (A5). Real
  translated EE code and translated VU1 microcode need the Phase 3
  prototype.
- **T1 is a statistical model of the draw stream,** not a replay of a
  real GS dump.
- **One device, one firmware, one driver:** RG40XX H, Knulli, Mali r20p0,
  fbdev. RG35XX models share the SoC but not the case, so they may
  throttle differently. A Panfrost-based firmware such as ROCKNIX would
  behave differently on the GPU side.
- **The GPU clock was not pinned or recorded** (devfreq 240–648 MHz).
- **The PAL scenes are still unmeasured** (PHASE_1 C5). The draw count
  margin (rule 9) is where they could matter most.

### B7. Recommendation (for review)

**Phase 2 is complete.** Every line that Phase 1 Part C made a condition
(T1, T2, T5, T6, T7) passes on the device, with the GPU path for T5. My
recommendation is to **lift the Phase 1 verdict from "conditional go" to
"go for Phase 3"**, with conditions:
1. Capture PAL S03, S05 and S06 before the renderer design is frozen.
2. Build the renderer to the rules in B5.
3. Have the Phase 3 prototype confirm T6 / T7 with real translated code.

Stopping here for your review.
