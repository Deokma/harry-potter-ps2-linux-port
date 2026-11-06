# Phase 4: title screen on the device

**Approved by the user on 2026-11-06, with changes:** thread spike first (done, below); M3
before or alongside M2, with renderer cost per dump and temperature; **M4 is not skippable**,
and before M5 VU1 is costed from the saved street and gameplay frames (instructions per
vertex, host cost on a separate core, measured vs estimated); stop and report if M1 passes 90
tool calls; after M1, a revised estimate of tool calls and tokens for M2–M5.

## Thread spike (done, 2026-11-06)

**Question:** can eegen's code run several guest threads, given PS2Recomp's way of switching
(unwinding the host stack) can't work with its direct calls and registers in locals?
**Answer: yes, unchanged,** with one host stack and one `R5900Context` per guest thread.
`tests/thread_spike/`: an assembled toy R5900 program (no game data) through the real eegen.

| | x86 | Device, 1.416 GHz |
|---|---|---|
| 3 threads round-robin from 5 nested calls, stack canaries, float sums | pass | pass |
| With 1 kHz timer interrupts running guest handler code at any instruction, a high-priority thread woken from them, and a thread spinning on RAM with no syscall | pass | pass, about 1,500 interrupts per run |
| Bare `swapcontext` | 235 ns | **about 850 ns** |
| Switch with the toy's work | 380 ns | **about 1.6 µs** |

Peak temperature 65.4 C. In the replayed frames semaphores are mostly uncontended, so real
switches should be few; 100 per frame would cost about 0.17 ms.

**Carried into the real kernel:** a preempted thread resumes before other ready threads of its
priority (the toy lost log entries until this was right). A guest interrupt handler sets its own
FPU mode (Linux resets it in a signal handler on x86). VU0 is one unit, so its state follows
the switch. Interrupts arriving in kernel code wait until it exits.

**Not shown:** this toy kernel isn't the real one; native stubs need the same interrupt
protection as kernel calls; the spin loop not being optimised away is one small loop, on GCC at
`-O2`, and should be looked at again if the real game hangs in a RAM loop.

# Plan

**Goal:** a thin end-to-end slice. Boot to the title screen on the RG40XX H with real
picture output (GLES), audio and input stubbed, no gameplay. No more scene measuring.

**What "title screen" means here:** the main menu is a 24 fps movie with the menu drawn
over it (PHASE_1 B2f). The slice shows the menu over black, with movie decode left for
later (PHASE_1 A9 rates it low risk). The language screen, with no movie, is the first
picture. The input stub is a script: Cross on the language screen, then skip the EA logo
and intro (`PS2PORT_PAD` exists).

## Milestones

| | Milestone | Done when | Tool calls |
|---|---|---|---|
| M1 | **PC reference picture.** PS2Recomp's runtime on x86, headless. Fix stop 6 (VU1 code never arrives, PHASE_3 B3), scripted pad, its software GS writing PNGs | PNGs of the language screen and menu; a log of GIF packets and of which path draws the menu | 40–70 |
| M2 | **Runtime on the device with eegen.** Kernel, threads, IOP stubs, files from `/userdata`, all headless. Whole program through eegen and vu0gen | Same frames and same GIF packet hashes as M1 | 50–90 |
| M3 | **GS to GLES, on its own.** Plays Phase 1's GS dumps on the device (`snapshots/F01` holds the menu), no game running | Dump frames match M1 / the software GS within a set tolerance | 40–70 |
| M4 | **VU1 through vu0gen.** Loops, `XGKICK`, `XTOP`, `LQI` / `SQI`, flags where read. Skipped if M1 shows the menu's VU1 load is tiny | Every call matches the interpreter; first real VU1 cost on the device | 30–50 |
| M5 | **Join.** M2 + M3 (+ M4) on the device, stubs for audio and input | Title on the panel, screenshot saved in test mode, frame time and temperature logged | 30–60 |

**Total: about 190–340 tool calls.** Estimates, likely to move after M1.

## What we reuse

- **eegen and vu0gen:** the whole EE program; vu0gen is the base for VU1, which adds
  backward branches and `XGKICK`. The interpreter stays as the bit-for-bit reference.
- **PS2Recomp's runtime:** kernel, RPC (our ~230-line fix), VFS, pad, `ps2_vif1_interpreter`
  (783 lines), `gs_frontend` (1,736), `gs_cpu_backend` (1,980, our reference picture).
- **Replay harness:** the four checked frames remain the regression test for every
  generator change, and they should hold real VU1 input (VIF1 chains in RAM).
- **Phase 1 GS dumps:** 25 in `D:\ps2port-data\snapshots`, renderer tests needing no PCSX2.
- **Phase 2:** the firmware SDL2 / GLES loader in `bench/`, and the renderer rules (B5).

