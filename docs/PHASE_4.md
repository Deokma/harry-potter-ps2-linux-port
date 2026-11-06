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
with callback `0x1af300`, which looks like the menu; that is from reading the code, since the menu has
not been seen. So the loop stays and only the work under it goes: video setup, audio setup and both
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

**Still open: the cutscene stops on its first subtitle.** After `CUT\HPM01.HOG` the screen is black
for about a minute, then a dim subtitle, a brighter one, and then no change for at least 118 s, with
the main thread still flipping and PATH1 traffic going on (about 50 packets a second). START and CROSS
don't skip it. A timeline driven by audio position, which the stubbed sound would never advance, would
fit, but this has not been checked. Other than that, the only pictures so far are the ones named here.

Traffic in `boot-20261106-192722`: PATH1 14,196 packets, 15.0 MB; PATH3 738 packets, 80.8 MB. Most of
the PATH3 bytes come before the cutscene. **Still missing from M1's done-when:** a menu PNG, its GIF
log, and which path draws the menu.

**Build.** `private/third_party/build-fast/`, `-O1` without LTO: 11.5 min for a full compile, then
about 70 s per change instead of 12 min. `build-runtime/` is now out of date. PC boot timings from
`build-fast` say even less about the device than before.

## Revised estimate for M2–M5

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
