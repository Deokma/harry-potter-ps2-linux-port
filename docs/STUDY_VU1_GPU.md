# Study: VU1 on the GPU (2026-11-09, study only, nothing implemented)

Question (user, 2026-11-09): could the VU1 microprograms run as GLES vertex (or compute) shaders on the Mali-G31, and what
would it save in stage A (VIF1 + VU1, the limit today) and stage B (GS parsing)? Numbers are marked **measured** (with
source) or **estimate**.

## What VU1 does in this game (measured on the saved frames `slot2+2`, gameplay, and `i01+5`, street)

- Micro memory holds one image with **8 entries** (a jump table at 0x000-0x017): entry 0 -> 0x7dd, 1 -> 0x731, 2 -> 0x769,
  3 -> 0x7cf, 4 -> 0x018, 5 -> 0x072, 6 -> 0x78f, 7 -> 0x1bf. Shared subroutines (BAL): 0x672, 0x5cf, 0x5f5, 0x6b0.
- **Calls per entry** (MSCAL targets in the VIF1 stream; gameplay / street):

  | Entry (target) | Calls | Vectors unpacked before the calls | Role (from the code; inference) |
  |---|---|---|---|
  | 7 (0x1bf) | 2,085 / 1,706 | 368,623 / 289,462 (94 % / 92 %) | the mesh program: transform, lighting, clip, XGKICK |
  | 0 (0x7dd) | 271 / 246 | 11,430 / 11,154 | small batches (setup or sprites) |
  | 3 (0x7cf) | 92 / 134 | 909 / 1,210 | tiny batches |
  | 5 (0x072) | 43 / 65 | 8,131 / 12,545 | second mesh path (calls the same subroutines) |
  | 4 (0x018) | 9 / 0 | 1,005 / 0 | rare |
  | 1 (0x731) | 6 / 2 | 150 / 50 | rare |
  | 2, 6 | 0 / 0 | | not seen in these frames |

- **Where the pairs go** (gameplay, `vu1_hot.py`, 6.32 M pairs): **0x65b-0x730 45.5 %** (per-vertex loops: 4x4 matrix
  transform of positions, two vertices per iteration; normals ITOF x 1/127 against a light matrix at VU memory 32-34),
  **0x301-0x4f3 45.1 %** (per-vertex/triangle loop with CLIP, DIV/SQRT, FTOI, the XGKICKs at 0x45a-0x46c), 0x5cd-0x5f4 5.6 %,
  everything else 3.8 %. Operation mix: LQ 23 % / SQ 16 % of lower ops; MUL/MADD/ADD/MAX/MINI dominate upper ops.
- **Input formats** (VIF1 UNPACKs, gameplay, 390,248 vectors): V2-32 masked 20 % (UVs), V3-32 masked 17 % (positions),
  V4-8 signed 17 % (normals), V4-8 unsigned 26 % (colours), V1-8 masked 8 %, V4-32 13 % (matrices, headers). Almost all
  `cl4 wl1` (interleaved vertex records in VU memory).
- **Output:** GIF packets through XGKICK (PATH1): 4,965 packets, 6.31 MB, 134,974 vertices a gameplay frame (PHASE_4, M4);
  **99.3 % of vertices are PACKED `ST RGBAQ XYZF2`** (PHASE_4, M3). Strips with ADC bits, Q = 1/w for perspective textures.

## Costs today (measured)

- Stage A (GsThread) in the shop: 43.2 ms in jobs of a 45 ms frame, the limit (PHASE_7 step 3). VU1 translated: 28.3 ms on
  the replay frame (`slot2+2`, one core, PHASE_4 M4), about 20-22 ms in the runner's frames; VIF fast path 10.6 ms (replay).
- Stage B (GsDraw): GS parsing and recording, about 18 ms of a frame (43 % of the old single GS thread; PHASE_4 thread split).

## How it would map to GLES

Mali-G31, GLES 3.2 (vertex, fragment and compute shaders; transform feedback). Two shapes:

1. **Vertex shader per program (entry 7 first).** Each MSCAL batch becomes a draw: vertex attributes straight from the VIF
   data (V3-32 -> `vec3`, V2-32 -> `vec2`, V4-8 -> normalised `GL_BYTE`/`GL_UNSIGNED_BYTE`), per-batch constants (matrices,
   lights: the V4-32 headers) in a uniform or storage buffer indexed by a batch id. The shader does the 4x4 transform,
   lighting, perspective (Q), the GS's screen mapping (XYZF2 12.4 fixed point, the -2048 offset, 24-bit Z) and fog. The CPU
   keeps the per-batch control code (headers, GIF tags: PRIM, register list, texture state), about 4-10 % of the pairs.
