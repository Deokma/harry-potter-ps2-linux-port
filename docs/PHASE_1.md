# Phase 1: Feasibility analysis

Status: **Part A (static analysis) done. Part B (measurements) approved
2026-10-27 with additions a–d; tooling validated on the pilot capture
(B2a); all NTSC scenes measured (B2b–B2h: N01–N03, I01, F01, A01); the PAL
scenes S01–S08 are deferred (user decision). Part C: conditional go,
written from the NTSC data only (2026-10-27). C8 (2026-11-01): every
condition met on the device. **Verdict upgraded to go for Phase 3,
approved by the user on 2026-11-01**, with the three conditions in C8.**
Primary disc: NTSC-U SLUS-20826. Secondary: PAL SLES-52055. The boot ELF is
the same file on both.

## TL;DR

**Verdict (Part C): conditional go for Phase 2.** It is based on NTSC
data only, at medium-low confidence.
- **Central estimate:** gameplay at the stock 30 fps needs about 1 of the
  A53's 4 cores and about a quarter of the Mali-G31's fill rate.
- **Pessimistic ends:**
  - draw-call submission (≈880 draws per frame) can miss its budget;
  - translated VU1 microcode can take a whole core.

  Both have fallbacks: batching, and HLE of VU1's few hot loops.
- **The verdict depends on:**
  - the Phase 2 device benchmarks T1, T2, T5, T6 and T7;
  - the PAL outdoor, Quidditch and effects scenes (C5), which may raise
    the draw count;
  - those scenes possibly needing framebuffer feedback.

Static analysis says the **EE side is close to a best case for static
recompilation**:
- One static ELF image with no symbols.
- About 5,500–6,000 functions.
- 0 undecodable words in `.text`.
- No self-modifying code and no dynamically loaded code, at least in the one
  savestate checked so far.
- Very little 128-bit MMI or VU0 macro code.
- Only 139 indirect calls.

The hard parts are **outside the EE**:
1. All world geometry goes through **VU1 microcode**: about 2,050
   instructions, 42 `XGKICK`s, permanently resident.
2. Everything VU1 produces must become **GLES draws on a Mali-G31**.
3. The IOP side includes a **custom audio streamer** (`STREAM.IRX`) that has
   to be reimplemented natively.

Whether 2 and 1 fit the budget is what Part B measures. The game appears to
target **30 fps** (2 vsyncs per frame), which halves the per-second budget
compared with 60 fps.

## Part A: static analysis (done)

Tools (all in `scripts/`, output under `private/analysis/SLUS_208.26/`):
`analyze_elf.py`, `analyze_irx.py`, `scan_code_in_data.py`,
`inspect_savestate.py`.

### A1. Disc and dump integrity

- NTSC-U ISO: the first 2,594,635,776 bytes (the ISO9660 volume) match
  **redump MD5 `a1e5106de7a21f9f122219ae06c7acf9`** exactly. The 160 extra
  sectors at the end are all zero. **Clean dump.** (Checked against the
  `RedumpDatabase.yaml` that ships with PCSX2.)
- PAL CHD: decodes cleanly (full-image SHA-1 matches the CHD header). Its
  logical size (4,698,112,000 bytes) differs from redump's (4,206,067,712),
  so the whole-image hash can't be compared. Not checked further: the boot
  ELF is byte-identical to the verified NTSC one.
- PCSX2 `GameIndex.yaml` for both serials: only `gameFixes: EETimingHack`,
  PAL rated compat 5 (perfect). **No GS hardware fixes, no VU/FPU clamp or
  rounding overrides.** That's a good sign: the game doesn't depend on exotic
  FPU/VU edge cases. It does show some sensitivity to EE / DMA / interrupt
  timing.

### A2. ELF layout

| Section | Address | Bytes | Note |
|---|---|---|---|
| `.text` | 0x00100000 | 2,194,660 | 548,665 R5900 instructions |
| `.vutext` | 0x00317CF0 | 19,712 | VU0 + VU1 microcode (see A6) |
| `.data` | 0x0031CA00 | 74,704 | |
| `.rodata` | 0x0032EE00 | 198,520 | 297 jump tables live here |
| `.sbss` / `.bss` | 0x0035F580 | 359,736 | |
| `.DVP.ovlytab` / `.DVP.overlay.*` | (not loaded) | | overlay table + zero-filled placeholders |

- Entry `0x002A2FA8`. One PT_LOAD, 2.85 MB in memory; everything above
  ~3.6 MB of the 32 MB EE RAM is heap / stack / loaded data.
- No `.symtab`, no debug info. Section headers present.
- Built with **SDK 2.7.0** libraries: libmc, libcdvd, libdma, libipu,
  libmpeg, libpad, libscf, libsdr (from `PsII… 2700` version strings). There
  are no libgraph / libvu0 / libpkt strings, so the engine likely drives
  DMA/VIF/GIF with its own code.
- Engine: C++ (`C:/HARRYPOTTER/Game/gameloop.cpp`, `C:/Tusk/tkMemory.cpp`).
  The data archives carry a `WART3.00` magic, which points to Warthog's
  in-house "Tusk" engine.

### A3. EE code: size and instruction mix

| Category | Instructions | Share |
|---|---|---|
| integer ALU (32-bit) | 153,806 | 28.0 % |
| loads/stores (≤64-bit) | 118,143 | 21.5 % |
| **FPU (COP1, incl. lwc1/swc1)** | 100,695 | **18.4 %** |
| jumps (j/jal/jr/jalr) | 43,347 | 7.9 % |
| integer 64-bit ops (`daddu` moves, `dsll`…) | 43,140 | 7.9 % |
| branches | 35,939 | 6.6 % |
| 128-bit `lq`/`sq` | 27,775 | 5.1 % |
| nop | 21,689 | 4.0 % |
| mul/div | 2,398 | 0.4 % |
| MMI 128-bit parallel ops | 479 | 0.09 % |
| VU0 macro ops / COP2 transfers / `vcallms` | 308 / 393 / 12 | 0.13 % |
| COP0, syscall, sync, cache, break | 541 | 0.1 % |
| **undecodable** | **0** | 0 % |

What this means for the recompiler:
- **Mostly plain scalar MIPS.** The R5900-specific parts the recompiler must
  handle are 128-bit GPRs (needed for `lq`/`sq`, which GCC uses to
  save/restore registers) and a small set of MMI ops (`qfsrv`, `pcpyld`,
  `pcpyud`, `pand`, `por`, `madd`, `mult1`). VU0 macro mode is barely used.
- **FPU-heavy (18 %).** The PS2 FPU isn't IEEE: no denormals, no inf/NaN
  (it clamps), and it rounds toward zero. On AArch64 we can set
  flush-to-zero and round-toward-zero globally, which is cheap. Clamping
  would cost extra and is only needed where the game relies on it.
  GameIndex lists no clamp fix for this game, so plan for no clamping.
- 5,365 branch-likely instructions (annulled delay slots). That's standard
  for a MIPS recompiler.

### A4. Functions and control flow

| Measure | Value |
|---|---|
| Distinct `jal` targets | 4,149 |
| `addiu sp,sp,-N` prologues | 4,363 |
| Function-like code pointers (data + `lui/addiu` pairs) | 1,318 |
| **Function estimate** (jal ∪ pointers … ∪ prologues) | **5,453 – 6,010** |
| `jr ra` count (cross-check) | 6,173 |
| Average function size | ~365 bytes |
| `jal` call sites | 36,668 |
| **`jalr` (indirect calls)** | **139** |
| `jr <reg>` (switches / tail jumps) | 306 |
| Jump tables in `.rodata` | 297 (6,963 entries) |

