# Where the port stands (2026-11-10)

Harry Potter and the Philosopher's Stone (PS2, NTSC SLUS-20826) on the Anbernic RG40XX H. The public summary is
`docs/PROGRESS.md`; this page is for picking the work up. Percentages are estimates; what was measured says so.

## In one line

**About 65 % of the technical work is done (estimate); about 2-3 % of the game's content has been seen on the device** (boot to
the first playable room, Ollivanders). Graphics, controls, loading, saving and game speed work; sound speed, frame rate, heat,
movies and the later chapters are left.

## By part

| Part | Done (estimate) | State |
|---|---|---|
| Game code (EE: 5,708 functions) | 95 % | Native; R5900 semantics since 2026-11-10 (the stick fix). Replay-checked against PCSX2 with PS2Recomp's semantics. |
| Vector units (VU0, VU1) | 95 % | Translated to native code; checked against recordings. |
| Graphics (GS on GLES) | 80 % | Right in every scene seen; palette textures with the linear filter now one fetch (fog screen 61 -> 21.6 ms of GPU). |
| IOP, disc, streaming | 85 % | The game's own drivers on an emulated IOP on its own thread, in step with the EE. |
| Controls | 95 % | SDL2, PS2 layout by position, sticky taps, at most one frame of lag; the stick fix is waiting for the user's test. |
| Saves | 90 % | Write, restart, Continue measured on the device. Save time shows 6 h off (clock stub's time zone). |
| Game speed | 95 % | The logic ticks at 25.0 a second (measured): real time. |
| Sound | 35 % | Plays, but at 24-28 k of 48 k samples a second (the IOP follows the slow EE): slow and low. |
| Movies | 60 % | Pictures play on the PC (logos, menu background, trailer), read from the disc folder because EA's stream stalls; movie sound silent; not run on the device. |
| Frame time | 50 % | 55-67 ms in Ollivanders with the full picture (15-18 a second). |
| Heat | 40 % | 85 C after 5-13 minutes at 1.416 GHz; the session stops there. |
| Launching | 50 % | `sh scripts/play.sh` over SSH works; the Ports launcher is written but has not run on the device. |

## What to do next, in order

1. **The user's tests** (device): stick, lag, Menu + L1, `docs/VISUAL_CHECKLIST.md`, `sh scripts/install_port.sh` and the Ports
   entry. Read the play log's `[lag]` and `[pad]` lines afterwards.
2. **Sound speed.** Find the IOP side's stream state (open streams) so the IOP can run on real time only while no stream is
   open, or resync without breaking a stream. Code: private `ps2xIOP/src/emulator/iop_emulator.cpp` (`ps2portIopGoal`);
   measure on the PC with `PS2PORT_SLOWDOWN=50 PS2PORT_SPU_PCM=FILE` (bytes / 4 / seconds = samples a second) and the New Game
   load time (mode 0 to 9: 38 s at half speed, 7 s at full).
3. **Frame time** (device): profile the shop (`PS2PORT_SAMPLE`), the game thread first.
4. **Movies:** pictures play (PC); left: the user's visual check, decode cost on the device, movie sound, and why EA's
   `STREAM.IRX` stops refilling (`docs/PHASE_8.md`, "Movie pictures on screen").
5. **Play through**, chapter by chapter, from saves.
6. **Heat:** sessions at 1.2 and 1.0 GHz.

## How to pick up

Read `NOTES.md`, "Current state and handoff", then `docs/PHASE_8.md` from "Saves work" on. Running: `docs/RUNNING.md`. Every
switch: `docs/ENV_VARS.md`. Device rules: `CLAUDE.md` (frame table, 85 C stop, no screen control, no pushes, visual checks are
the user's).