2. **Compute shader running the whole program per batch**, writing vertices and a per-triangle keep/drop flag to a buffer,
   then one draw. Closer to VU1 semantics (the per-triangle clip/reject branches), more work in shader code.

## Estimated savings (estimates, from the measured shares above)

| | Today (measured) | With entry 7 on the GPU (estimate) |
|---|---|---|
| Stage A: VU1 | ~20-22 ms | ~2 ms (control code of all entries, entries 0-6 stay on the CPU) |
| Stage A: VIF unpack | ~8-10 ms | ~3 ms (copy raw vertex data into a GL buffer; unpack in the shader) |
| Stage B: GS parsing of PATH1 vertices | ~18 ms | ~5-8 ms (only per-batch state, texture uploads, PATH3) |
| GL thread: draw calls | batches by state | **+2,000 draws a frame unless merged**: at 10-20 us each in Mali's driver (estimate) that is 20-40 ms, so batches must be merged (batch id attribute + storage buffer) |
| GPU vertex work | small | ~135k vertices a frame, a few ms at 240 MHz (estimate, not measured); the GPU clock may have to be raised (power) |

If the merging works, stage A could drop from ~43 ms to ~15-20 ms and stage B by ~10 ms (estimate). The game thread
(~40 ms of work, PHASE_7) would then be the limit, so **the frame would not go below the game thread's time**: the gain is
capped near 43.5 -> ~38-40 ms until the game thread is also cut.

## Risks

1. **Per-triangle rejection.** The mesh loop reads clip flags and branches per triangle (rejects triangles outside the
   guard band, sets ADC). A vertex shader sees one vertex: rejection needs the compute shape, or degenerate positions
   plus a precomputed triangle pass. Wrong rejection means spikes across the screen.
2. **Floating point.** VU floats clamp (no Inf/NaN/denormals) and round toward zero; GLES `highp` is IEEE with round to
   nearest. Positions differ by fractions of a 1/16 pixel, colours by 1 after FTOI; Z equality tests (decals, multi-pass)
   can break. Byte-exact output is not possible.
3. **Ordering.** VU1 draws must stay in order with PATH3 texture uploads and GS register writes; stage B's command list
   already keeps order, but a draw would now refer to GPU buffers written earlier in the frame (lifetime, double buffering).
4. **Driver overhead and the tiler.** Many small draws are slow on Mali; merging needs a storage buffer in the vertex
   shader (supported on Bifrost, to be checked on this driver) or texture buffers.
5. **Power and heat.** Work moves from an A53 at 1.416 GHz to a GPU at a higher clock; the net heat change is unknown and
   heat is the session limit today (item 3).
6. **Debugging.** The VU1 programs become two implementations (translated C++ for reference and fallback, shaders).

## Test coverage to redo

- **Kept:** the EE harness (RDRAM byte for byte), VIF fast path checks, the translated VU1 as the CPU reference and
  fallback, gsplay on GS dumps for everything that is not PATH1.
- **Lost for entry 7:** `vu1play --mode check` (GIF packets byte for byte against the interpreter: 0 of 2,506 calls differ
  today). It has to become a tolerance test: transform feedback of the shader's outputs against the translated code's
  XYZF2/ST/RGBAQ (positions within 1/16 pixel, colours within 1), plus image comparisons with a tolerance on the saved
  frames and the user's scenes. Estimate: about a third of the existing graphics checks would need a new, tolerant form.

## Cheaper alternatives to weigh first (estimates)

- **Direct vertex handoff for PATH1** (translated VU1 writes GL-ready vertices instead of GIF packets; PHASE_4 M4 measured
  about 1 ms of cost and at most 14-17 ms of saving in stage B). Keeps byte-exact VU1 tests.
- **A PACKED `ST RGBAQ XYZF2` fast path in stage B** (one tight loop instead of per-register dispatch): part of the same gain.
- **NEON in the hot loops** of the translated VU1 (0x65b-0x730, 0x301-0x4f3): the 4x4 transforms are four-lane by nature.
- **Entry 7 batches split over two cores** (VU memory copied per batch): about half of VU1's time, for one more busy core (heat).

**Recommendation:** not yet. The game thread caps the gain near 38-40 ms. Do the direct handoff and NEON first (low risk,
exact tests kept), and keep VU1-on-GPU as the option if stage A is still the limit once the game thread is under ~30 ms.
