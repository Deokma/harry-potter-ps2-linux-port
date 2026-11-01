# Phase 3: Recompiler evaluation and prototype

**Part A (evaluation) done (2026-11-01). It ends in a decision that is
yours (brief: ask before choosing the recompiler).** Phase 3 was approved
on 2026-11-01 with three conditions (PHASE_1 C8):
- PAL S03, S05 and S06 captured before the renderer design is frozen;
- a renderer built to PHASE_2 B5;
- the prototype confirming T6 / T7 with real translated code.

**Everything in Part A comes from reading the repositories,** on
2026-11-01, with the last upstream push on 2026-10-23. Nothing has been
downloaded, built or run yet.

## A1. What this game needs from a recompiler

From PHASE_1 Part A and PHASE_2 Part B:

| Need | This game | Source |
|---|---|---|
| ISA coverage | 548,665 R5900 instructions with **0 undecodable**. 18 % FPU (PS2 semantics: no denormals, clamping, round-to-zero), 27.8k `lq`/`sq` (128-bit GPRs), 5,365 branch-likely, MMI 479, VU0 macro 308 + 12 `vcallms` | P1 A3 |
| Function discovery without symbols | 5.5–6.0k functions; 4,149 `jal` targets; 297 jump tables (6,963 entries); 139 `jalr`; 306 `jr <reg>`; 36,668 `jal` sites | P1 A4 |
| Code stability | **No self-modifying and no dynamically loaded code** in 4 savestates. One static image at `0x100000` | P1 A8 |
| Kernel | 52 syscalls actually called: threads, semaphores, INTC / DMAC handlers, alarms, SIF DMA, **SetSyscall / Copy / FindAddress** (SDK kernel patches) | P1 A5 |
| IOP | IOP reboot with `IOPRP270.IMG`, then 7 modules. sio2man, padman, mcman / mcserv and libsd / sdrdrv are Sony's; **`STREAM.IRX` is custom** (CD → SPU2 audio streaming over its own RPC) | P1 A7 |
| Graphics | All world geometry goes through VU1 microcode (≈2,045 instructions, resident, 42 `XGKICK`) to GS PATH1. ~1 MB/frame of palettized texture uploads | P1 A6, B2 |
| Video | PSS (MPEG-2 640×448) through the IPU / libmpeg | P1 A9 |
| Timing | `EETimingHack` in PCSX2; the game paces at 2 vsyncs per frame and drops to 20 fps by itself in one intro shot | P1 A1, B2e |
| Device | aarch64, GLES 3.2 on the Mali r20p0 blob over fbdev (no DRM), SDL2 `mali` driver. CPU budget at 1.416 GHz sustained. Register-struct code ran at 327 M guest instr/s vs 470 M/s with register locals. Renderer rules in PHASE_2 B5 | P2 B1–B5 |

## A2. Candidates

1. **[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp)**: the only
   PS2 static recompiler found.
   - **License:** GPL-3.0.
   - **Activity:** created 2025-04-12, last push 2026-10-23; 3.8k stars,
     234 forks, 170 commits, at least 8 recent contributors.
   - **Status:** the README calls it "Experimental".
   - The other repositories named PS2Recomp found in search
     (sp00nznet, wanderwestdev, DaniSBR1, gmh5225 and others) are forks
     of it. One fork's README says "doesn't work as it should".
