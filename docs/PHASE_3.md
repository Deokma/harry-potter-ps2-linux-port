# Phase 3: Recompiler evaluation and prototype

**Part A (evaluation) done (2026-11-01). It ends in a decision that is
yours (brief: ask before choosing the recompiler).** You approved the
trial it recommends; **Part B (the trial) is done (2026-11-02), and the
choice of recompiler is now yours (B5)**:
- step 1 (recompile) passes;
- step 2 (PC boot) reaches the game's main loop after five fixes, with
  nothing visible yet (VU1);
- step 3 (device speed): PS2Recomp's code as is runs at about 63 M guest
  instructions/s at 1.416 GHz, a fifth of the bar; reworked code
  generation reaches about 290 M/s on loop code.

Phase 3 was approved
on 2026-11-01 with three conditions (PHASE_1 C8):
- PAL S03, S05 and S06 captured before the renderer design is frozen;
- a renderer built to PHASE_2 B5;
- the prototype confirming T6 / T7 with real translated code.

**Everything in Part A comes from reading the repositories,** on
2026-11-01, with the last upstream push on 2026-10-23. Part B is the
trial: PS2Recomp downloaded, built and run.

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

# Part B: the trial

PS2Recomp at commit `e5636981` (2026-10-23), cloned and built in WSL into
`private/third_party/` (git-ignored). The tools build cleanly with GCC 15.2:
the clone is 6 MB, the build directory 728 MB with its fetched
dependencies. Steps: `scripts/ps2recomp_trial.sh`, then
`scripts/compile_generated.sh`.

## B1. Step 1: recompile

Three runs on SLUS_208.26:
- **A:** PS2Recomp's analyzer and its own function discovery;
- **B:** A plus our function map (`scripts/export_function_map.py`,
  5,465 starts from `analyze_elf.py`);
- **C:** B plus a one-line change (below).

| | A | B | C |
|---|---|---|---|
| Functions | 5,750 | 5,465 | 5,465 |
| Recompiled + library stubs | 5,624 + 126 | 5,338 + 127 | 5,338 + 127 |
| Decode failures, unhandled instructions, errors | 0 | 0 | 0 |
| **Functions with the per-instruction fallback** | 108 (1.9 %) | 102 (1.9 %) | **4 (0.07 %)** |
| Extra entry points (resume labels) | 60,842 | 60,264 | 44,544 |
| Time, output | 40 s, 129 MB | 46 s, 128 MB | 45 s, 125 MB |

- **Every function is translated.** No instruction is left to an
  interpreter. Its analyzer found 4 jump tables, but the recompiler
  recognizes them itself at the `jr`: 300 of 305 `jr <reg>` sites become a
  local `switch`.
- **The fallback** is what PS2Recomp does when it can't resolve an
  indirect jump: it makes *every* instruction of that function a label the
  runtime can enter. That's correct, but those functions compile to slow
  code.
- **Most of the fallback is a design slip, not missing analysis:** a
  `jalr` (a call through a function pointer) goes through the same
  jump-table matcher as a `jr`, and fails it. A call only comes back at
  `+8`, which already gets a label. Skipping the matcher for `jalr`
  (run C) leaves 4 functions and 5 `jr` sites. Those are jump tables whose
  setup the matcher's pattern doesn't cover; our `analyze_elf.py` found
  them. The change is 13 lines in `control_flow_analyzer.cpp`, behind an
  environment variable, in `private/third_party/trial/ps2recomp-trial.patch`
  (with the step 2 test hooks).
- **A vs B:** both translate everything. Its discovery splits a few
  hundred more functions than ours. Inside a function, which split is right
  doesn't matter, because every internal target gets a label either way.
  **Across functions it does** (found in step 2, boot 3): a function
  reached *only* through a pointer must be a function start, or calling it
  fails at run time. Our map missed tiny leaves behind nop padding, such as
  a `return 0` in the game's script function table. With that fixed
  (2026-11-02) the map has **5,696 functions** (run B: 5,569 recompiled +
  127 stubs, still 102 with the fallback, 0 failures). The 89 starts that
  PS2Recomp's discovery still adds are mid-function labels (jump-table
  targets, return points after a `jal`).
- **Against the decision rule** (≥ 95 % of functions recompiled cleanly):
  **met**, 98.1 % as is, 99.93 % with the change.