## The GS packet path

VIF1 (`UNPACK`, `MPG`, `MSCAL`) → VU1 → `XGKICK`, plus GIF DMA → GIF tags (`PACKED`,
`REGLIST`, `IMAGE`) → GS register state (`PRIM`, `FRAME`, `ZBUF`, `TEX0`, `ALPHA`, `TEST`,
`SCISSOR`...) → draws batched by state, one vertex upload per frame (B5 rule 1) → an FBO at
GS size → `DISPFB` presented through SDL. Image transfers go into a 4 MB GS memory shadow;
textures are R8 indices plus a CLUT in a shader (rule 4), cached by content hash. Blends use
fixed function, framebuffer fetch where GLES can't express them (rule 3). No readback per
frame (rule 6); screenshots only in test mode.

## Biggest unknowns

1. **Guest threads under eegen.** PS2Recomp switches threads by unwinding the host stack
   and re-entering at labels. eegen uses direct calls and registers in locals, so it can't.
   Likely answer: one host stack per guest thread (coroutines). The replay never needed this.
2. **Stop 6 and hardware timing:** why the VU1 upload doesn't land, and how much the game
   depends on DMA-end interrupts and vblank order.
3. **Stubs the game waits on:** sound, `STREAM.IRX` and movies. A stub that never says
   "finished" hangs the boot. This is the likeliest place for a surprise.
4. **Renderer details the menu needs:** swizzled uploads, and the texture cache's hash cost
   (not measured in Phase 2).
5. **Binary format:** the harness is static, but SDL loads through `dlopen`, so this needs a
   dynamic build against the firmware's libraries, as `bench/` does, at 50 MB of code.

## Working rules

Stop only under the rule in `CLAUDE.md`. Device scripts will log temperature and stop over
85 C from the first M2 run. No screen control: reference pictures come from the software GS.
Our harness sources aren't in git; at M1 they move into the repo (no game data in them).

# M1 results so far (2026-11-06)

Measured on the PC only: x86 in WSL, the runtime's software GS. Nothing here ran on the
device, and the picture has not been compared with PCSX2 pixel by pixel.

60 s boot with scripted CROSS presses from 15 s:

| | Before the fix | After |
|---|---|---|
| Picture | all black | language screen from about 4 s |
| VIF1 interrupts run | 0 | 2,492 |
| PATH3 packets, bytes | 2, 304 | 2,494, 298,521,968 |
| PATH1 packets, bytes | 20,587, 8.3 MB | 16,837, 6.8 MB |
| TRXDIR writes | 0 | 4,984 |

Changes (all in the runtime, `trial/ps2recomp-m1-runtime.patch`):
- A VIFcode with the interrupt bit runs the game's INTC 5 handler (`0x2D3398`) in place when
  it completes, on a scratch guest stack and context. Scheduler checkpoints can't unwind out
  of it.
- Reads of `VIF1_MARK` and `VIF1_CODE` return the VIF state, not the last CPU write. The
  handler compares the two and aborts the transfer if they differ.
- `processPendingTransfers` takes the VIF1 work list out first, because the handler's GIF kick
  re-enters it.

Open:
- CROSS presses don't leave the language screen, so the menu has not been reached. (Pad part fixed, see "M1 continued"; the menu is still not reached.)
- 24 of the 26 sampled frames are two different pictures (14 and 10) that differ across the
  whole screen. Cause not looked at.
- The frame sampled at 2 s is solid magenta at 640x448. Not looked at.
- Texture traffic is about 120 KB per interrupt, about 40 a second, about 5 MB/s. The cost of
  that on the device is not measured; M3 and M5 have to account for it.

VIF1 chains from saved frames, for M4 (`scripts/vif_chain.py`, static walk, not a run):

| | Gameplay (slot2+2) | Street (i01+5) |
|---|---|---|
| Tags, qwords | 5,780, 199,439 | 4,930, 164,821 |
| MSCAL | 2,506 | 2,153 |
| UNPACK vectors, bytes | 390,248, 2,912,668 | 314,421, 2,401,376 |
| Vectors per MSCAL, mean | 155.7 | 146 |

The gameplay chain has no DIRECT and 54 interrupt-flagged NOPs, each after a MARK.

# M1 continued (2026-11-06, second session)

Measured on the PC only: x86 in WSL, the runtime's software GS. Nothing here ran on the device.
Boots of 120 or 150 s, CROSS every 5 s from 5 s after the first pad read (the first row: 60 s,
presses from 15 s).

