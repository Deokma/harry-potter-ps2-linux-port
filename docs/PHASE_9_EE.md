# Phase 9, EE worker: cutting the game thread's work (2026-11-12)

The brief: cut the gameplay frame in Ollivanders from 55-67 ms toward 40 ms on the device, working on the EE game thread (guest code
and the runtime it calls), measured on the PC only. **All numbers below are PC numbers (WSL, x86), not device timings.** The
device was not used.

## How it was measured

`scripts/ee_profile_play.sh NAME RUNNER`: New Game with no input after the menu (`runner/try_sound.sh`'s pad route), full PC
speed, 270 s; the game reaches play (mode 5 in `0x38c194`) at about 183-187 s; samples (`PS2PORT_SAMPLE`) from 200 to 260 s.
New work counters (`PS2PORT_STATS=1`, `[eework]` lines, sandbox runtime; `scripts/ee_frames.py` turns them into per-frame numbers
over the window):

- **game frames**: failed `PollSema` calls returning to `0x2cd6c8` (the game's vsync wait), the `[m5f]` definition;
- **EE instructions per game frame**: the game thread's user-mode instructions from the hardware counter
  (`perf_event_open`, `PERF_COUNT_HW_INSTRUCTIONS`, works in WSL), read at each game frame: the stable measure;
- **EE CPU per game frame**: the game thread's own CPU time (`CLOCK_THREAD_CPUTIME_ID`). **Not stable across runs on a shared
  machine**: the same baseline binary gave 11.72 ms (`base1`, three other workers busy) and 7.48 ms (`base2`, later); compare CPU
  only between runs made at the same time;
- **lookups**: `PS2Runtime::lookupFunction` calls (each dispatch through the function table);
- **scheduler entries**: calls into guest code from the EE scheduler's loop, i.e. how often the host call stack unwound to it.

The game runs at 30 game frames a second on the PC in this window, so per-frame numbers compare runs directly. GameThread
sample shares are of that thread's samples.

## Baseline (the main checkout's state, plus the counters)

Function-table lookups 6,368-6,896 and scheduler-loop entries (stack unwinds) 477-516 per game frame (three runs). GameThread
(`base1`): eegen guest code 41.2 %, libc 27.3 %, scheduler and runtime 9.1 %, **PS2Recomp guest code 7.3 %**
(`FUN_002c4fe0` alone 3.4 %), PS2Memory 5.4 %.

Why PS2Recomp's code still ran: eegen entered only at function starts and at its 3,210 loop heads. A checkpoint inside a callee
(a loop's `EG_LOOP_DUE`, or the charge `dispatchGuestBranch` makes at every indirect call) returns all the way to the scheduler;
the guest then comes back to each caller at its return address, where only PS2Recomp had an entry, and the rest of that caller
(and of every caller above it) ran in PS2Recomp's slower code, which also checkpoints at every loop iteration.

## What changed

1. **eegen entries at call and syscall return points** (`eegen.py --return-resumes`, on in `runner/gen.sh`;
   `PS2PORT_EG_RETURN_RESUMES=0` for the old code). Every `jal`, `jalr` and `syscall` whose next instruction lies in the same
   function gets a case in the function's resume switch, reached through `EG_xxx_r` from the function table (36,347 return points
   in 5,579 functions, plus the 3,210 loop heads). Only the registers live at that point are loaded from the context (the data
   flow's `lin`, which also covers registers written back later without being set first). Return points that are another
   function's start, or already a loop head, are left out. `native_hooks.cpp` installs them with the loop heads
   (`PS2PORT_NO_EG_RESUME=1` still turns all resumes off).
2. **Calls through the function table inline** (`eegen.py --fast-dispatch`, on in `runner/gen.sh`, `PS2PORT_EG_FAST_DISPATCH=0`
   off): `eegen_rt.h`'s `eg_dispatch` does what `dispatchGuestBranch` does for a call to a function in the table (the 8-cycle
   checkpoint charge, the call, the stop and return-address checks), without its debug history, `PS2PORT_CALLCOUNT` counts and
   the replay recorder's hooks; anything else still goes to `dispatchGuestBranch`. 95 functions use it.
3. **Counters** (sandbox runtime, globals only, no class layout change): `[eework]` line in `ps2_runtime.cpp`, game-frame
   count, EE CPU time and user-mode instructions in `Syscalls/Sync.cpp`, scheduler entries in `EeScheduler.cpp`. Patch:
   `D:\ps2port-data\workers\ee-runtime.patch`.

## Results (PC)

**A/B made at the same time on the same machine** (`scripts/ee_ab_build.sh`: one runtime, eegen with both changes off and on;
`abbase`, `abnew`; window 200-260 s, 1,733-1,735 game frames each):

| per game frame | before | after | change |
|---|---|---|---|
| **EE user-mode instructions (hardware counter)** | 28.15 M | **25.50 M** | **-9.4 %** |
| GameThread samples | 1.84 | **1.66** | **-9.9 %** |
| GameThread samples outside libc | 1.27 | **1.01** | **-20 %** |
| EE CPU (same-time runs) | 9.25 ms | 8.36 ms | -9.6 % |
| function-table lookups | 6,368 | **187** | -97 % |
| scheduler-loop entries (stack unwinds) | 477 | **129** | -73 % |
| PS2Recomp guest code, share of GameThread | 6.0 % | **0.0 %** | |
| GIF packets / KB / kicks (same work drawn) | 5,081 / 7,676 / 135,144 | 5,084 / 7,675 / 135,140 | same |
| reached play (mode 5) | 185.9 s | 183.1 s | |

Steps measured separately (earlier, separate runs; counters only, since CPU time varied with the other workers' load):

| per game frame | base1 | + return resumes (`ret1`) | + fast dispatch (`fd1`, `fd2`) |
|---|---|---|---|
| lookups | 6,837 | 1,814 | 193 / 179 |
| scheduler entries | 516 | 131 | 135 / 121 |
| PS2Recomp share of GameThread | 7.3 % | 0.0 % | 0.0 % |

The libc share of the thread rises (31 -> 39 %) only because the rest shrank: libc samples per frame stayed about the same.

## What the profile shows next (PC; device shares differ)

- **libc, 27 % of the game thread on the PC, is mostly futex wakes, not copies:** 15 % of the thread's samples sit right after the
  `syscall` in `pthread_cond_broadcast` (a `notify_all` with sleeping waiters), 4 % in `pthread_cond_signal`
  (`scripts/ee_lib_pcs.py`). About 1.5 ms of EE CPU a game frame on WSL. 200 stack
  snapshots of the game thread in play (`PS2PORT_STACKS`, 200-240 s) caught it 16 times in a wake: `ps2portFrameFlip`
  (`ps2_memory.cpp` ~382, `workCv.notify_one` at every flip) 6, `ps2portEnqueueGsWork` (~2549, `workCv.notify_one` per job) 5,
  `IopEmulator::runEeCycles` (`iop_emulator.cpp` ~1130, from `EeScheduler::accountCycles`) 4, `ps2portGsAcquire` (~2372,
  `idleCv.notify_all`, reached from `mapVuMemory` on an EE read of VU memory) 1. Proposal for the GS and IOP workers: wake a
  worker only when it is asleep (an `idle`/`waiting` flag under the lock, as `ps2portIopRunAhead` already does), and batch GS jobs.
  On the device a futex wake is cheaper, but it is still a syscall per hand-off.
- The hottest guest functions are all eegen's now: `EG_002d7878` 4.5 %, `EG_002df7f0` 4.3 %, `EG_00309414` 3.6 %.
- `PS2Memory::writeIORegister` 1.9 %, `translateAddress` 0.9 %, `inRange` 0.8 % (GS worker's files).

## Tried and dropped

- EE CPU time as the measure: dropped for the hardware instruction counter (see above).
- Nothing else was tried: the budget went to the two changes, their validation and the A/B.

## Risks

- Return resumes enter a function in the middle with registers from the context. That is the same contract as the loop-head
  resumes (Phase 7 step 3), now at 36,347 more places. The replay harness runs without checkpoints, so it does not exercise them;
  the PC boot to play and the sound run do. A wrong live set would show as a wrong register after a resumed call.
- `eg_dispatch` skips `pushDispatchPc`: a "No exact recompiled function" report lists fewer recent dispatches;
  `PS2PORT_CALLCOUNT` no longer counts calls eegen makes through the table. Build with `PS2PORT_EG_FAST_DISPATCH=0` to debug those.
- Code size: the runner grew from 101.85 MB to 105.08 MB (x86, unstripped) with the resume switches.
- Not run on the device (no device access in this phase): the device gain is unmeasured.
