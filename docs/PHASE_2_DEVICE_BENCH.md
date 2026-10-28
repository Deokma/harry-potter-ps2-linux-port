# On-device benchmark spec (Phase 2, run on the device's first day)

**Purpose:** replace Phase 1's extrapolations with measurements on the real
RG35XX (H700: 4× Cortex-A53 @ 1.5 GHz, Mali-G31 MP2, 1 GB). Each test maps
to one budget line of the Phase 1 verdict (Part C). The pass threshold is
written against the Part B numbers (p95 / max per scene). The thresholds
were filled in from Part C on 2026-10-27, from **NTSC data only**. The "PAL
margin" figures cover the 2× growth that PHASE_1 C5 allows for the
unmeasured PAL scenes.

Phase 2 builds this as `bench/` (C11, SDL2 + GLES 3.x, CMake,
aarch64 cross-build), pushed and run with `scripts/deploy.sh` /
`scripts/run_remote.sh`. **Implemented and verified off the device on
2026-10-28.** [PHASE_2.md](PHASE_2.md) covers the design decisions, what
was verified, and the day-one procedure. On the PC, qemu-user only proves the CPU tests
(T5-CPU, T6, T7) run and give correct results. **No timing from qemu
counts**, and GLES tests can't run there at all.

## Run conditions (recorded in every result file)

