# Phase 9: frame time and heat, four pieces in parallel (2026-11-12 and 13)

Goal (user, 2026-11-12): frame time in Ollivanders from 55-67 ms to <= 40 ms (ideal 33), and 30+ minutes of play without the
85 C stop at 1.416 GHz. Four workers took one area each in a git worktree of their own, with no device access; this page is the
index and the merge record. **Everything here was measured on the PC. Nothing in this phase has run on the device: the device
numbers are the user's next test.**

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
