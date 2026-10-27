# Phase 1: Feasibility analysis

Status: **Part A (static analysis) done. Part B (measurements) approved
2026-10-27 with additions a–d; tooling validated on the pilot capture
(B2a), waiting for the scene runs. Part C (verdict) not started.**
Primary disc: NTSC-U SLUS-20826. Secondary: PAL SLES-52055. The boot ELF is
the same file on both.

## TL;DR so far

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

### A10. Preliminary risk view (before measurements)

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

### B3. Part C: verdict

Measured numbers against the target (4× A53 @ 1.5 GHz, Mali-G31 MP2,
GLES 3.x, 1 GB shared):
- EE in A53 core-equivalents;
- VU1 in NEON core-equivalents;
- GS as draw calls and fill/bandwidth on G31;
- memory;
- audio.

The result is **go / no-go / conditional-go** with ranked risks. Without
the device every A53/Mali number is an **extrapolation**, labelled with
error bars. The on-device benchmarks that turn those into measurements
are specified in [PHASE_2_DEVICE_BENCH.md](PHASE_2_DEVICE_BENCH.md), to run
the day the device arrives.

## Open items

1. **BIOS:** the clean profile now uses `SCPH-50004_BIOS_V9_EUR_190`,
   copied into `D:\ps2port-data\pcsx2-clean\bios\` (outside the repo).
   Whether it is a dump of your own console is still unconfirmed.
2. ~~Pilot capture~~: done and validated (B2a).
3. **Savestates** per SAVESTATES.md, then the per-scene runs. NTSC slots 1
   and 2 already exist; which scenes are they?
4. The OSD numbers for the pilot weren't filed in `observations.md`. They
   aren't needed for the pilot (pacing comes from the dumps), but the EE
   sweep in the scene runs only exists as OSD readings.
