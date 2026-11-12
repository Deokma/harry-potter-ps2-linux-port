# Phase 9, GS worker: work per frame on the graphics side (2026-11-12, PC only)

Goal of the effort: Ollivanders gameplay frames from 55-67 ms toward 40 ms on the device. This worker's part: VIF1, VU1, the GIF
arbiter (stage A, `GsThread`), GS parsing and recording (stage B, `GsDraw`) and the GLES backend (the main thread's GL work).
**Everything here was measured on the PC (x86, WSL, Mesa llvmpipe for GL). No device runs: device timings are not known.** Gains
are given as work counters, which carry over to the device; PC times only where marked.

**In short:** two validated changes with an identical picture. (1) Strip vertices that stage B parses register by register: -69 %
per kick in the game's shop (PC run), -31 to -75 % on the dumps. (2) Draw calls: -8 to -19 % on the PCSX2 dumps, but about 0.1 % in
the game's own shop frames. Stage B's PC time did not move measurably; whether the A53 gains (where parsing was 25-31 % of the GS
path's samples) needs a device run. The draw-call lever that remains is reordering the per-channel lightmap passes (below).

## What was measured (PC)

- **gsplay on the GS dumps** (`scripts/gs_gsplay_set.sh TAG cpu|gles`, frames and counters per dump; `scripts/gs_gsplay_cmp.sh A B`
  compares the pictures in numbers with `scripts/gs_ppm_diff.py`). Dumps: N02_0-3 (Ollivanders, N02_1 is the PCSX2 reference of the
  shop), I01_1 (street), F01_1 (menu), N01_1, N03_1.
- **Why runs break** (a temporary counter in `beginRun`, N02_1, consecutive draw runs): 686 differ only in texture and colour mask
  (the lightmap x texture passes, one colour channel at a time), 484 in program, texture, sampler, uniforms and blend (base pass
  against light pass), **148 in nothing the GL thread reads** (another TEX0 address resolving to the same texture, a CLUT reload with
  the same palette, registers the GL path ignores), the rest scattered.
- **Which strip vertices reach the backend straight from the packet** (`PACKED ST RGBAQ XYZF2` tristrips, the VU1 output; N02_1,
  231,920 vertex kicks): only 105,272 (45 %). Of the rest, 74,912 were in flat strips (IIP 0), which the backend refused, and
  46,096 in packets that start with a state change (the frontend's "dirty" flag), which went through the registers as a whole.
  Through the registers each vertex costs three `writeRegisterPacked` calls, a `vertexKick`, and per triangle a
  `SubmitSameState` -> `appendPrimitive` with its lambda; straight from the packet it is one tight loop.
- **gsplay CPU samples** (`scripts/gs_gsplay_prof.sh`, 1 sample per ms of CPU time, `scripts/gs_sample_syms.py` for PIE builds):
  in gsplay most samples are Mesa's software rasterizer and, per replay loop, texture decode and render-target creation that the
  game does not repeat each frame (gsplay resets its caches every loop), so its wall times are not a stage B measure.
- **vu1play** (`private/vu1/vu1play-x86 private/vu1/slot2 --mode check --passes 2`): 0 of 2,506 calls differ in GIF packets (83 in
  data memory, as documented in PHASE_4 M4). Native VU1 4.2-4.9 ms a frame on this PC (noisy: four workers on the machine).
  `private/vu1/vu1_gen.inc` was stale (an older `eg_vu1_kick` signature) and vu1play no longer linked against the current runtime;
  both fixed (regenerated from `micro.bin`, identical to `build-m2/gen/vu1_gen.inc`; `g_ps2portEeBlockedNs` defined in vu1play).

## What changed

1. **Run merging in the GLES backend** (`gsgles/gs_gles_backend.cpp`, `beginRun`). A new run whose GL state equals the previous
   draw's in every field `executeCommands` reads (target, depth, programs, textures, sampler, uniforms, blend, mask, depth test,
   scissor, mode) and whose vertices follow on continues that draw; an empty run is replaced instead of kept. Same GL calls in the
   same order, one draw call fewer. `GSGLES_MERGE=0` turns it off.
2. **Flat strips straight from the packet** (`SubmitPackedStrip`): each triangle takes colour and fog from its last vertex, as
   `appendPrimitive` does for IIP 0. `GSGLES_FLAT_STRIPS=0` turns it off.
3. **Strips after a state change or carried over from the packet before** (private runtime, `gs_frontend.cpp`,
   `processGIFPacket`): the fast path is tried at the packet's start and again at each vertex until taken (at most two refusals a
   packet), so a packet whose first triangle went through the registers (which records the new state) hands the rest over with the
   frontend's two queued vertices; the backend continues the strip from them (`SubmitPackedStrip`'s `queue` argument, unused
   before). The bookkeeping moved into `GS::ps2portStripTaken` (a member function; no data member, layout unchanged).
   `PS2PORT_GS_STRIP_RETRY=0` restores the old rule (start of packet, nothing queued). New counter `g_ps2portStripVertices`.
   The software backend (`GSCpuBackend`, the PC runner without `PS2PORT_DISPLAY`) never takes strips; for it the change is at
   most two refused virtual calls a packet.