2. **Building blocks for a minimal recompiler of our own:**
   - [rabbitizer](https://github.com/decompollaborate/rabbitizer): MIT,
     with an R5900 decoder. PS2Recomp itself uses it.
   - [spimdisasm](https://docs.rs/spimdisasm) and
     [splat](https://github.com/ethteck/splat): a decompilation toolchain
     whose PS2 support splat calls limited.
   - Our own `scripts/analyze_elf.py`: function, jump-table and syscall
     discovery for this ELF already.
3. **Design references from other consoles:** N64Recomp and similar
   projects. Read only, not used.

Press coverage, January 2026:
- [Gigazine](https://www.gigazine.net/gsc_news/en/20260130-ps2recomp)
  and [Linux Adictos](https://en.linuxadictos.com/ps2recomp--the-project-that-wants-to-bring-PS2-games-to-PC-without-an-emulator.html)
  describe PS2Recomp as early-stage. They say it needs an external GS
  implementation.
- A [TASVideos thread](https://tasvideos.org/Forum/Topics/27000) notes
  that no game has been completed with it.

## A3. PS2Recomp: what it can and cannot do (from the code)

**Structure:**
- `ps2xAnalyzer` writes a TOML config: stubs, skips, instruction patches.
- `ps2xRecomp` turns R5900 into C++.
- `ps2xRuntime` holds memory, kernel, devices, GS and VU1.
- `ps2xIOP` is an R3000A interpreter that runs the real IRX modules,
  with a virtual IOP kernel and HLE fallbacks.
- Plus `ps2xTest` (unit tests), `ps2xStudio`, and Android and Vita build
  files.

**It can (read from the code, not tested on this game):**
- **Translate the ISA,** with this game's special cases:
  - R5900 to C++ via rabbitizer, with MMI and VU0 macro support;
  - PS2 FPU semantics (2026-10);
  - branch-likely with annulled delay slots;
  - jump tables as a `switch`;
  - extra `entry_…` wrappers for internal entry points.
- **Find functions in stripped ELFs.** The preferred workflow is a Ghidra
  export script, with its own analyzer as a fallback.
- **Run the kernel side:**
  - a syscall dispatcher, including **SetSyscall (0x74)**, which this
    game calls 12 times;
  - an EE thread scheduler, semaphores, interrupts and EE timers;
  - SIF / RPC, CD, file I/O, memory card, pad, a VFS;
  - MPEG via FFmpeg, and audio with a VAG decoder.
- **Run IRX modules on an IOP interpreter.** That's a possible route for
  our custom `STREAM.IRX`, instead of reverse-engineering its protocol
  for HLE.
- **Run GS and VU1:** a GS front end, a **CPU (software) GS rasterizer**,
  a GIF arbiter, GS memory swizzling (PSMCT16/32, PSMT4/8), a VIF1
  interpreter and a **VU1 interpreter**.
- **Build for ARM:** Android ARM build files exist, so the code compiles
  for ARM64 somewhere. A Vita build exists too.

**Real games in its issue tracker:**
- Dragon Quest VIII boots after manual label registration.
- Killzone, Burnout 3 and Gauntlet: Dark Legacy are being fixed.
- None is listed as complete. Its 781-entry game database (names only)
  doesn't list Harry Potter.

**It doesn't fit this device as it stands:**
- **GS is drawn on the CPU.** The README says "Performance is very bad
  for VU and GS". Four A53 cores can't rasterize ~3 M textured, blended
  pixels per frame, so a **GLES backend** built to PHASE_2 B5 is needed.
- **VU1 is an interpreter.** It needs NEON / HLE for the ~10 resident
  programs (T7: 49–116 ns per vertex natively).
- **The host backend is raylib 5.5.** Its desktop (X11 / Wayland) and DRM
  backends don't fit Knulli's fbdev Mali. It needs an SDL2 + GLES host
  backend.
- **The generated code keeps every guest register in a context struct**
  (`ctx->r4 = ADD32(ctx->r4, 0x20)`). On the device that style ran at
  327 M/s versus 470 M/s with locals (T6). It passes, but with less
  margin at 1.416 GHz (≈306 M/s).
- **Every `jal` goes through `runtime->dispatchGuestBranch`** instead of
  a direct C++ call. That's a table lookup plus an EE-scheduler
  checkpoint, a debug PC-history push and an existence check, done for
  each of this game's calls. The cost per call on the A53 is unknown.
  This is the biggest performance question.
- **It is tested mainly with MSVC** and uses SSE4 / AVX in some vector
  paths. GCC / Clang portability work has been merged.
- **License:** GPL-3.0. Building on it makes this port GPL-3.0.

## A4. The options

| | **A. Fork PS2Recomp, adapt it** | **B. Minimal recompiler of our own** |
|---|---|---|
| Decoder / ISA semantics | have it (rabbitizer, MMI, VU0 macro, PS2 FPU) | rabbitizer + own C emitter; PS2 FPU via AArch64 FPCR (flush-to-zero, round-toward-zero) |
| Function discovery | have it (Ghidra or analyzer) | have it for this ELF (`analyze_elf.py`) |
| Code generation for the A53 | change the emitters: direct calls, register locals, cheaper checkpoints | designed for it from the start (direct calls, locals) |
| Kernel, SIF / RPC, CD, MC, pad, MPEG | **have a large partial base** | **write it**, but only what this game calls (52 syscalls, 7 modules) |
| IOP / `STREAM.IRX` | IRX interpreter exists | HLE: reverse-engineer the RPC protocol, or port an interpreter |
| GS → GLES, VU1 → NEON, SDL2 host | **write it** (same work in both options) | **write it** (same work in both options) |
| License | GPL-3.0 | your choice |
| Risk | inheriting an experimental, fast-moving code base with its own design (dispatch, scheduler) | the effort of the runtime breadth; no community fixes |

The kernel / IOP / device runtime is the larger share of the work in both
options. The device-critical pieces are new work either way: GLES GS,
VU1 fast path, host backend. So the decision comes down to two
questions:
- Does PS2Recomp's runtime actually carry this game far enough?
- Can its code generation reach the T6 budget on the A53?

Both can be answered by measurement, not reading.

## A5. Recommendation: a short trial before the choice

Run PS2Recomp on this ELF first. Everything stays in `private/`, and
nothing of its code enters our repo history until you choose.

1. **Recompile.**
   - Run `ps2xRecomp` on SLUS_208.26, using its analyzer and our function
     list (`analyze_elf.py`, converted to its TOML).
   - Record functions emitted, unresolved targets, jump tables recognized,
     warnings, generated C++ size, and compile time on x86-64 and aarch64.
2. **Boot on the PC.**
   - Build the runtime in WSL (x86-64, its software GS) and run the game.
   - Record each stop: IOP reboot with `IOPRP270.IMG`, module loads,
     `STREAM.IRX`, first frame, logos, title.
3. **Speed on the device.**
   - Compile a slice of the generated code for aarch64 and run it on the
     RG40XX H with recorded inputs.
   - Measure guest instructions per second **including the call
     dispatch**. That also starts condition 3: real translated code
     against T6.

**Decision rule:**
- **Fork (A)** if all three hold:
  - step 1 recompiles at least 95 % of the functions cleanly;
  - step 2 gets near the title screen with contained fixes;
  - step 3 reaches ≥ 295 M guest instr/s at 1.416 GHz, or a contained
    code-generation change gets there.
- **Otherwise (B),** reusing our analysis and rabbitizer.

**Leaning: A.** The runtime breadth is most of the effort, PS2Recomp has
a real start there, and the parts it lacks are ones we'd write anyway. The
GPL-3.0 license comes with that choice.

## A6. What the trial needs from you

1. **OK to download** into `private/third_party/` (git-ignored):
   - `ran-j/PS2Recomp` (≈6 MB);
   - its build-time fetches: ELFIO, toml11, fmt, libdwarf, rabbitizer and
     raylib 5.5. That's roughly tens of MB in total; I'll report the
     exact size.
2. **In WSL, packages only you can install** (sudo):
   - FFmpeg development libraries;
   - raylib's desktop build dependencies (X11 / GL headers) for the PC
     boot test.

   I'll give you the exact one-line `apt` command.
3. **Later and optional:** Ghidra, if PS2Recomp's analyzer misses
   functions our list doesn't cover.

**One more rule, in either option.** Generated C/C++ is derived from the
game's ELF, so it is treated like game data. It goes to `private/` and is
never committed.