Few indirect calls for a C++ code base, so there are few virtual calls, and
jump tables are easy to recover from `.rodata`. Function discovery should not
be a blocker.

### A5. Kernel services used

155 syscall stubs are linked (the whole libkernel), and **52 are actually
called**. The runtime has to provide them:
- **Threads** (×~20 sites): Create, Start, Sleep, Wakeup, ChangePriority,
  RotateReadyQueue, Suspend, Terminate…
- **Semaphores** (×~220): `SignalSema` ×86, `WaitSema` ×68, Create/Delete ×50,
  Poll ×15.
- **Interrupt and DMA handlers**: AddIntcHandler, AddDmacHandler and their
  enable/disable calls. In practice that's the VSync/GS interrupt and DMA
  completion callbacks.
- **Alarms**: SetAlarm.
- **SIF**: SifSetDma, SifSetReg/GetReg, SifDmaStat, SifSetDChain. This is the
  RPC transport to the IOP.
- **Cache / config / other**: FlushCache ×27, GetOsdConfigParam (system
  language), GetMemorySize, EndOfHeap, SetTLBEntry/_InitTLB.
- **SDK kernel patching**: SetSyscall ×12, Copy, FindAddress,
  GetEntryAddress. These become no-ops in a native runtime.

Hardware touched directly (counted by `lui` of the base address):
- EE hardware registers at 0x1000xxxx (timers, GIF, VIF, IPU): 372.
- DMAC: 6.
- GS privileged registers: 15.
- Scratchpad: 29.
- Uncached RAM mirrors: 116.
- BIOS ROM: 2 (the `rom0:ROMVER` region check).

The game builds DMA chains and VIF/GIF packets itself, so the runtime has to
model:
- DMAC channels (VIF1, GIF, SIF, SPR, IPU);
- VIF1 unpack into VU1 memory;
- GIF path arbitration;
- the GS register interface.

### A6. VU microcode (from `.DVP.ovlytab` + `.vutext`)

| Unit | Overlays | Instructions | Program ends (E-bit) | `XGKICK` |
|---|---|---|---|---|
| VU0 (4 KB micro mem) | 2 (0x000, 0x800) | 404 | 16 | 0 |
| VU1 (16 KB micro mem) | 8 (0x000…0x3800) | ~2,045 | 10 | 42 |

- **VU1 is the renderer.** Its 8 overlays tile the full 16 KB micro memory
  and send GS packets over PATH1 (`XGKICK`).
- The savestate shows **all 8 VU1 overlays and both VU0 overlays resident and
  byte-identical to the ELF**. VU code is a fixed, known set of about 2,450
  instructions. That allows either translating it ahead of time or replacing
  the renderer programs with native code (HLE). It's a Phase 3 decision.
- VU0 micro mode is invoked from 12 `vcallms` sites (small helpers,
  16 programs).

### A7. IOP side

At boot the game reboots the IOP with `rom0:UDNL cdrom0:\IOPRP270.IMG;1`,
then loads `sio2man`, `padman`, `mcman`, `mcserv`, `libsd`, `sdrdrv` and
`stream`. `MTAPMAN.IRX` is on the disc but never referenced.

| Module | Name / version | Role | HLE effort |
|---|---|---|---|
| IOPRP270.IMG | 15 kernel modules (LOADCORE…STDIO, CDVDMAN 2.33, CDVDFSV 2.33, FILEIO 2.15) | IOP kernel + CD/file services | replaced by runtime (CDVD + file RPC) |
| SIO2MAN / PADMAN | sio2man 3.00, padman 4.05 | controller | small (libpad RPC → SDL input) |
| MCMAN / MCSERV | mcman 2.34, mcserv 2.16 | memory card | medium (libmc RPC → save files) |
| LIBSD / SDRDRV | Sound_Device_Library 3.03, sdr_driver 4.01 | SPU2 access, remoted to EE via libsdr | medium: needs an SPU2 core (voices, ADPCM, reverb) |
| **STREAM.IRX** | **multi_streamer 1.02** (non-Sony) | **CD → SPU2 audio streaming**: imports cdvdman ×9, libsd ×16, timrman ×5, sifcmd ×6 | **high: custom RPC protocol to reverse from the EE side** |

### A8. Dynamic code, self-modifying code, overlays

- **Data files:** all 94 files on the NTSC disc were scanned for MIPS and VU
  signatures.
  - Only the ELF and the IOP modules contain code: 2,500–12,000 `jr ra` per
    MiB.
  - Every `.HOG` and `.PSS` file is at random-noise level.
  - Caveat: `.HOG` files are `WART3.00` archives with **compressed** members
    (zlib ratio 0.85–1.03), so this scan cannot see inside them. The RAM
    check below covers that gap at runtime.
- **Savestate** (your PAL quicksave, an indoor shop scene, PCSX2 2.6.3):
  - `.text` differs from the ELF in exactly **12 words, all of them the
    widescreen patch**; `.vutext` and `.rodata` are identical. So there's
    **no self-modifying code**.
  - **No code anywhere in RAM above the ELF image** (0x3B8000–32 MB). So
    there's **no dynamically loaded code**.
  - Low memory holds the EE kernel (0x0–0x1FFFF) and SDK kernel patches
    copied from the ELF's `.data` (0x70000–0x7FFFF). 0x80000–0x95060 looks
    like boot-loader leftovers (178 of 242 code chunks are not in the ELF).
- **Two more savestates** (NTSC slots 1 and 2, clean profile, no patches):
  - `.text`, `.vutext` and `.rodata` are **byte-identical to the ELF**
    (0 differing words);
  - no code above the ELF image;
  - all 10 VU overlays resident.

  Low memory 0x70000–0x96000 differs in only 16–23 of 38,912 words across
  all three states (two discs, two profiles), so it is static and not
  game-loaded code.
- So far: **3 gameplay states, no self-modifying and no dynamically loaded
  code**. Every new savestate gets the same check.

### A9. FMV and frame rate

- FMV: 7 PSS files on NTSC (21 on PAL), **MPEG-2 640×448 at 24–30 fps,
  3.2–8 Mbit/s**, with audio in private stream 1. Software MPEG-2 at this
  size is well within one A53 core, so FMV is low risk. The runtime can
  decode PSS directly instead of emulating the IPU.
- Frame rate: PCSX2's "50 FPS" patch for this game changes a frame-interval
  variable at `0x3A4FD8` from 2 to 1. So the stock game waits 2 vsyncs per
  frame: **30 fps on NTSC, 25 on PAL** (to be confirmed in B).

### A10. Preliminary risk view (before measurements; superseded by C6)

| # | Risk | Why | Pre-measurement rating |
|---|---|---|---|
| 1 | GS → GLES on Mali-G31 | PS2 blending (`(A−B)·C+D`, alpha up to 2.0), 4/8-bit CLUT textures, render-to-texture, very high GS fill/bandwidth (eDRAM) vs shared LPDDR on the handheld; per-draw driver overhead on A53 | **high**, unknown until B |
| 2 | VU1 execution cost | all geometry goes through ~2,045 VU1 instructions per batch; 4-wide FMAC + flags; translated or HLE'd to NEON | **high/medium**, depends on vertices/frame |
| 3 | EE recompiled speed on A53 | 294 MHz in-order MIPS vs 1.5 GHz in-order A53; code is scalar, FPU-heavy; expansion from address translation and 128-bit regs | **medium** (static picture is favourable) |
| 4 | IOP HLE | custom `STREAM.IRX` protocol + SPU2 + CDVD + pad + memory card | **medium** (effort more than speed) |
| 5 | Timing | `EETimingHack` in PCSX2: the game is sensitive to EE / DMA / interrupt timing | **medium** |
| 6 | Memory (1 GB) | 32 MB EE + 4 MB VRAM + recompiled code + GL textures ≈ 200–300 MB | **low** |
| 7 | Audio mixing / FMV | SPU2 mixing and SD MPEG-2 decode fit easily on one core | **low** |