| Boot | Change | GIF packets | Where it ends |
|---|---|---|---|
| `161825` | before | 19,331 | language screen, never left |
| `164914` | pad ID | 2,212 | wait at `0x29bd38`, black from about 10 s |
| `171129`, `171626` | `Count` edited in `trial/out-b/` only, **not compiled** | 2,429, 2,336 | same wait |
| `174738` | `Count` in `src/runner/` | 2,496 | spin at `0x2d5288`, black from about 10 s |
| `180859` | + `sceCdGetReadPos` | 2,527 | same spin |

**Pad, fixed.** The game's poll (`0x2ccc50`) drops the pad unless the ID byte is `0x79` once it
has asked for pressure mode (`0x2f7e50`). The stub always answered `0x73`. It now answers `0x79`
in pressure mode. That CROSS is what leaves the screen rests on this before and after; the fixed
build was not run without presses.

**CP0 Count, fixed.** Nothing in the runtime advanced `cop0_count` (7 reads in 4 generated
functions, no writes). It now reads a host clock at 294.912 MHz (`ps2portReadCount`). The game's
stopwatch (`0x2f1f58`) is built on it at 250 units a second (`DAT_00327360`), so the wait at
`0x29bd38` is 2 s, inside the memory card check (`0x29baa0`, states 6, 7, `0x16`). **The first
attempt patched `trial/out-b/`, but the runtime builds from `ps2xRuntime/src/runner/`, a copy. So
`171129` and `171626` still had a frozen Count, and the earlier note here that the fix was "not
enough" was wrong.** With Count running the wait ends and the game goes past the card check.

**Then: a spin in what looks like the movie player's first read (settled in the next section).** The main thread sits
in `0x2d5288`, `while (DAT_003a7824 < DAT_003a7838)` (68 of 73 samples), called from `0x2dd8e0`,
which sets up IPU DMA. Calling it the movie player comes from reading the code; no movie has
played. The counter is blocks read: `0x2d3830`, a completion callback registered through
`0x2f58a8`, increments it (+0x14 of the stream struct at `0x3a7810`). `0x2d4c88` opens the file and
asks for the first read through `0x2f2af8(0x7e, ...)`. `STREAM.IRX`, the game's real driver, runs in
the emulated IOP.