**Compile check** (`scripts/compile_generated.sh`, run B output, objects
only, no link): **x86-64 compiles with no errors**: 5,466 files
(functions plus the registration table) in 171 batches of 32, GCC 15.2
`-O2`, 1,049 s wall on 12 WSL threads reading from D:, 63 MB of objects.
aarch64: waits for the cross C++ compiler.

## B2. What the generated code does (counted in run C's output)

This matters for step 3 more than the counts above:
- **Registers:** every guest GPR is a 128-bit `__m128i` in the context
  struct. A 32-bit `addiu` reads and writes it through SSE intrinsics (on
  ARM, sse2neon). T6 measured a plain `uint64_t` struct; this is heavier.
- **Each instruction also stores `ctx->pc`**, and each branch stores
  `in_delay_slot` and `branch_pc`. The compiler can't drop those stores,
  because guest memory accesses could alias the context.
- **Each memory access checks for MMIO** before the fast path (a compare
  and branch).
- **Calls:** 36,070 `jal` sites call `runtime->dispatchGuestBranch`. That
  is an out-of-line scheduler checkpoint (three atomic loads and a
  compare), two lookups in a flat function table (one slot per guest
  word, cheap), a debug PC-history push and an indirect call. **3,415 loop
  back-edges** call `eeCheckpointDue` too.
- **Resumable functions:** every function starts with a `switch (ctx->pc)`
  over its resume labels, so a thread switch can unwind the host stack and
  re-enter mid-function. This is how its EE scheduler works; it also means
  returns after an unwind go through table lookups.
- **The IOP is interpreted, in step with the EE's call count.** Every
  guest call charges 8 "EE cycles" and every loop back-edge 32, and the
  R3000A interpreter runs one IOP cycle per 8 of those. So the device
  pays for interpreting the IOP (a 36.8 MHz CPU) on top of the EE code.
  While the EE runs, IOP timing (audio streaming, disc reads) follows how
  many calls the EE makes, not how much time passes; only while the EE
  idles does it follow the clock.
- **Thread switches throw a C++ exception** (`EeDispatcherTransfer`) to
  unwind the host stack: every blocking wait (semaphore, sleep, vsync)
  pays an exception unwind through the guest call depth. That's cheap
  if the game blocks a few hundred times a second, not if it blocks tens
  of thousands of times.
- 2.6 M lines of C++ in 5,465 files.

None of this is a blocker; all of it is in the emitter and could be changed
in a fork. How much it costs on the A53 is what step 3 measures.

## B3. Step 2: boot on the PC

**Setup:**
- PS2Recomp's runtime built in WSL from run B's code (x86-64, its software
  GS, a raylib window through WSLg), with FFmpeg off: movies show blank
  frames. GL headers were already installed, so this needed no new
  packages.
- **No BIOS image:** the runtime makes up its own `rom0:`.
- The runtime reads disc files by name from the ELF's folder, so the whole
  NTSC disc was extracted with our `extract_disc.py --all` (94 files,
  1.7 GB, in `private/extract/`).
- raylib 5.5 as a shallow clone (115 MB). PS2Recomp's CMake fetches it
  with full history, about 500 MB.
- **Test hooks** in the trial patch, all off unless an environment variable
  is set: scripted pad presses (`PS2PORT_PAD`, since the game waits at
  language select), a PNG of the shown frame every 120 presents, and a
  time limit. No screen control is needed.

**Build:** 2,534 s in WSL over `/mnt/d` (5 min copying the generated
files, 30 s configure, the generated code in 171 unity batches with a
precompiled header, then an LTO link of about 8 min). Binary 73 MB, build
directory 574 MB.