## Before / after (PC; work counters first)

Draw calls (GL passes) per dump, gsplay GLES, all loops of one replay:

| Dump | Before | After | Change |
|---|---|---|---|
| N02_1 (shop, PCSX2 reference) | 1,470 | 1,322 | -10.1 % |
| N02_2 | 1,038 | 946 | -8.9 % |
| N02_3 | 786 | 700 | -10.9 % |
| N02_0 (128 vsyncs) | 59,072 | 54,144 | -8.3 % |
| I01_1 (street) | 814 | 716 | -12.0 % |
| N01_1 | 354 | 288 | -18.6 % |
| N03_1 | 3,668 | 3,360 | -8.4 % |
| F01_1 (menu) | 9 | 9 | 0 |

Vertices sent to GL, texture uploads, render-target binds and program binds are unchanged (same vertices, same textures).

Strip vertices through the registers (stage B's slow path), N02_1: **126,648 -> 36,060 of 231,920 kicks (-72 %)**; straight from
the packet 105,272 -> 195,860 (45 % -> 84 %). Other dumps (register-path vertices): N02_2 114,916 -> 44,012 (-62 %), I01_1
119,680 -> 82,574 (-31 %), N03_1 281,808 -> 69,051 (-75 %).

**In the game** (PC runner through the GLES backend on Mesa, `scripts/gs_runner_gles.sh 300 PS2PORT_SLOWDOWN=60
PS2PORT_GS_STRIP_LOG=1`, same binary with the three switches off, then on; shop from 240 s):

| | Switches off (old behaviour) | On |
|---|---|---|
| Strip vertices through the registers, whole run | 48.7 % of 356.5 M kicks | **15.2 % of 339.7 M kicks (-69 % per kick)** |
| Draw calls per game frame (shop) | 1,672 | 1,687 (runs 1,688: about 1 merge a frame) |
| Vertices per game frame | 267,972 | 270,550 |
| Stage B busy per game frame (PC ms, noisy) | 16.8 | 16.4 |
| Play mode reached | 230.7 s | 227.8 s |
| Frame stats of the captured shop frames | lit 84 %, mean 52 | lit 84 %, mean 52 |

**Run merging does almost nothing in the game's own frames** (about 1-2 of ~1,650 draws a frame, against 8-19 % on the PCSX2
dumps): the game's shop frames differ from the dumps (more runs, ~1,650 a frame against ~735), and the runs that break are the
lightmap passes and base/light alternations, which differ in real GL state. The strip change does carry over (-69 % register-path
vertices). Stage B's PC time does not move measurably: on this PC a register-path vertex is cheap and the machine is loaded; on the
A53 the share was larger (below), which only a device run can show.