`STREAM.IRX` imported `cdvdman:44` (`sceCdGetReadPos`; the ordinal is from memory of the
open-source cdvdman's import table), which was unhandled and returned 0. It now returns the whole
size of the last read, since reads here finish at once. The warning is gone and **the spin
remains**, so that was not the cause, or not all of it. The spin has a scheduler checkpoint
(`eeCheckpointDue`), so it should not starve other guest threads by itself. Cause not established.

Traced boot (`boot-20261106-181311`, 60 s, `PS2PORT_IOP_TRACE=1`, same build, ends at pc=0x2d5298): 66 `sceCdRead`, 4 `sceCdGetReadPos`, 88 `callEeRpc` (sids: 0x12344321). The trace caps at 400 CD lines and 200 EE lines, so counts at those values are floors. Its last lines:

```
[iop-trace] sceCdRead lsn=449778 sectors=16 buf=0x1d1880 ra=0x4f538
[iop-trace] callEeRpc sid=0x12344321
[iop-trace] callEeRpc sid=0x12344321
[iop-trace] sceCdRead lsn=115582 sectors=16 buf=0x1d9880 ra=0x4f538
[iop-trace] callEeRpc sid=0x12344321
[iop-trace] callEeRpc sid=0x12344321
```
Not checked: whether `0x2d3830` ever runs, what it gets from `0x2f5b18`, and whether the request
from `0x2f2af8` reaches `STREAM.IRX` over RPC `0x12345`.

**GIF packet log.** `PS2PORT_GIF_LOG=FILE` (the boot script sets it to `gif.log`): one line per
packet with path, bytes and FNV-1a 64 of its contents; the line number is the sequence. A masked
PATH3 packet appears twice. In `boot-20261106-171129` it has 2,429 lines (PATH1 2,119, PATH3 310)
and 37,749,184 bytes, matching the runtime's own `gifpkts` and `gifbytes`. It covers the language
screen only, and has no frame markers. A repeat boot of the same build with the same pad script differs: 2,429 and 2,336 packets, the first 1,753 identical, 99 of 99 distinct hashes shared. So M2's done-when ("same GIF packet hashes as M1") can't be read as an identical log. Those two boots had Count frozen, so the variation comes from elsewhere (host-timed vblanks are the likely source, not checked), and a running Count can only add to it. M2 needs either a fixed-step clock for its comparison runs or a looser comparison such as matching hash sets. Not decided.

Still missing from M1's done-when: a menu PNG, its GIF log, and which path draws the menu.

Other notes: in these boots the frame sampled at 2 s is black at 512x447, not the solid magenta at
640x448 recorded above. The language screen frames are 512x447. Neither was looked into. Harness
sources moved to `harness/`; generator check after the move: 0 RDRAM bytes differ, 2,861,449
instructions. `scripts/rpc_binds.py` lists a game's RPC binds and callers.

Calls at that point: about 175.

# M1 continued: from the first movie read to the opening cutscene (2026-11-06)

Measured on the PC only: x86 in WSL, the runtime's software GS, a `-O1` build without LTO. Nothing
here ran on the device. Frames are sampled about every 2 s and compared by hash.

| Boot | Added | How far it gets |
|---|---|---|
| `190538` | movie stub | WB logo, then asleep in the read of `RESOURCE\HPM01.HOG` |
| `191333` | IOP allocator fix | loading screen, the bar stays empty, the picture is static |
| `192107` | table entry for `0x2600e8` | bar fills, then the opening cutscene to its first subtitle |
| `192341`, `192722` | START, then CROSS, repeated | same subtitle; unchanged from 122 s to 240 s |

**Movies are stubbed (`scripts/ps2recomp_movie_stub.py`).** The play-movie function (`0x2dc178`) is a
blocking loop: decode, draw a full-screen quad, call an optional callback, flip. `frontend.thp` plays
with callback `0x1af300`, which runs the menu. So the loop stays and only the work under it goes: video setup, audio setup and both
teardowns do nothing, and decode reports "ended" for a movie with no callback and "still playing" for
one with a callback. The movie picture is left out, as the plan allows.

**PS2Recomp's IOP allocator never reused freed memory.** `IopMemory::allocate` searched upward from a
cursor that only rises. Each opened file takes a 32 KB buffer, and `STREAM.IRX` frees it on close, but
the addresses only climbed (`0x1ac4c0` to `0x1e1880` in 8 opens), and then a read request produced no
CD read. It now searches first-fit from the start of the heap. The failed allocation itself was not
logged; what shows it is that reads carry on after the fix. This also explains the movie stall above,
which needed a larger block, so the "not solved" there is settled as far as the stub needs. The
`sceCdGetReadPos` import was still worth adding.

**A hole in the function map.** `0x2600e8` is a two-instruction function (`jr ra; move v0, a0`) merged
into the end of `FUN_00260008`, so an indirect call from `0x2b6fc4` found no target. The log had said
so all along (`[guest-branch:missing-target]`). With a table entry the loading bar fills. Why skipping
that one call stopped the loading screen was not worked out. It is the only missing target in these
boots, but other gaps could show up on code paths not yet run.

**The cutscene looked stopped on its first subtitle; it was not (see "M1 result").**

Traffic in `boot-20261106-192722`: PATH1 14,196 packets, 15.0 MB; PATH3 738 packets, 80.8 MB. Most of
the PATH3 bytes come before the cutscene. 

**Build.** `private/third_party/build-fast/`, `-O1` without LTO: 11.5 min for a full compile, then
about 70 s per change instead of 12 min. `build-runtime/` is now out of date. PC boot timings from
`build-fast` say even less about the device than before.

# M1 result: the main menu on the PC (2026-11-06)

**M1's done-when is met on the PC: PNGs of the language screen and menu, a GIF packet log, and which
path draws the menu.** This is x86 in WSL with the software GS, a `-O1` build without LTO, and
rasterizing limited to 3 s of each 9 s. Nothing here ran on the device, the picture has not been
compared with PCSX2 pixel by pixel, and movies are stubbed, so the menu is over black as planned.

Boot `boot-20261106-204702`:

```
PS2PORT_GS_DRAW_WINDOW=3/9 PS2PORT_RUNNER=$PWD/private/third_party/build-fast/ps2xRuntime/ps2EntryRunner PS2PORT_STATS=1 sh scripts/ps2recomp_trial.sh boot 130 "CROSS@5+0.3,START@40+3"
```

Language screen, WB logo, loading screen, opening cutscene, then a held START skips to the menu, which
stays from about 47 s to about 77 s. The game's 30 s idle timeout then moves on (`PS_TRAIL.PSS`, stubbed;
not followed further). In `boot-20261106-204340`, a held CROSS on the menu loaded `RESOURCE\DAY\DAY0.HOG`
and `CUT\HPM02.HOG`, with the loading bar at about half when the run ended, so New Game starts loading.
Saved in `trial/reference/` (not in git): `menu.png`, `language.png`, `menu-gif.log`,
`language-to-cutscene-gif.log`.

**Which path draws the menu.** Over 24 s of it (per second only, since the game's frame rate in this run
was not measured):

| | Packets a second | Traffic | Packet size, median and largest |
|---|---|---|---|
| PATH1 (VU1 `XGKICK`) | 961 | 797 KB/s | 304 and 2,320 bytes |
| PATH2 | 0 | 0 | |
| PATH3 (GIF DMA) | 54.5 | 4.77 MB/s | 131,408 and 132,304 bytes |

