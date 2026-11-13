# Phase 9: frame time and heat, four pieces in parallel (2026-11-12 and 13)

Goal (user, 2026-11-12): frame time in Ollivanders from 55-67 ms to <= 40 ms (ideal 33), and 30+ minutes of play without the
85 C stop at 1.416 GHz. Four workers took one area each in a git worktree of their own, with no device access; this page is the
index and the merge record. **The workers' numbers were measured on the PC. The device numbers are in "Device results" below
(the user's two runs).**

## The four pieces

| Piece | Report | What changed | Work counters (PC) |
|---|---|---|---|
| EE game thread | `PHASE_9_EE.md` | eegen keeps resumed callers in its own code (36,347 return entries, `--return-resumes`), table calls go straight through the function table (`--fast-dispatch`); `[eework]` counters | EE instructions per game frame 28.15 M -> 25.50 M (-9.4 %); table lookups 6,368 -> 187 a frame; stack unwinds 477 -> 129; PS2Recomp's code 6.0 % -> 0 % of the game thread |
| GS / VU1 | `PHASE_9_GS.md` | draws with the same GL state merge; flat strips on the fast path; the strip fast path retried after a state change | draw calls -10 % on the shop dump (N02_1), about one merge a frame in the game's own shop; register-path strip vertices -69 % in the game's shop; 0 pixels differ on 8 dumps |
| IOP / sound | `PHASE_9_IOP.md` | LIBSD's `sceSdGetParam` / `sceSdGetAddr` native (same reads, same cycle charge); the mixer visits only sounding voices; cheaper import-stub check; PCM capture on its own thread; `PS2PORT_IOP_COUNTERS` | interpreted IOP instructions -28 % for the same guest work; LIBSD 252 k -> 45 k instructions/s; mixer output byte-identical |
| Heat | `PHASE_9_HEAT.md` | main thread sleeps until a frame is recorded; the game thread is woken by the IOP instead of polling every 50 us; `CPU_KHZ`, `CPU_GOV`, `THROTTLE_MC`, `GPU_MIN_HZ` reach the device scripts; the guard restores the clock when stopped by a signal; the Ports launcher pins 1.416 GHz | main loop waits 171-312 -> 12-43 a second; game-thread wakeups in play 559 -> 459 a second |

## What the pieces say about the targets

- **Frame time:** the EE cut is the only one with a counted drop on the limiting thread (-9.4 % instructions). If the device
  follows it, a 55-67 ms frame becomes about 50-61 ms: **not 40 ms**. The GS cut removes parsing work on stage B, whose PC time did
  not move measurably. Estimates only.
- **Heat:** in play all four cores are 60-73 % busy and two threads are near 100 %, so at a fixed clock faster code gives more
  frames, not less heat. Removing the polling is an estimated 1-3 % of one core. **The CPU clock decides the session length**
  (`PHASE_9_HEAT.md`, section 6): about 78 C at 1.32 GHz and 73 C at 1.2 GHz by estimate, with frame times 8-18 % longer, which
  is over the 55 ms line of the stop table. That trade is the user's decision after the device runs.
- **Not done, proposed by the workers:** wake a worker thread only when it is asleep and batch GS jobs (`ps2_memory.cpp`:
  `ps2portFrameFlip`, `ps2portEnqueueGsWork`, `ps2portGsAcquire`; 25-30 % of the PC game thread's samples are futex wakes);
  reorder the per-channel lightmap passes (686 of N02_1's draw breaks); a decoded-instruction cache for the IOP interpreter or
  native versions of `STREAM.IRX`'s voice poll; spread the real-time catch-up over the frame.

## Merge record (main checkout, PC, `PS2PORT_SLOWDOWN=60`, `runner/try_sound.sh 260`)

One branch at a time; after each: x86 build, sound check, New Game with no input through the cutscenes to play (mode 5).

| Step | Play reached at | Mix rate in play |
|---|---|---|
| heat (repo files) | 220.0 s | 47.8-48.1 k of 48 k |
| + GS (branch, `gs_frontend` patch, heat's frame event in `ps2_memory.cpp`) | 219.4 s | 48.0-48.2 k |
| + IOP (branch, `iop_emulator.cpp` / `iop_memory.cpp` patch) | 214.8 s | 48.0-48.2 k |
| + heat's IOP wake-up (`iop_emulator.cpp`, `ps2_iop_host.cpp`) | 214.8 s | 48.0-48.2 k |
| + EE (branch, runtime counters, `runner/gen.sh`, all eegen code rebuilt) | 215.0 s | 48.0-48.2 k |

- Heat's IOP patch was written against the old `iop_emulator.cpp`: it applied with fuzz, and the IOP worker's new counter path
  called the wait helper with the old two-argument form; fixed by hand (the needed `done` is `t.target`, as in the other calls).
- The EE worker's harness check (0 RDRAM bytes differ, exactly 2,861,449 guest instructions for `frame-ntsc02r`) ran in the
  shared harness tree with the new generator and header; the harness does not take the new return entries, the runner runs do.
- No runtime class layout changed in any piece (`gs_frontend.h` gained a member function only).

## How the parallel work was set up

`scripts/worker_sandbox.sh MAIN` gives a worktree its own `private/`: a copy of the PS2Recomp checkout (baseline tagged
`sandbox-base`), copies of `build-m2/{gen,x86}` with the batch lists pointed at the copy, links for the rest. A sandbox links a
runner identical to the main one without recompiling. WSL's git cannot read a worktree made by Windows git (git from Git Bash).
Each worker's runtime change came back as a patch in `D:\ps2port-data\workers\`. `scripts/merge_keep_both.py` resolves the
`NOTES.md` conflicts (both branches add a paragraph at the same place).

## Device results (user's runs, 2026-11-13, binary `7a0e74dc`)

Logs copied with `sh scripts/play_fetch.sh play-120126 play-121055` to `private/play/`. New Game, through the cutscenes, into
Ollivanders.

| Run | Clock | Length | Peak | 85 C stop | `[m5]` stats |
|---|---|---|---|---|---|
| `play-120126` | 1.416 GHz | 457 s | 73.3 C | none | on |
| `play-121055` | 1.32 GHz | 426 s | 73.1 C | none | off |

- **Frame time (run 1, `[m5f]`, 5 s averages, after the last cutscene, t=307-402):** 40-42 ms standing still (t=362-402; logic
  24.9 ticks a second, 23.5 frames drawn), 44-66 ms while moving; mean of the twenty windows 49.2 ms. Before Phase 9: 55-67 ms.
  The scenes are not matched view for view, so the gain is not a clean A/B. By the stop table (<= 50 ms) the work continues.
  CPU in the standing part: GameThread 87 %, GsThread 76 %, GsDraw 55 %, main 33 %, IopThread 17 %.
- **A slower part at the end (t=407-458): 84-109 ms a frame**, GsThread 79 %, GsDraw 61 %, GameThread 58 %; draw runs double
  (15,672 a second against 7,800) and 9.9 target uploads a second. Which screen that was is not known (ask the user).
- **Heat:** no stop in 7 minutes at either clock; 1.32 GHz reached the same peak (it started 5 C warmer). Both runs are too
  short for the 30 minute target.
- **Sound rate (run 1):** mixed at 47.5-48.5 k of 48 k frames a second through the whole run: real time on the device.
- **Sound that stops (both runs):** `[spu] ... went round its stream ring 3 times without a refill: ended`, 8 times at
  1.416 GHz (first at 190 s) and 11 times at 1.32 GHz (first at 42 s, then 77 s and 123 s, voices 0:0 and 0:1). The user heard
  it as sound that sometimes does not start or stops at the start at 1.32 GHz, and good sound at 1.416 GHz. This is the
  starved stream of Phase 8 (`STREAM.IRX` stops refilling; cause unknown), more frequent at the lower clock. Whether Phase 9's
  IOP changes add to it is not known: the A/B is `sh scripts/play.sh 10 CPU_KHZ=1320000 PS2PORT_IOP_HLE=0`.

## For the user: the device test

1. `sh scripts/play.sh 10 PS2PORT_M5_STATS=1` (the `[m5]` and `[m5f]` lines appear only with that switch; plain
   `sh scripts/play.sh 10` plays the same without them)
2. Go to Ollivanders (Continue), walk, then stand still for the last minutes.
3. In the log read: `[m5f]` (game frames: ms a frame, was 55-67), `[m5]` (CPU per thread), the temperature rows and whether
   `THERMAL STOP` came and when, `[lag]` (`main thread waits/s ... (event)`, frames drawn but never shown), sound by ear
   (speed, repeated phrases, cutscenes ending).
4. Then the clock runs of `PHASE_9_HEAT.md`, section 7 (each one command), starting with `sh scripts/play.sh 30` and
   `sh scripts/play.sh 30 CPU_KHZ=1320000`.

Switches to turn a piece off if something looks wrong: `GSGLES_MERGE=0`, `GSGLES_FLAT_STRIPS=0`, `PS2PORT_GS_STRIP_RETRY=0`
(picture), `PS2PORT_IOP_HLE=0` (sound). The EE change is a build switch (`PS2PORT_EG_RETURN_RESUMES=0`,
`PS2PORT_EG_FAST_DISPATCH=0` for `runner/gen.sh`).