- Firmware (muOS / Knulli / ROCKNIX + version), kernel; GL driver
  (`GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, full extension list): the Mali
  blob and Panfrost differ a lot.
- CPU governor (set `performance` if allowed), min/max/current frequency,
  SoC temperature before and after each test.
- Every test runs **3×**, reporting the median plus min/max, after 10 s of
  warm-up. Wi-Fi off. Screen at native 640×480.
- Timing:
  - CPU: `CLOCK_MONOTONIC`.
  - GPU: `EXT_disjoint_timer_query` if exposed. Otherwise wall time over
    N frames with swap interval 0 and a `glFinish` fence.
  - Plus one vsync-on run for real frame pacing.
- Output: `bench/results/<date>-<firmware>.json` (+ CSV), fetched by
  `run_remote.sh`.

## Tests

| ID | What | Variants | Reports | Pass if (vs Part B / C) |
|---|---|---|---|---|
| **T1** | **Draw-call rate** (CPU submission cost) | N = 100…4000 tiny quads/frame (GPU idle). Per draw: (a) no state change, (b) uniform update, (c) texture bind (pool of 64), (d) blend-func change, (e) program change (8 programs), (f) FBO switch every 50 draws. Vertex paths: client arrays, streaming VBO ring, one big VBO + offsets. 1 vs 2 threads (game thread + render thread) | µs per draw per variant; fps | p95 draws/frame × µs/draw ≤ **10 ms** of the 33.3 ms frame on the render core. Part B p95 is 877 draws (max 887): 82 % textured, 74 % blended, 6 FBO switches and ≈1,070 CLUT loads per frame. That means **≤ 11 µs per draw** at that mix; PAL margin ≤ 5.7 µs. No-go territory: > 22 µs, unless batching halves the draw count |
| **T2** | **Fill and blending at 640×480** (and a 640×448 offscreen RT) | full-screen quads ×1/2/4/8/16: opaque untextured; textured RGBA8 256² nearest / bilinear; fixed-function alpha blend; additive; depth test on/off (D24, D16). **PS2-blend emulation** for the draws Part B flags as non-fixed: (i) framebuffer fetch (`EXT/ARM_shader_framebuffer_fetch`) if exposed, (ii) copy-RT-to-texture ping-pong | Mpix/s per mode; overdraw budget at 30 fps = Mpix/s ÷ (640×480×30) | p95 panel-scaled fill ≤ **70 %** of measured throughput, at Part B's mix (≈75 % textured, 33–59 % blended, ≈90 % depth-tested, palettized textures, no mipmaps). Gameplay p95 is 3.17 M px/frame (95 Mpx/s), so it needs **≥ 136 Mpx/s**. The NTSC max is 5.17 M (155 Mpx/s), so it needs ≥ 221 Mpx/s. Also record whether framebuffer fetch is exposed |
| **T3** | **Render targets and readback** | FBO bind (+ clear / no clear); RT→texture copy (`glCopyTexSubImage2D`, `glBlitFramebuffer`) 64²…640×448; `glReadPixels` 4 KB and full frame | µs per switch / copy; MB/s; readback stall ms | Part B p95 RT switches + copies + readbacks per frame fit in **3 ms**. NTSC p95 is 6 switches, 0 copies, 0 readbacks, so **≤ 0.5 ms per switch**. Any per-frame readback must be avoidable by design |
| **T4** | **Texture upload** | `glTexSubImage2D` RGBA8 32²…512², direct vs PBO; R8 index textures | µs per call, MB/s | p95 upload bytes/frame (as RGBA8) ≤ **2 ms**. Part B p95 is 1.06 MB native (max 1.63 MB), ≈4–6.5 MB as RGBA8. Re-uploading all of it every frame would need ≥ 3.3 GB/s, which is not expected. So the threshold applies to a content-hash cache: hashing 1.63 MB plus uploading the new share (assume ≤ 25 %, ≤ 1.6 MB RGBA8) must fit in 2 ms |
| **T5** | **Palette and de-swizzle conversion** | CPU: PS2-swizzled PSMT8 / PSMT4 + CLUT (CSM1, 32-bit) → linear RGBA8, scalar vs NEON, 1–2 cores. GPU: R8 index texture + 256×1 palette texture lookup in the fragment shader (fill cost vs T2 plain RGBA8) | CPU MB/s per core; GPU Mpix/s with lookup | p95 *new* texture+CLUT bytes per frame converted in **2 ms** (CPU path), or the GPU-lookup fill cost stays within T2's margin. Picks the path. Worst case: all 1.63 MB new (≈1.6 M T8/T4 texels) plus ≈1,070 CLUT loads per frame |
| **T6** | **Recompiled EE code** | Real game functions picked in Phase 3 (one FPU-heavy, one memory-copy, one typical game-logic function), recompiled to C by the prototype, `-O2` for aarch64, driven by recorded inputs; plus synthetic kernels: integer / branch mix, FPU with flush-to-zero and round-toward-zero set in FPCR, 128-bit `lq`/`sq` moves, guest memory access through the address-translation path | guest instructions per host second on one A53; expansion (host / guest instructions, via `perf stat` if available) | Phase 3's guest-instructions-per-frame estimate ÷ measured rate ≤ **60 %** of one core at 30 fps. Until Phase 3, use the Phase 1 estimate of 3.3–5.9 M guest instructions per gameplay frame: that needs **≥ 295 M guest instructions/s** on one A53 (≈5 host cycles each). No-go territory: < 150 M/s |
| **T7** | **VU1-style transform kernel (NEON)** | N vertices: 4×4 transform, perspective divide, clip flags, pack to a GS-like vertex; variant with 3-light lighting | ns and cycles per vertex, 1 and 2 cores | p95 vertices/frame (Part B) × ns/vertex ≤ **0.5 core**. Part B p95 is 132.5k vertices (max 135k), so **≤ 126 ns (≈190 cycles) per vertex**; PAL margin ≤ 63 ns. For translated microcode, compare with VU1's 8–28 VU cycles per vertex (PHASE_1 C3.2): ≤ 0.5 core (VIF unpack included) means about ≤ 12 A53 cycles per VU cycle at the central 13 VU cycles per vertex |
| **T8** | **System** | free RAM after boot (`free -m`); SD card sequential and random read; SDL2 audio at 48 kHz stereo, 512 / 1024-frame buffers, underruns over 60 s while T6 loads all 4 cores; 10-minute 4-core load for thermal throttling | MB, MB/s, underruns, frequency over time | ≥ **400 MB** free (Part C estimates ≈120–260 MB in use). Disc streaming ≪ SD read rate: A01 measured 85 kB/s mean and 0.73 MB/s peak. 0 underruns. No throttling below **1.5 GHz** (CPU) and the GPU clock T2 ran at |

## Day-one order (≈30–40 min)

T8 system info → T1 → T2 → T4 / T5 → T3 → T6 / T7 → T8 thermal. The first
three are the go / no-go-critical ones: draw rate, fill / blend, upload.

## What the results decide

- **T1 + T2 + T3** decide the renderer design:
  - how aggressively draws must be batched;
  - whether non-fixed blends go through framebuffer fetch, ping-pong copies,
    or approximations;
  - whether we render at 640×448 or lower.
- **T4 + T5** decide CPU vs GPU texture conversion.
- **T6 + T7** decide how many cores the EE and VU1 paths may use, and
  whether VU1 needs native (HLE) replacements of its microprograms instead
  of translation.
- If any critical test misses its threshold by more than 2×, Part C's
  verdict is revisited before Phase 3 work continues.