Of 24,895 packets, 407 hashes are distinct. So the menu's geometry goes through VU1, and PATH3 carries
texture uploads that keep going at about 5 MB/s even here. M3's texture path has to take that cheaply, and
the menu does depend on VU1 (M4 was not skippable anyway).

**Corrections to the section above.** The cutscene never stopped on its first subtitle. It was crawling:
with everything rasterized the game's frame counter gained about 0.45 a second where it should gain 25,
because the software GS takes all the time and the game waits for it. The buttons seemed ignored because a
short press fell between frames, and the audio-position guess was wrong. Tick values move in steps of 11,
which fits the frame limiter catching up by up to about 10 ticks a frame: about 24 s per rendered frame with
all drawing, about 2 s without. What takes the 2 s was not profiled; the VU1 interpreter in a `-O1` build
is the likely part. None of these PC speeds says anything about the device, which won't use this renderer
or this interpreter.

**More fixes.**
- `PS2PORT_GS_DRAW_WINDOW=ON/PERIOD` skips rasterizing outside short windows, PC only. Packets, register
  state and image uploads are unaffected.
- Holes in the function map, again: the menu callback `0x1af300` lies inside `FUN_001aefb0`. An unlisted
  entry point needs a label, a re-entry `case` and a table entry (`scripts/ps2recomp_gen_fixes.py`): a table
  entry alone enters at the function's top and runs the wrong code. A general scan-back fallback was tried
  and removed for that reason. Listed so far: `0x1af300`, `0x126428`, `0x2a3180`, `0x2fff98`, `0x3020e8`,
  `0x3021a0`, plus `0x2600e8`. A new one shows in a log as `[guest-branch:missing-target]`, and the game
  carries on without that call, so it is easy to miss.
- The movie stub's first version read the callback from a line that never ran, because the re-entry switch
  has a case for the function's own entry. It now reads `s5`, where `0x2dc178` keeps it.

**Carry into M2.** eegen has no re-entry labels, so its function list must split at all the addresses
above, and its native list must cover the `scePad*` stubs and the movie stub's functions. `Count` must match
`ps2portReadCount`. `STREAM.IRX` still runs in the emulated IOP for every file read, at a cost on the device
that nobody has measured; a native file layer over the game's own (`0x2d3f60` and its neighbours) is the
alternative. Scripted presses must outlast a frame. GIF logs differ between boots, so M2's comparison has to
be by hash set or with a fixed clock. Not decided.

# M3 result: GS to GLES on the device (2026-11-06)

**M3's done-when is met: GS dumps play on the RG40XX H through GLES, with no game running, and their frames
match the reference within a set tolerance.** Its CPU cost is the finding that matters, and it is over budget:
see "Cost".

`gsgles/` holds a dump player (`gsplay.cpp`, `gsdump.h`) and the backend (`gs_gles_backend.*`). It sits behind the
runtime's `GSRasterBackend`, so the same frontend will feed it at M5. A contained `GSCpuBackend` keeps the 4 MB
local memory for anything unusual.

**Fidelity.** Tolerance, set here: mean absolute difference per channel on 0-255, after a 1 pixel blur, of at most
6 against the software GS at the same frame. The software GS replaying a dump matches PCSX2's screenshot closely,
which is why it serves as the reference, but it sits about a pixel to the left of PCSX2 where GLES does not, so it
is shifted 1 pixel first. On the 2D logo GLES is closer to PCSX2 than it is (1.80 against 3.12).

| | Dumps | Median | Worst |
|---|---|---|---|
| Mesa against the software GS | 25 | 3.43 | 5.06 |
| Mali against the software GS | 20 | 3.39 | 5.03 |
| Mali against Mesa | 20 | 0.26 | 0.44 |

The 5 dumps not run on Mali are the large ones, over 250 MB. These are numbers against software rendering, not
against PCSX2 pixel by pixel. The screenshots are only within a frame or so of a dump's ending, so they can't be
compared exactly; by eye the frames match them.

`F01_1` is black at every vsync in both renderers while PCSX2's screenshot shows the menu. Not explained.

**What it took to look right.** This game lights surfaces in three per-channel passes (`FBMSK`) with `Cd * As`
where As goes up to 2.0, so a clamped fixed-function factor lost all surface detail. `Cd * C` is drawn as
`Cd * max(C-1, 0) + Cd * min(C, 1)`, and `Cs * C + D` is premultiplied in the shader; neither needs a framebuffer
read. Shadows come from CT16 render targets sampled as textures, whose 1-bit alpha goes through `TEXA`. In all the
dumps, framebuffer fetch was used for 0 runs, the no-fetch fallback for 0, and partial `FBMSK` for 0.