**Boot 1 (no fixes beyond the build flag):** the window opens, the ELF
loads, guest code runs at 60 presents/s, but no frame ever comes from the
GS (the runtime's magenta placeholder). The game spins in its heap setup:
- crt0 calls `SetupThread(stack = -1, size = 64 KB)` and
  `SetupHeap(end of BSS, -1)`;
- the game then asks for its whole heap, up to RAM end minus 72 KB
  (0x01FEE000), and retries forever if that fails;
- PS2Recomp's `EndOfHeap` answers 0x01F00000, because the runtime keeps
  the top megabyte of guest RAM for itself (RPC and thread-local pools,
  callback stacks). So `sbrk` fails and the game never gets past it.

**Fixes needed so far:**
1. **Build:** the runtime uses SSE4.1 intrinsics, but its CMake sets flags
   only for MSVC and ARM. GCC on x86-64 needs `-msse4.1`.
2. **Guest memory layout (runtime):**
   - the runtime's pools and callback stacks move to free EE-kernel RAM
     (0x20000–0x6FFFF and 0xA0000–0xFFFFF; the game's kernel patches sit
     at 0x70000–0x7FFFF, PHASE_1 A8);
   - `SetupThread(-1)` returned the stack region's *bottom* as SP, so the
     stack grew down into the heap. It now starts near the top;
   - `SetupHeap(-1)` ends the heap at the main stack's base, as the real
     kernel does;
   - the runtime's own `guestMalloc` (used by stubs such as
     `sceGsResetGraph`) shared the region the game manages with its own
     `sbrk`. It gets its own region in kernel RAM.

   About 40 lines in three files. It's a design assumption in the runtime
   (that the top megabyte is free), so other games may hit it too.

**Boot 2 (with fix 2), 120 s, no input:**
- Heap setup passes. **All 7 IOP modules load and start on PS2Recomp's
  IOP interpreter**, with no HLE fallback and no missing import: SIO2MAN,
  PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV and the custom `STREAM.IRX`.
- `STREAM.IRX` finds the disc and caches its directory (94 files), as on
  a PS2.
- Then the EE stops for good, about 4 s in, still before any frame (every
  dump is the magenta placeholder). The game's file layer has queued a
  read for `STREAM.IRX`, stored its thread ID with the request and called
  `SleepThread`; it waits for a wake-up that never comes.

**Stop 3: IOP→EE RPC is missing.**
- This game registers an RPC server *on the EE* (sid `0x12344321`).
  `STREAM.IRX` imports `sceSifBindRpc` and `sceSifCallRpc` (sifcmd 15 and
  16, PHASE_1 A7 import table) to call it, which is how it reports
  finished reads.
- In PS2Recomp the IOP side of those two calls returns 0 and does nothing,
  and the EE side of `sceSifRpcLoop` is a stub that returns at once. So an
  IOP module can't call an EE server, and the wake-up never happens.
- The runtime already runs guest code on the EE for commands the IOP sends
  (`sceSifSendCmd` → a `GuestInvocation`), so IOP→EE RPC can be built the
  same way: bind fills the client from the EE server table, call copies
  the arguments into the server's buffer, runs the server function as an
  invocation, copies the reply back and wakes the IOP caller. That's a
  missing feature of maybe 150–300 lines in the IOP emulator and the
  runtime, not a small fix.

**Fix 3: IOP→EE RPC (built 2026-11-02, your go-ahead).** About 230 lines
in 7 files of the trial copy:
- IOP side: `sceSifBindRpc` fills the client from the EE server table,
  `sceSifCallRpc` hands the call to the host and puts the calling IOP
  thread to sleep (unless it asked not to wait), `sceSifCheckStatRpc`
  reports calls in flight;
- runtime side: copies the arguments into the EE server's buffer, queues
  the server function as an EE invocation, then copies the reply back to
  the IOP and wakes the caller. End functions on the IOP aren't supported
  (logged if used).

While doing it, two more slips of the same kind turned up: the EE stubs
for `sceSifRegisterRpc` and `sceSifSendCmd` read arguments 5–7 from the
stack, but the EE's EABI passes them in t0–t3 (its `sceSifCallRpc` stub
gets this right). Both now read the registers.

**Boot 3 (fixes 1–3), 180 s, X pressed every 5 s from second 20:**
- **The RPC fix works:** the game's reads complete and it loads several
  files in a row (the dispatch trace shows the read path repeating).
- Then it builds its script function table (`ScriptFns`) and calls one of
  the pointers in it, 0x288A38, which run B didn't have as a function:
  **stop 4**, still before any frame.
