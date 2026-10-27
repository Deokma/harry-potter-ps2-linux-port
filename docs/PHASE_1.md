# Phase 1: Feasibility analysis

Status: **Part A (static analysis) done. Part B (PCSX2 measurement plan)
proposed, waiting for approval. Part C (verdict) not started.**
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
    like boot-loader leftovers (178 of 242 code chunks are not in the ELF);
    we'll confirm it never changes.
  - Limit: one scene. Part B repeats this check on every new savestate.

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

## Part B: measurement plan (proposed, needs your OK)

**Goal:** per-scene numbers for GS, VU1, EE and audio at stock settings,
which we then put against the device budget in Part C.

### B0. Clean PCSX2 profile (I set this up)

- A separate portable copy of your PCSX2 2.6.3 program files in
  `private/pcsx2-profile/`. Your bundle and its settings stay untouched.
- **All patches off** (widescreen, 50 FPS), no cheats, default speed hacks,
  EE cycle rate 100 %.
- Vulkan hardware renderer at native resolution. OSD shows FPS, VPS, CPU and
  GPU load, and GS statistics.
- BIOS: the profile **points** at your BIOS file (once you confirm it's your
  own dump); nothing is copied.

### B1. Scenes (savestates, from you)

Per-frame workload is driven by the ELF, which is identical on both discs,
so PAL savestates are fine for most scenes. Frame pacing differs (25 vs
30 fps), and I'll scale for that. Ideally, 6–8 savestates:

1. Main menu / frontend.
2. A busy Hogwarts interior with several characters.
3. The largest outdoor area (long view distance).
4. A flying lesson or Quidditch match (probably the heaviest).
5. A fight / boss / spell-heavy moment.
6. An in-engine (non-FMV) cutscene.
7. One or two of these on the **NTSC** disc too, to confirm PAL ≈ NTSC.

Boot logos and the FMV need no savestate; I boot the disc.

### B2. What I measure per scene

| Measure | How | Feeds risk |
|---|---|---|
| FPS / VPS, i.e. confirm 30 fps NTSC (25 PAL) pacing | OSD | all |
| GS per frame: primitives, vertices, state changes and estimated batched draw calls, render targets and their formats, texture uploads (bytes, PSMs, CLUT loads), blend / alpha-test / depth modes, render-to-texture use, **estimated pixels filled (overdraw)** | **GS dump** (PCSX2 multi-frame dump, ~10 frames) parsed by a new `scripts/gsdump_stats.py` | 1 |
| VU1 load: GIF data volume per path (PATH1 = VU1, PATH3 = direct), vertices produced by VU1, × cycles per vertex from the microprogram's inner loop | GS dump + static VU1 analysis | 2 |
| EE utilisation: lowest EE cycle rate (50 / 60 / 75 / 100 %) that still holds full frame rate | PCSX2 EE Cycle Rate sweep in the scene | 3 |
| Self-modifying / dynamic code check, VU residency | `inspect_savestate.py` on each state | 3 |
| Audio streaming rate and channel use | CDVD read activity during play (PCSX2 log) + STREAM RPC traffic | 4, 7 |

I drive PCSX2 myself (load the state, toggle settings, take dumps and OSD
screenshots) through screen control. That needs you to allow access to
PCSX2 when I start. You can also do the clicks yourself if you prefer.

### B3. Part C: verdict

Measured numbers against the target (4× A53 @ 1.5 GHz, Mali-G31 MP2,
GLES 3.x, 1 GB shared):
- EE in A53 core-equivalents;
- VU1 in NEON core-equivalents;
- GS as draw calls and fill/bandwidth on G31;
- memory;
- audio.

The result is **go / no-go / conditional-go** with ranked risks.

Being upfront about the limit: without the device, every A53 / Mali number
is an **extrapolation**, not a measurement. The verdict will state its
error bars. If it comes out "conditional", the condition will be a short
on-device benchmark list for the first day the device exists (GLES draw-call
and blending throughput, a recompiled-code microbenchmark). A cheap
H616/H618 board (same CPU/GPU family as the H700) could serve as a proxy
earlier, if you want one.

## Open items for you

1. **Approve Part B** as written, or adjust scenes / measurements.
2. **BIOS**: confirm the SCPH-50004 in the bundle is a dump of your own
   console (or give me the path to yours). Needed for anything that runs
   PCSX2.
3. **Savestates** for the scenes in B1 (PAL is fine), saved with the
   bundle's PCSX2 (F1 = save to slot) or in the new profile. Tell me which
   slot is which scene.