**Cost,** measured on the device at 1.416 GHz, warm (caches kept between loops), GPU at 648 MHz, no swaps, one
thread. Milliseconds per two vsyncs, a frame at the game's usual 30 fps; the dumps' real pacing wasn't measured.

| Dump | GIF parse | of which VRAM writes | GL commands | GPU (`glFinish`) |
|---|---|---|---|---|
| `F01_2` WB logo | 2.2 | 1.5 | 1.6 | 6.8 |
| `F01_0` menu with movie | 21.2 | 6.3 | 1.3 | 5.5 |
| `PILOT_0` | 23.7 | 6.4 | 6.8 | 13.1 |
| `N01_1` | 27.6 | 7.9 | 5.4 | 9.2 |
| `N01_5` | 48.9 | 7.0 | 19.8 | 25.6 |
| `N02_1` | 42.1 | 6.5 | 16.0 | 21.7 |
| `N03_0` | 48.5 | 6.0 | 18.7 | 23.8 |
| `N03_2` | 70.9 | 9.0 | 28.8 | 31.2 |
| `I01_1` street | 46.1 | 8.9 | 10.9 | 18.6 |

Temperature peaked at 62.7 C over the whole table and a 52 s sustained run. That is the renderer alone, with no EE
or VU1 running.

- **The GPU is fine,** at 648 MHz. The driver's `simple_ondemand` stays at 240 MHz under this load, though, and the
  `performance` governor isn't available. Raising `min_freq` works, and the port will have to do that.
- **The CPU is not.** GIF parsing plus GL commands runs to 35-100 ms on one thread for a 3D frame. PHASE_2 B4
  budgeted 0.23-0.25 of a core (about 8 ms) for this line, but T1 only measured GL submission and vertex
  conversion, never parsing GIF on an A53. Parsing alone is 0.9-2.1 cores at 30 fps. This is after removing the
  frontend's debug history and state copies, cutting CLUT reloads, uploads and redundant uniforms, and going from
  133 ms to 84 ms on `N02_1`. The A53 runs this code about 7 times slower than the x86 host.
- **Options, not tried:** the frontend's per-vertex work could go on a thread of its own, separate from GL commands,
  which leaves parsing as the limit (30-70 ms). VRAM writes could go to a worker. Most of the parse is VU1 output: a
  translated VU1 (M4) could hand over vertices directly and skip GIF packets altogether, which would remove most of
  it, and is the most promising, but it is untested.

**Not implemented, none visibly needed yet:** mipmapping (`TEX1` MXL, always the base level), partial `FBMSK`,
`COLCLAMP` wrap, dithering, antialiasing, a render target sampled from anywhere but its exact base, render targets
synced by local-to-local copies, field-mode presentation. Render target alpha above `0x80` is lost once written.

**Traps found.** On this fbdev driver a swap paces everything to the display whatever the swap interval says, so
timing with swaps measures the flip. `scripts/device_run.sh` silently exited when a file wasn't yet on the device.

Calls: about 85, inside the revised 80-140.

## Revised estimate for M2–M5

M1 finished at about 255 tool calls against a plan of 40-70, about four times over; the table below
only doubled the plan. Much of that went on bugs M2 won't meet again, but if M2-M5 overrun as M1 did, the
total is nearer 600-1,080 than 300-540. That is scaling too, not a fresh analysis of each milestone.

M1 used about 115 tool calls and isn't finished (about 20–40 more), against a plan of 40–70.
The revised column doubles the plan. That is simple scaling, not a fresh analysis of each
milestone.

| Milestone | Plan (calls) | Revised (calls) |
|---|---|---|
| M2 | 50–90 | 100–180 |
| M3 | 40–70 | 80–140 |
| M4 | 30–50 | 60–100 |
| M5 | 30–60 | 60–120 |
| Total | 150–270 | 300–540 |

Tokens: roughly 0.5–1.5 million of new input and output, assuming 1.5–3k per call, plus the
cached context re-read on each round trip. This is a rough calculation, not a measurement.

# M4 result: VU1 translated (2026-11-06)

**Done:** the whole microprogram is translated, its GIF packets match the interpreter on both saved frames on x86 and on the device,
and VU1 has real device costs. **VU1 plus VIF fits the street's budget and misses gameplay's by about 6 ms.**

## What exists

- `scripts/vu1_extract.py` pulls one frame's VIF1 stream, VU1 memories and VIF registers out of a savestate
  (`private/vu1/slot2`, `private/vu1/i01`; `vu1-frames.tar` for the device).
- `vu1/vu1play.cpp` replays that stream with no game running, using the runtime's own VIF code: `--mode interp`, `native` or
  `check`. `check` runs both on separate copies, call by call, and compares registers, data memory and packets.