- Fix 4 is in our tooling, not PS2Recomp: the function map (B1, "A vs
  B").

**Boot 4 (fixes 1–4), 240 s, X presses:**
- No missing functions. One IOP import is unhandled: `cdvdman` 44 (I
  believe `sceCdGetReadPos`, used by streaming), right after the directory
  cache. Not yet known to matter.
- The EE then sits in the game's vsync wait for the rest of the run, still
  with no frame. That wait drains a semaphore with `PollSema` until it
  returns -1. **Stop 5:** PS2Recomp's `PollSema` returns -419
  (`KE_SEMA_ZERO`, the IOP kernel's code) on an empty semaphore, so the
  loop never ends. The EE kernel returns -1.
- **Fix 5:** the EE `PollSema` handler returns -1 on failure (one line).

**Boot 5 (fixes 1–5), 240 s, X presses:**
- **The game reaches its main loop.** For the whole run the EE sits in a
  function that calls the per-frame routines and increments a frame
  counter. The display is set up: frames are now black, not the "no
  frame" magenta.
- **Stop 6, VU1:** the log fills with 45,349 "VU1 reserved lower"
  errors, all for the same word `0xFFFFFFFB` at 4 addresses near the top
  of VU1 code memory. This game's microcode is resident and fills the
  whole 16 KB (PHASE_1 A6), so real code should be there: it looks like
  the microcode upload (VIF1 `MPG` from the DMA chain) didn't land, not
  a missing opcode. All 3D goes through VU1, so nothing is drawn.
- Not diagnosed further. VU1 and GS are the parts the port replaces
  anyway (VU1 → NEON / HLE, GS → GLES), so this stop matters less for the
  decision than the EE and IOP ones.

## B4. Step 3: speed on the device (run on the RG40XX H, 2026-11-02)

**Method.**
- **Recording on the PC.** The trial patch records one guest call
  (`PS2PORT_SNAP_PC`): registers, RAM and scratchpad before and after, and
  every function it reached. A second hook (`PS2PORT_CALLCOUNT`) counts
  function entries, used to pick the calls. In boot 5's main loop the frame
  loop runs about 28 times a second.
- **The harness** (`private/third_party/trial/harness/`, private because
  it builds against PS2Recomp's headers and the generated code):
  - the generated files the recorded call reaches, plus a stand-in for
    PS2Recomp's runtime with the same per-call work: checkpoint charge,
    three atomic flag loads, the IOP cycle carry, a dispatch-history push,
    two table lookups and an indirect call;
  - **not modelled:** the IOP step the real runtime takes on every guest
    call (servicing IOP DMA, callbacks and timers and looking for a ready
    IOP thread). On the PC profile that was about 1 % of all time against
    12 % in generated code;
  - uncontended semaphores and the thread ID answer as an idle kernel
    would; any other syscall, MMIO, break or VU0 microprogram makes a replay
    invalid (none happened);
  - each iteration restores only the RAM pages the call changes and times
    only the call; **every build is checked against the PC's result**
    (RAM, v0, sp);
  - a counting build gives the exact guest instruction count (as T6 did);
  - aarch64 build with PS2Recomp's ARM flags and sse2neon v1.9.1, static.
- **On the device:** EmulationStation stopped, `performance` governor
  (1.512 GHz, restored after), 3 runs of 500–5,000 iterations; medians
  repeat within 1 %. Figures at 1.416 GHz (the 4-core sustained clock) are
  scaled from 1.512 GHz.

**Recorded calls** (all from the main loop, 800th call of each, except the
first):

| Call | What it is | Guest instr. | Calls inside | Loop checkpoints |
|---|---|---|---|---|
| `FUN_002ce218` | small per-frame routine (100th call) | 573 | 6 | 9 |
| `FUN_001b3988` | per-frame routine, straight-line | 2,200 | 21 | 26 |
| `FUN_001b0fd0` | per-frame routine with loops | 24,049 | 403 | 1,818 |

**The aarch64 build of PS2Recomp's code gives exactly the PC's result**
(0 bytes of RAM differ, same v0 and sp), under qemu and on the device.

**Speed, and what code-generation changes buy** (each variant also passes
the result check):
- **v0:** PS2Recomp's output as is;
- **v1:** guest registers read and written as plain 64-bit integers instead
  of 128-bit vector lanes (a header change);
- **v2:** v1 without the per-instruction `ctx->pc` stores;
- **v3:** v2 with loads and stores going straight to RAM (no MMIO check,
  no wrap handling; valid here because these calls touch no MMIO) and no
  out-of-line loop checkpoint;
- **v4:** v3 with a lean call (one table lookup and the call), roughly what
  direct calls cost.

| | `FUN_001b0fd0` (loops) | | `FUN_001b3988` (straight-line) | |
|---|---|---|---|---|
| | per call | M guest instr/s at 1.512 → 1.416 GHz | per call | M guest instr/s at 1.512 → 1.416 GHz |
| v0 as is | 358 µs | **67 → 63** | 66.4 µs | 33 → 31 |
| v1 | 312 µs | 77 → 72 | 59.7 µs | 37 → 35 |
| v2 | 307 µs | 78 → 73 | 55.9 µs | 39 → 37 |
| v3 | 96 µs | 250 → 234 | 11.6 µs | 190 → 178 |
| v4 | 77 µs | **312 → 292** | 10.4 µs | 212 → 198 |

ARM code for `FUN_001b0fd0`'s 32 functions: 387 KB as is, 299 KB at v2,
101 KB at v3. For comparison, T6's struct-style synthetic code ran at
327 M/s on the same device.

**What this says:**
- **As it stands, PS2Recomp's code reaches about 63 M guest instructions
  per second at 1.416 GHz: about a fifth of the 295 M/s bar.**
- The cost isn't in the translated instructions. It's in how every memory
  access is wrapped (MMIO check, wrap handling, a lambda per access), the
  out-of-line scheduler call on every loop iteration, and the call
  dispatch. The 128-bit register model and the `pc` stores cost less (v1,
  v2: 15 %).
- **With those parts reworked, the same translated code runs at about
  290 M/s on loop-heavy code and about 200 M/s on straight-line code:
  at the bar, not clearly over it.** Straight-line code stays slower
  because code size still exceeds the A53's 32 KB instruction cache.
- v3 and v4 are measurements, not finished designs. A real emitter needs
  a cheap MMIO path where an access can be one (a range compare), a cheap
  scheduler checkpoint (a local countdown) and direct calls. That's a
  rewrite of PS2Recomp's emitter and its runtime calling conventions, not
  a contained change.
- **Not measured yet:** the heavy per-frame work. These calls come from
  the boot screens; the game's big per-frame update (`FUN_002635d0`, a
  2,613-function call tree) blocked mid-call, so it couldn't be recorded.
  Gameplay frames will be much heavier than the ~27,000 guest instructions
  these three routines execute.

## B5. The trial against the decision rule

| Rule (A5) | Result |
|---|---|
| Step 1: ≥ 95 % of functions recompile cleanly | **Met.** 98.1 % as is, 99.93 % with a one-line change (B1) |
| Step 2: near the title screen with contained fixes | **Partly.** The game reaches its main loop with the display set up, after 5 fixes (one of them a ~230-line missing feature, IOP→EE RPC). Nothing visible yet: VU1 runs code that never arrived (stop 6, open) |
| Step 3: ≥ 295 M/s at 1.416 GHz, or a contained code-generation change gets there | **Not met as is** (63 M/s). Reworked code generation gets to about 290 M/s on loop code, but that's an emitter rewrite, not a contained change |

By the rule as written, that's **option B**. The trial also shows where
PS2Recomp's value is: its runtime got the game from the ELF entry to the
main loop, through the IOP modules (including the custom `STREAM.IRX`), the
kernel and SIF, with five fixes. Its code generation is what doesn't fit
the A53.

# Part C: milestone 1, a gameplay frame through our code generator

**Decision (you, 2026-11-02):** the hybrid route. Fork PS2Recomp's
runtime, write our own code generator; the port is GPL-3.0. Conditions in
NOTES D3.4. This milestone comes before any other Phase 3 work.

**The frame.** In both gameplay savestates (NTSC slots 2 and 3, in
Ollivanders) the main thread's stack shows the live call chain:
crt0 → `main` (`0x26E438`) → **`0x2635D0`, called once per frame** →
`0x251610` (the gameplay frame) → `0x2C5F70` → `0x2CE2C8` (end of frame) →
the vsync wait, where the main thread sits. So one call of `0x2635D0`
from `main`, started on the savestate's RAM, is one gameplay frame.

**Plan:**
1. **Replay state from the savestates:** RAM, scratchpad, FPU control,
   VU0 registers and memory from the `.p2s`; the call's registers
   rebuilt from `main`'s call site (the savestate's own CPU registers
   belong to the PS2 kernel's idle code).
2. **Reference run on the PC:** the same frame through PS2Recomp's
   generated code, in the replay harness with a deterministic "idle
   hardware" stand-in (DMA idle, no interrupts, the vsync wait returns at
   once). Both generators run against the same stand-in, so the comparison
   is byte-for-byte.
3. **Our code generator,** designed from the trial's measurements (B4):
   plain 64-bit registers, direct RAM access with a cheap MMIO path, a
   local countdown instead of a scheduler call per loop pass, direct
   calls. Differential tests against the reference until the frame
   matches byte for byte.
4. **On the device at the 1.416 GHz budget clock** (`scaling_max_freq`
   pinned): instr/s, and A53 PMU counters for the instruction cache.
5. **IOP interpreter share,** and whether `STREAM.IRX` needs native HLE.
6. Report; stop if the frame misses the budget (condition 3).

**Plan adjustments (you, 2026-11-02):**
1. **Cost model:** the budget covers all EE work per frame, not only the
   `0x2635D0` call: interrupt handlers (vblank, DMA end), other guest
   threads and callbacks the IOP triggers. State what the stand-in
   hardware omits and estimate it (traced in the PC run with real timing).
2. **PCSX2 check:** besides the PS2Recomp reference, load the same
   savestate in PCSX2, advance exactly one frame, dump RAM and diff against
   our result, masking known-volatile regions; report mismatches by region.
3. **De-risk first:** replay the gameplay frame with the quick optimized
   variant (B4's v4: plain registers, direct RAM, cheap calls, applied to
   PS2Recomp's output) and report instr/s at 1.416 GHz. **Write the full
   generator only if that is within about 2x of the budget.**
4. Short tool output, commit and update NOTES before long steps, batch
   fixes to keep rebuilds few.

## C1. The replay (built 2026-11-02)

**State** (`scripts/make_replay_state.py`): RAM, scratchpad and VU0's vf/vi
registers from the savestate (VU0 holds live values between calls: vf1–3 look
like a rotation matrix); VU0 data and micro memory alongside. Registers for
the call: pc `0x2635D0`, sp `0x1FFEBD0` (the frame function saves `ra` at
0(sp) after `addiu sp, -0x40`, and its saved `ra` sits at `0x1FFEBD0 - 0x40`),
gp `0x3A7570` (crt0), ra 0. The function takes no arguments.

**What the replay runs:** the whole program (all 5,696 functions) is compiled
into the harness; one call of `0x2635D0` runs to its return.

**The idle-hardware model** (`trial/harness/hwmodel.inc`), the same for every
code generator:
- scratchpad and VU memories are real memory; other I/O registers remember
  the last value written;
- every DMA transfer has finished (CHCR.STR reads 0, D_STAT reads all
  channels complete); GIF / VIF status idle; timers advance per read;
- every syscall answers at once: counting semaphores (`PollSema` returns -1
  when empty, a wait that would block counts as satisfied), the thread ID,
  default 0 for the rest, counted by number;
- PS2Recomp's own `LibC.cpp` for the 16 math / memory / string stubs the
  game binds natively (identical semantics for both generators); the other
  110 native stubs (pad, memory card, CD, MPEG, sound, SIF RPC) return 0 and
  are counted;
- VU0 microprograms (`vcallms`) are counted and skipped;
- the vsync wait (`0x2CD6A0`) returns at once (`HARNESS_SKIP`): on the PS2
  it is idle time, and the vblank handler it waits for is costed separately.

**What it leaves out, to be costed separately (plan adjustment 1):**
- **interrupt handlers:** vblank (`0x2CD788`, which also calls a game
  callback through the pointer at `0x328480`; two per frame), DMA-end
  handlers, alarms;
- **other guest threads** (the boot profile showed a long-running thread at
  `0x17DD50`);
- **work the IOP triggers on the EE:** SIF command handlers, the EE RPC
  server `STREAM.IRX` calls (`0x2F58B8`), RPC end callbacks;
- **VU0 microprograms** and the **native stubs' own work** (pad, sound RPC);
- everything outside the EE: GS, VU1, VIF, IPU, SPU2 and the IOP itself.