Stage B CPU (PC, gsplay N02_1, 400 replays, samples in the parsing and recording functions): 606 -> 539 (-11 %, about +-4 % from
sample counts alone). On the device parsing and vertices were 25-31 % of the GS path's samples (PHASE_4 M4), so the share there
should be larger, but that is an estimate, not a measurement.

## Validation (PC)

- **Pictures:** GLES before vs after on all 8 dumps: 0 pixels differ. CPU backend before vs after (frontend change): 0 pixels
  differ on all 8 dumps.
- **vu1play check:** 0 of 2,506 calls differ in GIF packets (no vu1gen change; harness check not needed).
- **Runner, software GS** (`runner/try_sound.sh 240 NAME PS2PORT_SLOWDOWN=60`): unmodified sandbox build (`runner.base`, md5
  `bc15193c`) reached play (mode 5) at 224.0 s; this build at 225.2 s. Mixing in play 47.2-48.2 k of 48 k frames/s in both (whole
  run 40,396 / 40,531 k).
- **Runner, GLES backend** (`scripts/gs_runner_gles.sh`): unmodified build reached play at 227.7 s, captured shop frames lit 84 %,
  mean 52; this build 236.7 s (switches on) and 227.8 s (second run), the same 84 % / 52. Arrival differs by run (real-time
  sound and IOP), not by build.

## Commands (WSL, repo root)

```
sh gsgles/build.sh gles && sh scripts/gs_gsplay_set.sh after gles && sh scripts/gs_gsplay_cmp.sh before after
sh scripts/gs_gsplay_prof.sh N02_1 400 "PS2PORT_GS_STRIP_RETRY=0 GSGLES_FLAT_STRIPS=0 GSGLES_MERGE=0" ""
sh scripts/gs_runner_gles.sh 300 PS2PORT_SLOWDOWN=60 PS2PORT_GS_STRIP_LOG=1
```

## Tried and dropped

- **Indexed draws** (strip vertices stored once, triangles as 32-bit indices; `glDrawElements` through the context's loader): on
  the dumps the vertex plus index bytes to GL went **up** (N02_1 6.65 -> 7.70 MB), because most strip packets are short and many
  triangles are cut by ADC bits, and the register path's triangles then needed indices too. Reverted.
- PC wall-time A/B of gsplay's packet phase (`scripts/gs_gsplay_ab.sh`): spread 30-63 ms between runs of the same binary with
  four workers on the machine; not usable. Replaced by CPU-time samples and counters.
- vu1gen code: the hot loops (`0x6ec`-`0x6f9`, the light attenuation) are already straight vector code; no change found that
  could be shown on the PC with the noise here. Not attempted.

## What would go further (proposals, estimates)

- **The lightmap passes** (686 of N02_1's run breaks): one colour channel at a time with its own texture and colour mask, object by
  object (R, G, B, next object). The passes of different channels write disjoint channels, so all R passes of a stretch could be
  drawn together, then G, then B: about two thirds of those draw calls gone (estimate). Safe only where the passes have no depth
  writes and the same blend, which needs checking per stretch; not done.
- **Uniform caching for the region uniforms** (`usePass` sends `u_region`/`u_regionMode` on every pass of a region program).
- **Stage A:** VU1 translated code is still the largest item on the device (PHASE_7 step 3: 59 % of stage A). Nothing measurable
  on the PC was found here in this session.
- For other workers: none of the changes needs another worker's files.

## Risks

- Run merging compares every field `executeCommands` reads today; a new field added to `Command` must be added to `sameGlDraw`, or
  two runs differing only in it would merge wrongly.
- The queued-pair continuation relies on (x - ofx) + 0.5 == x + (0.5 - ofx) for GS coordinates (multiples of 1/16 below 4096,
  exact in a float); identical pictures on the 8 dumps support it.
- Not run on the device: the Mali driver's cost per draw call and the A53's cost per register-path vertex are what turn these
  counters into milliseconds there.