- `eegen/vu1gen.py` turns all of micro memory into one C++ function: 8 entries, 2,041 of 2,048 pairs reached, about 4,100 lines,
  compiled in seconds. VF and VI are sequential, because the interpreter stalls any read of a write in flight. For what doesn't
  stall (Q, P, MAC and clip flags, a branch right after an integer write) it follows the interpreter's timing along every path
  (7,921 pipeline states) and fixes each read at translation time. No read depends on the path. 22 Q reads and 3 P reads really
  do take the previous value while a newer one is in flight, so the timing work was needed.
- `eegen/eegen_vu1.h` holds the helpers. `scripts/vu1_dis.py` lists micro memory, `scripts/vu1_hot.py` shows where pairs run
  (with a `vu1gen --count` build), `scripts/vif_unpack_stats.py` shows what UNPACKs a stream asks for.
- A VIF fast path in the runtime (`ps2_vif1_interpreter.cpp`): UNPACK in mode 0 with `cl >= wl`, all these frames use. VU data memory
  at every call hashes the same as with the generic loop (`VU1PLAY_VIF_GENERIC=1`), checked on x86.

## Results

Device, one thread on one core, 1.416 GHz, median of 11 passes, spread under 1 %. Peak 60.3 C, no thermal limit near.

| | Gameplay `slot2+2` | Street `i01+5` |
|---|---|---|
| MSCAL calls | 2,506 | 2,153 |
| VU cycles, interpreter's count | 7,417,794 | 4,667,354 |
| VU pairs executed | 6,320,859 | 3,904,204 |
| Vertices (XYZ registers in packets) | 134,974 | 107,658 |
| VU cycles per vertex | 55.0 | 43.4 |
| GIF packets, data | 4,965, 6.31 MB | 4,344, 5.06 MB |
| Packet hash: interpreter and translated, x86 and device | `dccff285...`, all equal | `e97301a0...`, all equal |
| Calls differing in GIF packets, x86 and device | **0** of 2,506 | **0** of 2,153 |
| Calls differing elsewhere (data memory, ACC, sometimes P), x86 / device | 83 / 152 | 108 / 193 |
| Interpreter on the device, per frame | 13.4 s | 8.3 s |
| **VU1, translated** | **28.3 ms** (210 ns per vertex) | **17.7 ms** (164 ns per vertex) |
| **VIF, fast path** | **10.6 ms** | **8.8 ms** |
| **Both on one core** | **39.0 ms** | **26.5 ms** |
| Frame budget | 33.3 ms | 50 ms |

Packets are copied to a ring in the timed passes, as a queue to a GS thread would, so that copy is inside the VU figure.

How the cost came down, same packets at every step (gameplay / street, ms):

| Step | VU1 | VIF |
|---|---|---|
| First version | 50.0 / 28.6 | 48.4 / 39.0 |
| Cheaper MAX and MINI, scalar operations re-done only when a host float goes bad, one-lane operations in scalar, flags in vector code | 35.3 / 22.5 | |
| VIF fast path | | 10.9 / 9.1 |
| `-O3` | 28.3 / 17.7 | 10.6 / 8.8 |

`-O3` is the plateau: `-frename-registers -fweb`, `-fsched-pressure`, superblock and selective scheduling on top gave nothing.
Turning scheduling off costs 40 %, so instruction order matters a lot on the A53.

The docs held no numeric VU1 estimate to compare with. These are the first real figures.

## What the numbers mean

- **Translated VU1 now beats the PS2's own speed on one A53:** 3.8 ns per VU cycle against 6.8 ns at 147.5 MHz.
- **VU1 alone fits gameplay's budget** (28.3 ms against 33.3 ms) and the street's with room to spare. VIF and VU1 on one core miss gameplay's
  by 5.7 ms. VIF can run ahead of VU1 on its own core, as on the PS2, since the chain double-buffers; that's untested.
- **By the interpreter's own count, that gameplay frame takes the PS2 50 ms of VU1 time,** so it can't have run in 33 ms there unless
  the count is too high. Either this frame runs at 20 fps on the PS2 or the cycle model overstates. Not settled; it decides
  whether 33.3 ms is the right budget for it.
- **Where the pairs go:** two runs take 90 % of them: `0x65b`-`0x730` and `0x301`-`0x4f3`. A 14-pair light attenuation loop (`0x6ec`-`0x6f9`, with ESUM, SQRT and
  DIV per vertex) alone runs 156,513 times in the gameplay frame, 35 % of its pairs.

## Where the GS thread's time goes, and direct vertex handoff

M3 said handing vertices over directly from VU1 could remove most of the GIF parsing cost. **That was wrong: parsing is a quarter to a third of it.**
A sampling profile on the device (`GSPLAY_SAMPLE`, `scripts/sample_profile.py`, `scripts/sample_modules.py`; 100 samples per second, about 1,400 per dump, so
1 or 2 points either way) shows the CPU time of the GS path, the "GIF" and "cmd" columns together:

| Share of CPU samples | `I01_1` | `N02_1` | About, ms per 2 vsyncs |
|---|---|---|---|
| Parsing and vertices (`writeRegisterPacked`, `processGIFPacket`, `vertexKick`, `SubmitPackedStrip`, `appendPrimitive` and the like) | 24.6 % | 30.6 % | 14-17 |
| VRAM writes and textures (`fastUpload`, `ReadVram`, `resolveTexture`, CLUT) | 18.4 % | 13.1 % | 7-10 |
| libc (mostly unnamed in its exports, so likely `memcpy`; `ioctl` 4-6 %, mutexes 3-5 %, `mmap` 2 %) | 28.2 % | 24.5 % | 13-15 |
| The Mali driver | 21.0 % | 23.5 % | 12-13 |
| gsplay's own per-packet timing, not in a real run | about 3 % | about 3 % | |

The parsing and VRAM rows come from one run and the libc and driver rows from a second of the same length; the two runs took the same time to within 1 %.

The hand-over itself is cheap. `VU1PLAY_HANDOFF=1` turns each packet's vertices into host vertices on the VU1 thread, and copies the rest (register writes) for the renderer:

| Device, ms | Gameplay `slot2+2` | Street `i01+5` |
|---|---|---|
| VU1, copying packets | 28.8 | 18.0 |
| VU1, handing vertices over | 29.8 | 18.8 |
| Vertices in the shape it takes (PACKED, `ST RGBAQ XYZF2`) | 99.3 % | 99.3 % |

So handoff costs about 1 ms and can save at most the 14-17 ms above, less whatever draw batching and state handling remain. **It wasn't tried end to end**, and these
dumps aren't the same frames as the savestates. It is worth doing, but it won't close the gap alone. The other half needs a thread split: GL calls away from parsing and
VRAM writes (M3's option), plus finding what the libc time is.

`-O3` also helps this path: GIF 90.8 to 85.8 ms, 82.4 to 76.2 ms, and 10.1 to 7.6 ms on `F01_1`, per 4 vsyncs; commands and GPU unchanged. It's now the default in `gsgles/build.sh`.
Pictures weren't compared again after that change.

## Two findings

**The interpreter was wrong, and the translated code showed it.** The game passes FBMSK along as an integer with
`MINIbc.y vf3, vf0, vf4.x`. The interpreter normalized it to `ff7fffff`. The game's own GS dump (`I01_0.gs`, from PCSX2) has
`ffffff00`, `ffff00ff` and `ff00ffff`, about 9,633 times each, which is what the translated code gave. So the three lighting passes
per colour channel were getting a wrong mask under the interpreter. `ps2_vu1_upper.cpp` now compares raw bits and hands the operand on
untouched, for VU1 only: VU0 stays as it was, so its byte-for-byte checks still mean the same. Fixing it also changed the frame's
output (4,464 to 4,965 packets on `slot2`), since the microcode's control flow depends on those values.

**"Every call matches" holds for packets, not for all state.** The calls counted above differ in data memory and ACC, and sometimes P,
but not in packets or VF. The sites sit in the pipelined vertex loop (`0x34a`-`0x36b`) and take integer bits into FMAC operations: the
interpreter normalizes them, the translated code passes the NaN bits on. It looks like read-ahead past the end of the vertex list. In
these two frames it never reaches a packet: the translated code running alone, with nothing carried over from the interpreter, gives
the same whole-frame hash. Other scenes could differ, which only more frames can show. Normalizing every operand would be exact but
costs on every operation. The two platforms' counts differ because they propagate NaN bits differently.

## Not done

- Micro memory is taken as fixed. The real runtime must hash it and fall back to the interpreter on a mismatch or an unknown entry.
- Only two frames were checked. The menu's VU1 work (M1's log) wasn't replayed here, because no savestate of it exists.

Commands, from Git Bash for the device:
```
python3 -I eegen/vu1gen.py private/vu1/slot2/micro.bin private/vu1/vu1_gen.inc --report
sh vu1/build.sh x86 && private/vu1/vu1play-x86 private/vu1/slot2 --mode check --passes 2
DEVICE_TIMEOUT=400 sh scripts/device_run.sh private/vu1/vu1play-aarch64 private/vu1/vu1-frames.tar vu1/device_table.sh -- sh device_table.sh check
```

The connection dropped twice during device runs. The device had not rebooted either time (uptime 3.7 and 4 hours, 53 C), so it was the network.
`scripts/device_detach.sh` starts a run with `nohup` and polls for it, which survives that. `scripts/device_run.sh` prints "DEVICE LOST ... (reboot?)" for any lost
connection: check uptime before calling it a reboot.
