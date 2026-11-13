# Phase 10 study: what a stable 30 frames a second would take (2026-11-13)

Target (user, 2026-11-13): stable 30 frames a second by any means, as long as the game looks as on the PS2; proposed wording:
"30 where the PS2 ran 30, the same drops where it dropped, at 100 % game speed". Bounded study, nothing implemented.
**Measured** means the device (RG40XX H, 1.416 GHz, binary `7a0e74dc`); everything else is marked as an estimate.

## The measurement

`sh scripts/play.sh 8 PS2PORT_M5_STATS=1 PS2PORT_PAD=CROSS@5+0.3 PS2PORT_PAD_SEQ=9:START:60,2:CROSS:60
PS2PORT_SAMPLE=/userdata/ps2bench/run/ee9.samples PS2PORT_SAMPLE_FROM=330 PS2PORT_SAMPLE_TO=470` (session `play-140841`, New Game
with no further input; logs and samples in `private/play/`). Peak 74.9 C at 8 minutes and still rising, no 85 C stop.
`python3 -I scripts/sample_threads.py private/play/ee9/ee9.samples .../aarch64/runner --nm aarch64-linux-gnu-nm --thread GameThread`.

- The sample window holds three parts: Ollivanders standing (t=331-382, 42 ms a frame), a part at 93-109 ms (t=387-412) and a
  part at 52-73 ms (t=417-470). The shares below are the mix of the three. The shop counter and the street were not reached
  (they need the user's play); 2,745 game-thread samples, so a 1 % entry is +-0.2 %.
- **Frame time (measured):** Ollivanders standing 42.1-42.8 ms, GameThread 89 % of a core: **37.6 ms of EE-thread CPU a frame**.
  A no-input scripted part at t=186-251 holds 63-64 ms.
- **In the slow parts no thread is saturated (measured):** at 93-109 ms a frame GameThread is 43-60 %, GsThread 65-77 %, GsDraw
  55-58 %, main 41-46 %. The EE is not the limit there; the GS pipeline or the GPU is. EE work alone cannot fix those scenes.

## 1. Where the EE thread's time goes (measured shares of GameThread)

| Group | Share |
|---|---|
| eegen guest code | 54.0 % |
| libc | 16.2 % (44.6 % of it called from `ps2portGather`, the DMA chain copy: 7.2 % of the thread; 5 % from `vector::resize`; 32 % inside libc itself, futex wakes by the PC profile) |
| PS2Memory | 10.5 % (`writeIORegister` 5.0, `translateAddress` 1.5, `inRange` 1.0, `write128` 0.7, `read128` 0.5) |
| EE scheduler and runtime | 8.2 % (`publishSnapshot` 0.9, unwinder 0.8, `checkpointDue` 0.5) |
| translated VU0 | 2.9 % |
| IOP stepped from the EE | 1.8 % |
| other | 5.5 % |

Guest functions, cumulative (measured): top 1 5.2 %, **top 10 16.1 %, top 20 24 %, top 50 about 36 %** of the thread; the other
18 % is spread over hundreds of functions. **The profile is flat.** PS2Recomp's code: 0.0 %.

Top 20 (share measured; instructions a frame estimated as share / 54 % x 25.5 M, the PC count; kind from an opcode census of the
function, the game has no symbols):

| # | Guest function | Share | Instr./frame (est.) | Size, shape | Kind |
|---|---|---|---|---|---|
| 1 | `002d7878` | 5.2 % | 2.5 M | 258, 99 % FPU, no loop, no call | matrix product (scalar float) |
| 2 | `002c4fe0` | 1.5 % | 0.7 M | 248, 4 loops | scene traversal / lists |
| 3 | `002a93d8` | 1.5 % | 0.7 M | 284, 32 % FPU | float math with table reads |
| 4 | `002cfa30` | 1.3 % | 0.6 M | 556, 3 loops, loads = stores | copy / pack loop |
| 5 | `002df7f0` | 1.3 % | 0.6 M | 84, VU0 macro + MMI 78 % | vector loop (skinning or transform) |
| 6 | `002d6340` | 1.1 % | 0.5 M | 182, 59 % FPU | matrix / vector math |
| 7 | `002d76c0` | 1.1 % | 0.5 M | 110, 98 % FPU | matrix / vector math |
| 8 | `002dfd90` | 1.1 % | 0.5 M | 100, 3 loops | float table loop |
| 9 | `00248f98` | 1.0 % | 0.5 M | 302, 2 loops | traversal |
| 10 | `002a6258` | 1.0 % | 0.5 M | 452, 5 loops, 30 % FPU | animation / float loop |
| 11 | `002db9a8` | 0.9 % | 0.4 M | 466, integer | other |
| 12 | `002c2528` | 0.9 % | 0.4 M | 180, VU0 macro 44 % | vector / matrix |
| 13 | `002a6968` | 0.8 % | 0.4 M | 170, 58 % FPU | float loop |
| 14 | `002c4cf8` | 0.8 % | 0.4 M | 100, VU0 macro 56 % | vector / matrix |
| 15 | `0018dff8` | 0.8 % | 0.4 M | 116 | other |
| 16 | `002ad3c0` | 0.8 % | 0.4 M | 422, 63 % FPU | float math |
| 17 | `002d8dd8` | 0.8 % | 0.4 M | 66, 65 % FPU | vector math |
| 18 | `002d8418` | 0.8 % | 0.4 M | 46, 95 % FPU | vector math |
| 19 | `002c0b68` | 0.7 % | 0.3 M | 272, 5 loops, 14 % branches | traversal |
| 20 | `002b2030` | 0.7 % | 0.3 M | 696, 62 % FPU | float math |

No particle system and no memcpy/memset guest routine is in the top 20; the copy that shows is the runtime's own
(`ps2portGather`).

## 2. Hand-written native versions of the top 20 (all estimates)

| Set | Functions | Share | Speed-up | Saved | Verification | Effort | Risk |
|---|---|---|---|---|---|---|---|
| Straight-line float | 1, 7, 17, 18 | 7.9 % | 2-4x | 1.9 ms | byte-exact (same order of operations, no fused multiply; harness RDRAM + PCSX2 recordings) | 30 tool calls each | low |
| Float with loops and tables | 3, 6, 10, 13, 16, 20 | 5.9 % | 1.3-2x | 0.7 ms | byte-exact | 40 each | medium |
| VU0 macro | 5, 12, 14 | 3.0 % | 1.5-2.5x | 0.4 ms | byte-exact if the VU flags are kept, else tolerance against PCSX2 | 40 each | medium |
| Integer, traversal | 2, 4, 8, 9, 11, 15, 19 | 7.4 % | 1.1-1.3x | 0.4 ms | byte-exact | 50 each | high (no symbols, branchy) |

All twenty: about 3.4 ms of 37.6 ms for about 800 tool calls. The first four: 1.9 ms for about 120.

## 3. Other levers, ranked by estimated ms saved per tool call

| # | Lever | Saves (EE ms a frame, est.) | Cost | Notes |
|---|---|---|---|---|
| 1 | Clock 1.512 GHz (`CPU_KHZ=1512000`, already in `play.sh`) | 2.5 (every thread 6 % faster) | 1 device run | Heat: +4-6 C by estimate on a run that reaches 75 C at 8 minutes and is still rising; may bring the 85 C stop back. 1.704 GHz is not in the device's clock table. |
| 2 | DMA chain gather without the copy (`ps2portGather`, 65 MB/s) and a fast path in `writeIORegister` / `translateAddress` | 4-5 | 80 | Largest single item; runtime only. Risk: the EE changing a chain after the kick. |
| 3 | Wake workers only when asleep, batch GS jobs (the workers' proposal) | 1-1.5 | 40 | libc's own 5 % |
| 4 | Scheduler: cheaper checkpoints and snapshots, no unwinder on the hot path | 1-1.5 | 50 | |
| 5 | PGO and hot/cold layout of the generated code | 1-2 | 40 | Flat profile over a 93 MB binary: cache and branch misses. The profile must come from an aarch64 run. |
| 6 | Native float routines 1, 7, 17, 18 | 1.9 | 120 | section 2 |
| 7 | Rest of the top 20 | 1.5 | 680 | poor return |
| 8 | Untranslated functions | 0 | - | PS2Recomp's code is 0.0 % already |
| 9 | Trimming the game's own work (particles, distant updates) | unknown | 150+ | No symbols; changes what the PS2 shows; against the target. Not advised. |
| 10 | Faster device class | all of it | 0 code | Cortex-A55 at 1.8-2.0 GHz about 1.6-1.8x, A76 class 4x or more (estimates). |

## 4. Can the EE thread reach 33 ms?

- **Ollivanders standing: yes by estimate.** 37.6 ms now; levers 2-6 give 9-12 ms: **26-28 ms** at 1.416 GHz, 24-26 ms at
  1.512 GHz. The frame then meets the next limit: GsThread is 78 % busy at 42 ms, about 33 ms of work a frame, so stage A needs
  its own cut for a steady 30.
- **Moving in Ollivanders (44-66 ms) and the 63-109 ms parts: not by EE work.** No thread is saturated there; the cause is in
  the GS pipeline or the GPU and was not studied here. A stable 30 everywhere is not shown to be reachable on this device.
- **The shop counter and the street: not measured.** They need one play session by the user with the sampler on.
- **The PS2's own drops:** the recorded Ollivanders frames (`D:\ps2port-data\reports\N02`, 66 frames) are all 2 vsyncs: 30 a
  second, no drop. No street recording exists, so the 20 a second there is not confirmed from recordings (`docs/PHASE_3.md`
  notes one intro shot at 20).