## Part B: measurements (approved, with your additions a–d)

**Goal:** per-scene GS, VU1, EE and audio numbers at stock settings, as
max and p95 rather than averages, to set against the device budget in
Part C. You do the clicks ([MEASURE_RUNBOOK.md](MEASURE_RUNBOOK.md)); I
run the analysis.

### B0. Setup (done)

- **Clean PCSX2 2.6.3 profile** at `D:\ps2port-data\pcsx2-clean\`, built by
  `scripts/make_pcsx2_profile.py` from your bundle:
  - program files only: no BIOS, no settings, no saves;
  - your controls and hotkeys carried over;
  - all patches / widescreen / cheats off, GameIndex fixes on;
  - EE cycle rate 100 % (your bundle had 130 %), MTVU off;
  - native resolution, OSD performance and GS stats on.

  No BIOS is configured: you select your own dump in Settings > BIOS.
- **Bulk data on D:** `D:\ps2port-data\{savestates,snapshots,memcards,logs,reports}`.
  This is outside the repo, so git can't see it (`.gitignore` also lists
  `/ps2port-data/` in case it is ever linked in). C: is not used.
- The disc and bundle moved into `private/` (`private/disc/SLUS-20826.iso`,
  `private/pcsx2/`).

### B1. Scenes

Fixed in [SAVESTATES.md](SAVESTATES.md), all created in the clean profile:
- PAL slots 1–8 (S01–S08): menu, busy interior, large outdoor area,
  flying lesson, Quidditch, fight/boss, in-engine cutscene, your
  worst-case pick;
- NTSC slots 1–3 (N01–N03);
- the FMV (F01) and a pilot capture, which need no savestate.

Savestates made with the bundle's PCSX2 are excluded: the 50 FPS patch
writes into game RAM, and that value would be baked into the state.

### B2. Measurements

| # | Measure | Source | Reported as |
|---|---|---|---|
| 1 | Frame pacing: FPS / VPS / speed at EE 100 % | OSD → `observations.md` | per scene |
| 2 | **Fill / overdraw**: scissor-clipped area of every primitive, total, per render target, and split by blending / texturing / depth test; overdraw = fill ÷ the main render target's drawn area (the scene buffer, not the CRTC output); **fill scaled to the 640×480 panel** (fill × 640·480 ÷ main RT area) | GS dumps → `gsdump_stats.py` | **max, p95**, mean per scene |
| 3 | **Draw calls**: runs of primitives with unchanged draw state (prim class, attributes, FRAME/ZBUF/TEST/SCISSOR/FBA, TEX0/TEX1/CLAMP/TEXA if textured, ALPHA/PABE/COLCLAMP if blending, DTHE, FOGCOL); a transfer also ends a run. A lower bound on what a straightforward GLES renderer would issue | GS dumps | **max, p95** |
| 4 | **Vertices** (XYZ writes) and primitives by type | GS dumps | **max, p95** |
| 5 | **Texture upload bytes** (host→local IMAGE data), upload count, CLUT loads | GS dumps | **max, p95** |
| 6 | **Display mode (b)**: circuit, interlace, frame vs field mode, display size, DISPFB format, per vsync | privileged registers in dumps | per scene, all values seen |
| 7 | **Render targets and depth buffers (b)**: every FRAME base / width / format drawn to, its drawn extent, ZBUF base / format | GS dumps | per scene list; RT count and switches per frame |
| 8 | **Blending GLES can't express (c)**: per draw, flags for `(1+C)` factor forms, C = Ad (dest alpha 0..0x80 needs ×2), FIX > 0x80, COLCLAMP wrap, PABE, AA1. C = As counts separately: exact only while As ≤ 0x80 (counted per prim for untextured draws; unknown for textured ones). Also destination-alpha test and partial AFAIL modes | GS dumps | count per frame, max/p95 |
| 9 | **Framebuffer reads (c)**: draws texturing from the RT being drawn (feedback), from any other RT drawn so far in the dump (render-to-texture, incl. the previous frame), local→local copies, local→host readbacks and FIFO reads (CPU stalls on Mali) | GS dumps | count/bytes per frame, max/p95 |
| 10 | **Texture+CLUT pairs (c)**: unique (TBP0, TBW, PSM, TW, TH [+ CBP, CPSM, CSA]) per frame, de-swizzled bytes native and as RGBA8 | GS dumps | max, p95 |
| 11 | VU1 output: vertices and GIF bytes per frame. PCSX2 2.6.3 dumps don't record the GIF path (every transfer is logged as index 3), so the proxy is: all world geometry is VU1 output in this engine (A6), and uploads are the EE's | GS dumps | max, p95; × cycles per vertex from the VU1 programs (static) |
| 12 | **EE utilisation (d)**: lowest EE Cycle Rate (75 / 60 / 50 %) that still holds full FPS | OSD sweep → `observations.md` | **coarse proxy, wide error bars** (below) |
| 13 | Self-modifying / dynamic code, VU residency | `inspect_savestate.py` on every savestate | per state |
| 14 | Disc streaming rate and which files stream (optional A01 run) | `CdvdVerboseReads` log mapped through `layout.tsv` | MB/s, files |

**Frames.** This game keeps one display buffer (512×448 16-bit at FBP 140)
and starts every frame by copying the previous finished frame into it
(16 sprites). That copy, the **present**, delimits frames. Display flips
and fixed 2-vsync windows remain as fallbacks for other games. Partial
frames at the start and end of each dump are dropped, and vsyncs per
frame give the real frame pacing. Each scene gets one ~5 s multi-frame
dump (≈125–150 frames, the source of p95) and three single-frame dumps at
moments you judge heaviest (candidates for max).

**On the EE sweep (d).** EE Cycle Rate scales emulated cycle accounting,
not just CPU speed. The game needs PCSX2's `EETimingHack`, so it reacts to
EE/DMA/interrupt timing, and an underclocked run can slow down for reasons
that aren't pure CPU load. "Holds full FPS at 60 %" will therefore be read
only as "EE busy roughly ≤ 60 %, give or take a wide margin", never as a
precise budget. Phase 3 replaces this with instruction counts from the
recompiled prototype (instructions executed per frame on the hot paths)
and checks the two against each other.

**Tool status.** `gsdump_stats.py` implements rows 2–11 from PCSX2 2.6.3's
dump format (`GSDump.cpp`, `GSState::Freeze`). It passes a synthetic-dump
self-test (`tests/test_gsdump_stats.py`) covering flip, present and
vsync-window framing, and has been **validated on the 4 pilot dumps**.
The pilot found three things, all fixed:
- 2.6.3 logs every transfer as index 3, so there's no per-path split.
- The game uses a present copy, not flips.
- Overdraw has to be measured against the main render target, not the
  512-wide display output.

Checks on real data:
- The present copy measures exactly 512×448 px.
- GIF bytes agree with vertices × ~3 qwords + uploads to within 2 %.
- All 111 frames are exactly 2 vsyncs, matching the frame pacing inferred
  statically in A9.

**Known approximations:**
- Fill counts clipped geometry. It doesn't apply depth or alpha-test
  rejection, so it overestimates written pixels, which is conservative.
- The draw count models a renderer without reordering.
- Texture/RT overlap is judged by GS memory page ranges, not exact
  swizzled addresses.
- Texture alpha above 0x80 can't be seen without decoding textures. For
  textured As-blends that is the remaining question for fixed-function
  blending. It can be answered by decoding the CLUTs from the dump's VRAM
  if the scene runs show many such draws.

### B2a. Pilot result: in-engine cutscene, NTSC (not yet a scene from SAVESTATES.md)

The pilot captured a subtitled close-up dialogue cutscene (Harry and
Quirrell) rather than the main menu. That's fine for validation, and it is
real data. 4 dumps, 111 complete frames:

| Per frame | max | p95 | mean |
|---|---|---|---|
| vsyncs (pacing) | 2 | 2 | 2.0, steady **30 fps** |
| draw calls (est.) | 184 | 184 | 85 |
| vertices | 78,374 | 76,942 | 38,778 |
| primitives | 28,714 | 27,928 | 12,253 |
| fill, px (all targets) | 2.61 M | 2.57 M | 2.33 M |
| overdraw vs 640×448 main RT | 9.1× | 9.0× | 8.1× |
| fill with blending, px | 0.94 M | 0.90 M | 0.82 M |
| texture upload, bytes (count) | 1.53 M (134) | 1.53 M (134) | 0.97 M (76) |
| unique texture+CLUT pairs | 89 | 89 | 47 |
| … de-swizzled, native / RGBA8 | 3.9 / 9.8 MB | 3.9 / 9.8 MB | 3.3 / 7.3 MB |
| CLUT loads | 399 | 394 | 227 |
| GIF bytes | 5.37 M | 5.30 M | 2.88 M |
| blends GLES can't express | 0 | 0 | 0 |
| readbacks / local copies / FIFO reads | 0 | 0 | 0 |

**Render setup:**
- **Main target** 640×448 CT32 with a Z24 buffer; about 1.95 M px of fill
  per frame lands there.
- **Display** 512×448 CT16, interlaced, frame mode, NTSC (SMODE1
  CMOD = 2, even with the European BIOS). It receives the 16-sprite present
  copy.
- **Two 256×256 CT16 targets** with their own Z16 buffers, about 2.8k
  triangles each, read back as textures. They are likely character shadow
  or render-to-texture passes.

**Texture traffic:**
- Textures are almost all 8-bit palettized (T8, some T4).
- No mipmapping.
- Roughly 1 MB of texture data is re-uploaded every frame, the usual PS2
  pattern with 4 MB of VRAM.

**Blending:** all blended draws use the standard `(Cs−Cd)·As+Cd` family.
No untextured primitive has alpha above 1.0.

**Early read (not a verdict).** Draw count and fill look modest for a
GLES 3 GPU at 640×448 and 30 fps. The heavy items are:
- about 1–1.5 MB of palettized textures to convert per frame;
- up to ~78k vertices per frame through VU1;
- about 9× overdraw.

The scene runs will show whether gameplay is heavier.

### B2b. N03: first gameplay area (Ollivanders), NTSC slot 3

The first playable area, with Harry and Ollivander between shelves of
individually drawn wand boxes. 4 dumps, 55 complete frames. The camera was
still during capture, so p95 ≈ mean here.

| Per frame | max | p95 | mean | vs cutscene (B2a, mean) |
|---|---|---|---|---|
| draw calls (est.) | 877 | 877 | 874 | **10×** (85) |
| of which blended | 647 | 647 | 644 | 15× (44) |
| vertices | 132,568 | 132,568 | 132,546 | 3.4× (38,778) |
| primitives | 44,334 | 44,331 | 44,280 | 3.6× (12,253) |
| fill, px (overdraw vs 640×448) | 2.96 M (10.3×) | 2.96 M (10.3×) | 2.95 M (10.3×) | 1.3× |
| fill with blending, px | 1.74 M | 1.74 M | 1.73 M | 2.1× |
| texture upload, bytes (count) | 1.06 M (108) | 1.06 M (108) | 1.06 M (108) | 1.1× |
| CLUT loads | 1,072 | 1,072 | 1,072 | 4.7× |
| unique texture+CLUT pairs (RGBA8 MB) | 88 (9.2) | 88 (9.2) | 88 (9.2) | |
| GIF bytes | 7.56 M | 7.56 M | 7.56 M | 2.6× |
| blends GLES can't express / readbacks | 0 / 0 | 0 / 0 | 0 / 0 | |

Same render setup as the cutscene: 640×448 CT32 + Z24 main target,
512×448 CT16 display buffer, two 256×256 CT16 + Z16 side targets. Pacing
is 2 vsyncs per frame in the long dump. A few 1-vsync and 0-vsync frames
appear right after short dumps start; each is a complete frame (≈44k
primitives) finishing early after the dump-start stall, not a split.

**What it means for the budget (still not a verdict):** gameplay brings
the draw count close to 900 per frame (≈26k draws/s at 30 fps). That is
the number Phase 2 test T1 (draw-call rate on the A53 + Mali driver) has
to clear, unless the renderer batches across state changes far better
than a straightforward translation. Fill (≈3 M px/frame, 1.7 M blended)
and uploads (≈1 MB/frame) stay moderate.

### B2c–B2h. NTSC scene runs (2026-10-27, clicks by Claude with your OK)

All on the clean profile (Vulkan HW renderer, native, EE 100 %, MTVU off).
OSD readings are in `D:\ps2port-data\reports\observations.md`, sampled
about once a second.

On reading the OSD: **FPS** counts game frames per host second, and
**VPS** counts vsyncs per host second. A dip in FPS with VPS steady at
59.9x and Speed 100 % is the game itself taking 3 or more vsyncs per frame.
A dip together with VPS / Speed is a host hitch, and is ignored below.

#### B2c. N01: Ollivander dialogue cutscene, NTSC slot 1

6 dumps, 131 frames. The long dump runs from the dialogue close-ups into
the wide establishing shot of the shop. The singles include that wide
shot and the first gameplay frame (with the hint box).

| Per frame | max | p95 | mean |
|---|---|---|---|
| vsyncs | 9 | 2 | 2.08 |
| draws (est.) | 887 | 415 | 231 |
| vertices | 135,144 | 66,886 | 45,359 |
| fill, px (overdraw) | 3.54 M (12.3×) | 3.41 M (11.9×) | 2.32 M (8.1×) |
| fill with blending, px | 1.93 M | 1.56 M | 0.98 M |
| texture upload, bytes | 1.63 M | 1.63 M | 1.00 M |
| unique texture+CLUT pairs | 93 | 71 | 54 |
| blends GLES can't express / readbacks | 0 / 0 | 0 / 0 | 0 / 0 |

Every frame longer than 2 vsyncs sits at a camera cut: black frames of 3
and 9 vsyncs, then frames of 3 and 4 vsyncs while the new shot comes up.
These are loads, not render cost. The heavy end (≈880 draws, 135k
vertices) is the first gameplay frame, so the cutscene alone is lighter
than gameplay.

#### B2d. N02: gameplay, Harry alone in Ollivanders, NTSC slot 2

4 dumps, 66 frames: a still long dump at the load point, plus singles
after walking (W, D, D+W) to three other views.

| Per frame | max | p95 | mean |
|---|---|---|---|
| vsyncs | 2 | 2 | 2.0 |
| draws (est.) | 877 | 877 | 861 |
| vertices | 132,532 | 132,532 | 131,145 |
| fill, px (overdraw) | 3.85 M (13.4×) | 2.96 M (10.3×) | 2.97 M (10.4×) |
| fill with blending, px | 2.34 M | 1.74 M | 1.75 M |
| texture upload, bytes | 1.12 M | 1.06 M | 1.05 M |
| unique texture+CLUT pairs (RGBA8 MB) | 88 (9.9) | 88 (9.2) | 87 (9.2) |
| blends GLES can't express / readbacks | 0 / 0 | 0 / 0 | 0 / 0 |

This is effectively the same room and camera as N03 (B2b), and the numbers
match it closely. The views reached by walking were lighter on the OSD
(230–440 PCSX2 draws, against ≈600 at the load point). So the load point
is the heavy view of this room.

#### B2e. I01: in-engine intro cutscene, Privet Drive (no savestate)

This is an extra scene, not in SAVESTATES.md. It is the in-engine intro
that plays after a fresh boot: the night sky, then the Privet Drive street
with Dumbledore, McGonagall and Hagrid's motorbike, then close-ups. It's
the only outdoor scene reachable without PAL savestates. 4 dumps, 80
frames.

| Per frame | max | p95 | mean |
|---|---|---|---|
| vsyncs | 3 | 3 | 2.96 |
| draws (est.) | 784 | 784 | 551 |
| vertices | 107,756 | 107,730 | 84,077 |
| fill, px (overdraw) | 4.83 M (16.8×) | 3.05 M (10.6×) | 2.42 M (8.4×) |
| fill with blending, px | 2.54 M | 1.29 M | 0.79 M |
| texture upload, bytes (count) | 1.63 M (166) | 1.63 M (166) | 1.43 M (140) |
| unique texture+CLUT pairs (RGBA8 MB) | 124 (9.8) | 124 (9.8) | 111 (9.0) |
| blends GLES can't express / readbacks | 0 / 0 | 0 / 0 | 0 / 0 |

**The stock game drops to 20 fps here.**
- All 77 frames of the long dump (the street shot) take 3 vsyncs.
- An OSD check on a second boot, with no dump running, confirmed it: the
  street shot reads 19.98 FPS for about 5 s (VPS 59.9x, Speed 100 %), and
  the close-ups after it are back at 29.97.

The fill maximum (4.83 M px, 16.8×) is a single of the Dumbledore
close-up, where layered hair and beard cover the screen. It is not the
street shot.

The slowdown is not GS-bound in this measurement:
- I01 has fewer draws and vertices than N02/N03, which hold 30 fps.
- PCSX2 doesn't charge GS rasterisation time to the emulated machine.

So in PCSX2's timing model the extra time is EE/VU-side work, most likely
outdoor scene logic and three animated characters. Two consequences:
- A port that matches the original pacing can run this shot at 20 fps.
- The heaviest EE frames need more than 2 vsyncs (33 ms) of R5900 time
  even on the PS2 (as modelled), which matters for the EE budget in
  Part C.

#### B2f. F01: FMV

3 dumps, 74 frames:
- the main menu, which is a 24 fps PSS movie with the menu drawn over it
  (long dump and one single);
- the EA logo movie (one single). The first boot's EA logo ended before a
  dump could be taken, so it was caught on the second boot.

| Per frame | max | p95 | mean |
|---|---|---|---|
| vsyncs | 3 | 3 | 2.47 (2/3 alternating: 24 fps at 59.94 Hz) |
| draws (est.) | 9 | 9 | 9 |
| texture upload, bytes | 1.42 M | 1.42 M | 1.38 M |
| fill, px | 1.26 M | 1.26 M | 1.23 M |

On the GS side the FMV is one decoded frame uploaded per movie frame
(≈1.4 MB) and a handful of sprites. The cost is in the MPEG-2 decode
(A9), not the renderer.

#### B2g. EE Cycle Rate sweep (N01–N03)

Lowest game-side FPS in a ~10–18 s window after reloading the state.
Host hitches (where VPS and Speed dip too) are excluded.

| Scene | 100 % | 75 % | 60 % | 50 % |
|---|---|---|---|---|
| N01 cutscene → gameplay | 29.93 | 29.9 | 27.97 (one sample, at the cut into gameplay) | 27.9 ×3 in dialogue, 19.98 at the cut |
| N02 gameplay | 29.96 | 29.97 | 29.96 | **19.98** in 6 of 11 samples |
| N03 gameplay | 29.96 | 29.96 | 29.96 | **17.98–19.98** in 5 of 10 samples |
| I01 intro street shot | **19.98** (stock) | – | – | – |

How to read it (coarse, per D1.7):
- In the Ollivanders gameplay scenes the EE holds 30 fps at 60 % and fails
  at 50 %. EE busy time is therefore roughly **50–60 % of a 2-vsync frame**
  there (≈ 17–20 ms of 33 ms), with wide error bars.
- The I01 street shot exceeds 100 % at stock.
- Both numbers come from PCSX2's EE timing model, not the hardware. The
  Phase 3 instruction counts replace them.

#### B2h. A01: disc streaming

The run: boot, load NTSC slot 1, then about 108 s of dialogue cutscene,
gameplay with tutorial hints, and walking. `EmuCore.CdvdVerboseReads` was
on. The log was mapped to files by the new `scripts/cdvd_reads.py`
through `layout.tsv`. Output: `D:\ps2port-data\reports\A01_reads.json`.

| File | Reads | MB | Rate while active |
|---|---|---|---|
| `/RESOURCE/DAY/DAY0.HOG` | 250 | 8.47 | 44–66 kB/s steady, plus one 727 kB/s burst at the cut into gameplay |
| `/CUT/HPM04A.HOG` | 20 | 0.68 | ≈ 76 kB/s, during the dialogue only |
| `/RESOURCE/REMEMBER.HOG` | 1 | 0.03 | menu open |

Findings:
- **The mean after the state load is 85 kB/s.** Reads are regular
  ~34 KB chunks.
- Reading stops for ~20 s while a hint or menu is up.
- A steady ~44–66 kB/s matches one stereo PS-ADPCM stream (≈ 50 kB/s at
  44.1 kHz). So the "streaming" is audio coming out of the level's `.HOG`
  and the cutscene's `/CUT/*.HOG`, not separate audio files.
- That rate is trivial for an SD card. The work in reimplementing
  `STREAM.IRX` is its protocol (A7), not bandwidth.
- Boot itself reads the IRX modules, `FRONTEND.HOG` and `STARTUP.HOG`. The
  ELF and `IOPRP270.IMG` don't appear in the log, probably because fast
  boot loads them without CDVD reads.

#### Cross-scene view so far (per frame)

| Scene | vsyncs max | draws max / p95 | vertices max / p95 | fill max (overdraw) | upload max | uniq tex+CLUT max |
|---|---|---|---|---|---|---|
| PILOT cutscene | 2 | 184 / 184 | 78k / 77k | 2.61 M (9.1×) | 1.53 MB | 89 |
| N01 cutscene | 9 (cuts) | 887 / 415 | 135k / 67k | 3.54 M (12.3×) | 1.63 MB | 93 |
| N02 gameplay | 2 | 877 / 877 | 133k / 133k | 3.85 M (13.4×) | 1.12 MB | 88 |
| N03 gameplay | 2 | 877 / 877 | 133k / 133k | 2.96 M (10.3×) | 1.06 MB | 88 |
| I01 outdoor intro | 3 (**20 fps**) | 784 / 784 | 108k / 108k | 4.83 M (16.8×) | 1.63 MB | 124 |
| F01 FMV | 3 (24 fps film) | 9 / 9 | 0.9k | 1.26 M | 1.42 MB | 5 |

Across every NTSC scene the render setup is the same:
- 640×448 CT32 + Z24 main target, 512×448 CT16 display;
- one or two 256×256 CT16 side targets;
- no blends GLES can't express, no readbacks, no local copies.

The worst cases for Part C so far:
- ≈ 890 draws, 135k vertices, 4.8 M px fill and 1.6 MB of uploads per
  frame (each from a different scene);
- EE ≈ 50–60 % in gameplay, and above 100 % in the I01 street shot.

The PAL scenes (S01–S08: large outdoor areas, flying, Quidditch, effects)
are still missing and are expected to be heavier.

## Part C: verdict (2026-10-27, NTSC data only)

**Verdict: conditional go.**

What "go" covers:
- It is a go for **Phase 2**: the device benchmarks, the cross toolchain
  and the deploy scripts.
- It is not yet a go for the full port. That depends on the Phase 2
  results and on the PAL scenes (C5, C7).
- Confidence is **medium-low**:
  - Every workload number below is measured.
  - Every device number is extrapolated, with ranges about 3× wide.
  - The workload covers one gameplay room and three cutscenes. It does not
    include outdoor gameplay, flying, Quidditch or effects-heavy scenes.

Why conditional:
- **Central estimates fit with room to spare.** Gameplay at the stock
  30 fps needs about one A53 core-equivalent of the four, and about a
  quarter of the Mali-G31's fill rate.
- **The pessimistic ends have two pinch points:**
  - draw-call submission can miss its 10 ms budget;
  - translated VU1 microcode can reach a full core.

  Both have known fallbacks: batching, and native (HLE) replacements for
  VU1's few hot loops.
- **The unmeasured PAL scenes could move both pinch points (C5).**

### C1. Basis and labels

- **Measured** (PCSX2 2.6.3, desktop): every per-frame workload number
  (Part B).
- **Extrapolated:** every A53 / Mali-G31 number in C3–C4.
  - These come from spec sheets plus the assumptions stated with each
    line. Nothing has run on the device.
  - Each line names the Phase 2 test that replaces it
    ([PHASE_2_DEVICE_BENCH.md](PHASE_2_DEVICE_BENCH.md)).
  - The ranges are optimistic / central / pessimistic. They are not
    statistical intervals.
- **Workload basis:** NTSC only.
  - Two gameplay views of one room (N02, N03).
  - Three in-engine cutscenes (PILOT, N01, I01).
  - The FMV (F01) and the streaming run (A01).
- **Frame budget:** the stock pacing of 2 vsyncs at 59.94 Hz, so 30 fps
  and **33.4 ms per frame**.
- **Resolution:** the game renders at 640×448. That goes onto the 640×480
  panel 1:1 with 16 black lines, so no upscaling is needed.

### C2. The workload the budget uses (per frame)

| | Gameplay p95 (N02/N03) | NTSC maximum (scene) |
|---|---|---|
| EE emulated cycles | 4.9–5.9 M (50–60 % of 9.84 M, B2g) | 9.8–14.8 M (I01 street shot, runs at 20 fps) |
| VU1 output vertices | 132.5k | 135k (N01) |
| draws (est.) | 877 | 887 (N01) |
| fill, panel-scaled px | 3.17 M | 5.17 M (I01 close-up) |
| fill with blending, px | 1.74 M | 2.54 M (I01) |
| texture upload, native bytes | 1.06 M | 1.63 M |
| CLUT loads | 1,072 | 1,085 |
| render-target switches | 6 | 6 |
| blends GLES can't express / readbacks / copies | 0 / 0 / 0 | 0 / 0 / 0 |
| disc streaming | 44–66 kB/s | 0.73 MB/s (5 s window) |

### C3. Budget lines, extrapolated to the device

#### C3.1 EE: recompiled R5900 on one A53 (extrapolated)

**Emulated cycles to guest instructions.**
- PCSX2 charges each instruction a fixed cost by class and models no
  cache misses.
- Assumption: 1.0–1.5 emulated cycles per guest instruction on average.
- That gives **3.3–5.9 M guest instructions per gameplay frame**, or
  100–180 M per second at 30 fps.

**Host cost.**
- Assumption: 2–6 A53 cycles per guest instruction.
- Static recompilation to C emits about 1.5–3 host instructions per guest
  instruction. The extras cover address translation, 128-bit registers
  and delay slots.
- The in-order A53 runs those at 0.6–1.0 instructions per cycle, plus
  some runtime overhead.

**Result.**
- Gameplay: **0.13–0.71 core, central 0.26** (4.4 M × 3 cycles = 8.8 ms of
  33.4).
- I01 street shot (6.6–14.8 M instructions): 0.17–1.2 core at its native
  20 fps, central 0.39.
- This is one thread. It fits on one core except in the pessimistic I01
  corner. There the port would drop frames exactly where the PS2 already
  does.

**Replaced by:** T6, and the instruction counts from the Phase 3
prototype.

#### C3.2 VU1: microcode on NEON (static count measured, cost extrapolated)

**Static count** (new: `scripts/vu1_loops.py`, output in
`private/analysis/SLUS_208.26/vu1_loops.json`).
- The VU1 image has 23 innermost loops.
- The hot transform loops take **8–8.5 VU cycles per vertex**, at 2 or 4
  vertices per iteration. They are bound by the non-pipelined DIV unit
  (one reciprocal per vertex).
- Their stores (3 qwords per vertex) match the GIF bytes per vertex
  measured in B2a.
- The slowest per-vertex loops take 14–28 cycles, likely the lit paths.
- With batch setup, stalls and kicks added, the budget uses **10–42
  cycles per vertex, central 13**.

**Sanity check on the PS2.** 132.5k vertices × 8.5–28 cycles = 1.1–3.7 M
cycles, or 11–38 % of VU1's 9.84 M cycles per frame. It fits, as it must.

**Translated microcode** (extrapolated).
- Assumption: 3–8 A53 cycles per VU cycle.
  - A 4-wide FMAC is 1–2 NEON instructions.
  - The A53's NEON datapath is 64 bits wide, so each 128-bit operation
    issues over two cycles.
  - Destination masks, flags (computed only where read) and the Q/P
    registers add more.
- VIF unpack of the input: assumed 12–40 bytes per vertex at 1–2 cycles
  per byte, so 1–7 ms per frame.
- Result: **0.11–1.1 core, central 0.25.**

**HLE alternative** (extrapolated). The few hot loops replaced by native
NEON kernels at 20–60 cycles per vertex: 0.08–0.37 core.

On the device, VU1 gets its own thread, as with PCSX2's MTVU option. Only
the pessimistic translated corner reaches a full core.

**Replaced by:** T7, and the Phase 3 choice between translation and HLE.

#### C3.3 GS → GLES, CPU side: the render thread (extrapolated)

**Draw submission: 3.5–13.2 ms.**
- 877 draws per frame (p95). Of these, 717 are textured and 647 blended,
  with 6 render-target switches and about 1,070 CLUT loads.
- Assumption: 4–15 µs per draw on an A53 with the Mali driver.

**GIF parsing and vertex conversion: 1.3–3.5 ms.**
- 132.5k vertices × 15–40 cycles each.
- This drops out if HLE VU1 writes GL vertex buffers directly.

**Texture and CLUT cache: 1.5–4 ms.**
- Hash 1.06–1.63 MB of uploads per frame.
- Convert the new textures and palettes. Assumption: at most a quarter
  of the uploads are new each frame. This is the usual PS2 pattern of
  re-uploading the same textures every frame.

**Result: 0.19–0.62 core, central 0.33.**

At the pessimistic end, draw submission alone misses T1's 10 ms budget.
Batching across CLUT and texture changes (a palette atlas, texture
arrays) then becomes **mandatory**, not an optimisation.

**Replaced by:** T1, T4, T5.

#### C3.4 GS → GLES, GPU side (extrapolated)

**Fill** (panel-scaled).
- Gameplay p95: 3.17 M px per frame, so **95 Mpx/s**.
- NTSC maximum (the I01 close-up): 5.17 M px, so **155 Mpx/s**.
- Mix: about 75 % textured, 33–59 % blended, about 90 % depth-tested.
- Textures are mostly 8-bit palettized, with no mipmaps.

**Mali-G31 MP2 throughput** (spec plus assumptions).
- About 1 pixel per clock per core.
- That gives 1.2–1.6 Gpx/s peak at an assumed 600–800 MHz. The H700's GPU
  clock is unconfirmed.
- Assumption: textured and blended PS2-style shaders reach 20–50 % of
  peak, so 250–800 Mpx/s.

**Result: gameplay 12–38 % of the GPU, NTSC worst case 19–62 %.**

Other GPU-side points:
- **Overdraw** (10–17×) costs shader time. It costs little external
  bandwidth on a tile-based GPU, as long as the depth buffer is discarded
  each frame. No depth readbacks were seen.
- **Vertices (≈4 M/s) and draws** are light work for the GPU itself.
  Their cost is on the CPU side (C3.3).
- **Nothing in the NTSC scenes needs special blending or readbacks:**
  - no blends GLES can't express;
  - no readbacks;
  - no local copies;
  - render-to-texture only into two 256×256 side targets.

**Replaced by:** T2, T3.

#### C3.5 Memory (estimate)

| Item | Size |
|---|---|
| Guest memory: 32 MB EE + 2 MB IOP + 2 MB SPU2 + 4 MB VRAM | ≈ 40 MB |
| Recompiled EE code: 548,665 instructions × ~3 host instructions | ≈ 7 MB, up to 15 MB with compiler overhead |
| Texture cache: working set ≈ 10 MB RGBA8 per frame (≤ 124 pairs) | capped at 64–128 MB |
| GL driver, buffers, FMV and audio | ≈ 50–100 MB |
| **Total** | **≈ 120–260 MB**, plus OS and frontend |

- That fits in 1 GB and leaves more than T8's 400 MB free.
- Memory bandwidth (texture reads, tile writeback, guest RAM) should stay
  well under 1.5 GB/s, so it isn't expected to be a limit.

#### C3.6 Audio, IOP, streaming, FMV (estimate)

- **SPU2 emulation, IOP HLE and a `STREAM.IRX` replacement:** 0.1–0.25
  core (assumed).
- **Disc streaming:** 85 kB/s mean, 0.73 MB/s at peak. Trivial next to SD
  card rates (T8).
- **FMV:** software MPEG-2 decode at 640×448 and 24–30 fps needs roughly
  0.3–0.6 of one A53. It only runs while 3D is idle. The H700's hardware
  video decoder should also handle MPEG-2, as a fallback.
- **The IOP's cost is effort, not speed:** the custom `STREAM.IRX`
  protocol has to be reverse-engineered (A7).

### C4. Totals: gameplay at 30 fps (extrapolated)

| Line | Thread | Optimistic | Central | Pessimistic |
|---|---|---|---|---|
| EE (C3.1) | game | 0.13 | 0.26 | 0.71 |
| VU1, translated (C3.2) | VU1 | 0.11 | 0.25 | **1.10** |
| GS front end + GL submission (C3.3) | render | 0.19 | 0.33 | 0.62 (**T1 budget missed**) |
| IOP + SPU2 + OS (C3.6) | I/O, audio | 0.10 | 0.15 | 0.25 |
| **CPU, core-equivalents of 4** | | **0.53** | **≈ 1.0** | **2.7** |
| GPU fill (C3.4) | | 12 % | ≈ 25 % | 38 % (62 % in the worst NTSC cutscene) |

- **Central:** about a quarter of the CPU and the GPU. That leaves 2–3×
  headroom for the unmeasured scenes and the error bars.
- **Pessimistic:** the total still fits (2.7 of 4 cores), but two lines
  hit their limits:
  - the VU1 thread reaches a full core;
  - draw submission misses its budget.

  The fallbacks are HLE for VU1's hot loops, and batching.
- **Pipelining:** these totals assume the port keeps the PS2's overlap.
  The EE builds frame N+1 while VU1 and the GS render frame N. That costs
  one frame of latency, the same as the original.

### C5. What the missing PAL scenes could change

**Not measured.** The rows below are plausible ranges, not data. They
mark where the verdict is exposed until S03–S06 are captured.

| Budget line | Basis now (NTSC) | PAL scenes that could move it | Plausible change | Effect on the verdict |
|---|---|---|---|---|
| **Draw calls** | 877–887 in one room | S02 crowded interior, S03 grounds, S05 Quidditch | 1.5–2× (1,300–1,800 draws) | **Most likely to flip the verdict.** At the pessimistic per-draw cost: 20–26 ms, over budget. Then T1 must show ≤ 5.7 µs per draw, or batching must cut draws by at least 2× |
| **VU1 vertices** | 135k | S03, S05 | 1.5–2× (200–270k) | Translated, central: 0.4–0.5 core (fine). Pessimistic: 1.6–2.2 cores, so HLE is required. Doesn't flip the verdict alone |
| **Fill / blending** | 1.74 M blended in gameplay; 5.17 M max (a close-up) | S06 effects, S04 flying (sky, clouds), S05 stadium | 2–3× blended fill | GPU pessimistic end at 80–100 %. Would need lower-resolution effects passes or other tricks. Can flip the verdict only at the pessimistic GPU end |
| **Blends GLES can't express, framebuffer reads** | 0 in every NTSC scene | S06 spells, S04, S05 | New forms possible: destination-alpha factors, (1+C) brightening, screen copies for glows or haze | Each needs framebuffer fetch or a copy. If per-draw feedback is common and the chosen driver lacks framebuffer fetch: high risk (T2 / T3) |
| **EE** | 50–60 % in gameplay; above 100 % in I01 | S05 (many players' AI), S04 flying | 1.5–2× gameplay | Central 0.4–0.5 core (fine). Pessimistic: over 1 core at 30 fps, which means slowdowns where the PS2 also slows down. Doesn't flip |
| **Textures / CLUTs** | 1.06–1.63 MB, ≈1,070 CLUT loads, 88–124 pairs | S03, S05 | More unique textures, 2+ MB of uploads | Cache sizing and T5. Unlikely to flip |
| **Render targets** | Same set in every NTSC scene | S04, S05, S06 | Extra RTs (reflections, effects) | T3. Unlikely to flip unless readbacks appear |
| **Memory, streaming, audio** | 120–260 MB; 85 kB/s | S05 (commentary plus music) | Perhaps 2 streams, ≈130 kB/s | None |

PAL itself runs at 50 Hz with a 25 fps target, which gives 20 % more time
per frame. The port targets NTSC (D1.1), so PAL workloads are judged per
frame against the 33.4 ms NTSC budget.

**Bottom line for C5.** The verdict becomes **no-go** in either of two
cases:
- PAL gameplay roughly doubles the draw count, per-draw cost comes in at
  the pessimistic end, and batching can't recover 2×.
- Common effects need per-draw framebuffer feedback, on a driver without
  framebuffer fetch.

Everything else in the table only moves margins.

### C6. Ranked risks (replaces the preliminary A10)

| # | Risk | Evidence now | Rating | Decided by |
|---|---|---|---|---|
| 1 | Draw-call submission on A53 + the Mali driver | 877–887 draws and ≈1,070 CLUT loads per frame; 3.5–13 ms estimated | **high** | T1, then the batching design |
| 2 | VU1 execution path | 8–28 VU cycles per vertex × 135k vertices; translated 0.11–1.1 core | **medium-high** | T7; Phase 3 translate vs HLE |
| 3 | Unmeasured PAL scenes (outdoor, flying, Quidditch, effects) | C5 | **medium-high** (uncertainty, not a known problem) | S03 / S05 / S06 captures |
| 4 | Texture and CLUT handling | 1.06–1.63 MB of palettized uploads and ≈1,070 CLUT loads per frame | medium | T4, T5 |
| 5 | IOP HLE effort (`STREAM.IRX`, SPU2) | custom RPC protocol; one ≈50 kB/s ADPCM stream | medium (effort) | Phase 3 onward |
| 6 | Timing fidelity | `EETimingHack`; the game drops to 20 fps by itself (I01), so its vsync and interrupt pacing must be reproduced | medium | Phase 3 prototype |
| 7 | Recompiled EE speed | 3.3–5.9 M instructions per frame; 0.13–0.71 core | medium-low | T6, Phase 3 counts |
| 8 | GPU fill and blending | 12–38 % in gameplay, at most 62 % in the worst cutscene | medium-low (NTSC basis) | T2 |
| 9 | Memory | ≈120–260 MB of 1 GB | low | T8 |
| 10 | Audio, FMV, streaming bandwidth | 85 kB/s; SD-resolution MPEG-2 | low | T8 |

Compared with A10:
- **GS → GLES splits into two risks:** draw submission (CPU side) is now
  the top risk, and fill (GPU side) drops to medium-low.
- **EE speed goes from medium to medium-low.**
- **The PAL coverage gap is a new risk** in its own right.

### C7. What "conditional go" commits to

1. **Go:** Phase 2, per [PHASE_2_DEVICE_BENCH.md](PHASE_2_DEVICE_BENCH.md).
   Its pass thresholds are now filled in from C2–C4.
2. **To continue past Phase 2,** T1, T2, T5, T6 and T7 must meet their
   thresholds. Per the bench spec, a miss by more than 2× reopens this
   verdict.
3. **Before the Phase 3 renderer design is frozen,** capture the PAL
   scenes S03 (grounds), S05 (Quidditch) and S06 (effects) at least. They
   don't block Phase 2.
4. **No-go triggers:**
   - T1 above about 22 µs per draw (2× the 11 µs budget) with no batching
     path that halves the draw count;
   - per-draw framebuffer feedback in common effects, with no framebuffer
     fetch on the chosen driver;
   - T6 below about 150 M guest instructions/s on one A53 (half of the
     ≈295 M/s threshold).

### C8. Update from the device (2026-11-01, PHASE_2 Part B)

Measured on an **RG40XX H** (same H700 SoC; Knulli, Mali r20p0). Details
and limits are in [PHASE_2.md](PHASE_2.md) B3–B6. C1–C7 above stay as
written on 2026-10-27.

**The C7 conditions:**

| Condition (C7.2 / C7.4) | Device result | Status |
|---|---|---|
| T1 ≤ 11 µs per draw; no-go above ~22 µs | **7.9–9.0 µs** at the Part B mix, 877 draws = 7.0–7.9 ms / frame | met |
| T2 ≥ 136 Mpx/s at the gameplay mix | **1.1–1.5 Gpx/s**; gameplay uses ≈ 9 % of GPU time | met |
| T5 conversion in budget, CPU or GPU | CPU 3–5× over; **GPU palette lookup passes** (no measurable cost opaque) | met via GPU |
| T6 ≥ 295 M guest instr/s; no-go below ~150 | **327–470 M/s** (synthetic kernels) | met |
| T7 ≤ 126 ns per vertex | **49 ns unlit, 116 ns lit** (native NEON) | met |
| Framebuffer fetch available for feedback blends | **yes**: `GL_ARM_shader_framebuffer_fetch`, 160–207 Mpx/s | met |

**C4 with device numbers** (gameplay p95, 30 fps, at the sustained
1.416 GHz):

| Line | C4 central / pessimistic | Measured |
|---|---|---|
| EE | 0.26 / 0.71 | 0.22–0.58 |
| VU1 | 0.25 / 1.10 (translated) | 0.21–0.49 as native NEON; translated still unmeasured |
| GS front end + GL | 0.33 / 0.62 | 0.23–0.25 |
| IOP + SPU2 + OS | 0.15 / 0.25 | not measured |
| **CPU total, of 4** | ≈ 1.0 / 2.7 | **≈ 0.8–1.6** |
| GPU fill | ≈ 25 % / 38 % | **≈ 9 %** (31 % bound) |

**C6 changes:**
- **#1 Draw submission: high → medium.** It's within budget for NTSC. A
  doubled PAL draw count would need ~1.5× batching.
- **#2 VU1: medium-high → medium.** The HLE route fits. Translated
  microcode is a Phase 3 question.
- **#4 Textures: medium → medium-low.** The R8 + GPU palette path fits;
  hash cost and de-swizzle placement are still open.
- **#8 GPU fill: medium-low → low.**
- **New, medium: driver behaviour.** The Mali r20p0 driver turns
  per-draw buffer sub-updates into ~21 ms stalls that hung the device,
  and drops work when timer queries surround framebuffer copies. Both
  are avoidable, and they're rules 1–2 in PHASE_2 B5.
- **New, low: thermal.** Sustained 4-core load throttles to 1.416 GHz
  (−6 %), which C8 already budgets.
- #3 (PAL scenes) is unchanged and is now the largest single uncertainty.

**Verdict: go for Phase 3** (approved by the user, 2026-11-01), with three
conditions:
1. PAL S03, S05 and S06 before the renderer design is frozen.
2. A renderer built to PHASE_2 B5.
3. The Phase 3 prototype confirming T6 / T7 with real translated code.

## Open items

1. **BIOS:** the clean profile now uses `SCPH-50004_BIOS_V9_EUR_190`,
   copied into `D:\ps2port-data\pcsx2-clean\bios\` (outside the repo).
   Whether it is a dump of your own console is still unconfirmed.
2. ~~Pilot capture~~: done and validated (B2a).
3. **Savestates** per SAVESTATES.md, then the per-scene runs. NTSC slot 1
   is an in-engine cutscene (Ollivander dialogue), slots 2 and 3 are
   gameplay in Ollivanders (identified from their embedded screenshots).
   All three NTSC slots, the FMV (F01), an extra intro scene (I01) and the
   streaming run (A01) are measured (B2b–B2h). **The PAL slots S01–S08 are
   deferred** (user decision, 2026-10-27). Part C was written without
   them. At least S03, S05 and S06 are needed before the Phase 3 renderer
   design is frozen (C5, C7).
4. The OSD numbers for the pilot weren't filed in `observations.md`. They
   aren't needed for the pilot (pacing comes from the dumps). The EE sweep
   for N01–N03 is filed.
