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
| Sound | 55 % | Real-time speed on the PC at the device's EE pace (47-48 k of 48 k samples a second; the device measured 24-28 k before the fix); not yet run on the device. Movie sound silent. |
| Movies | 60 % | Pictures play on the PC (logos, menu background, trailer), read from the disc folder because EA's stream stalls; movie sound silent; not run on the device. |
| Frame time | 50 % | 55-67 ms in Ollivanders with the full picture (15-18 a second). |
| Heat | 40 % | 85 C after 5-13 minutes at 1.416 GHz; the session stops there. |
| Launching | 50 % | `sh scripts/play.sh` over SSH works; the Ports launcher is written but has not run on the device. |

**2026-11-13, Phase 9 (`docs/PHASE_9.md`):** less work per frame on the EE (-9.4 % instructions), GS parsing, the IOP and in the
waits, counted on the PC; play binary `7a0e74dc`, not yet run on the device. Frame time and heat rows above are still the last
device measurements. First thing next: the user's device test, then the CPU clock runs (`docs/PHASE_9_HEAT.md`, section 7).

**2026-11-13, first device runs of `7a0e74dc` (user):** Ollivanders 40-42 ms standing, 44-66 ms moving (was 55-67); no 85 C
stop in 7 minutes at 1.416 or 1.32 GHz (peak 73 C); sound rate 48 k of 48 k; streams still starve and end, more often at
1.32 GHz. Details: `docs/PHASE_9.md`, "Device results".

## What to do next, in order

1. **The user's tests** (device): stick, lag, Menu + L1, `docs/VISUAL_CHECKLIST.md`, `sh scripts/install_port.sh` and the Ports
   entry. Read the play log's `[lag]` and `[pad]` lines afterwards.
2. **Cutscenes that never end:** the stream race fixed, and a starved dialogue stream now ends instead of looping
   (`PS2PORT_SPU_STARVE`); PC at the device's pace goes through all New Game cutscenes. Device check pending. Why EA's
   `STREAM.IRX` stops feeding a dialogue is still unknown (`docs/PHASE_8.md`, "Dialogue that loops").
3. **Frame time** (device): profile the shop (`PS2PORT_SAMPLE`), the game thread first.
4. **Movies:** pictures play (PC); left: the user's visual check, decode cost on the device, movie sound, and why EA's
   `STREAM.IRX` stops refilling (`docs/PHASE_8.md`, "Movie pictures on screen").
5. **Play through**, chapter by chapter, from saves.
6. **Heat:** sessions at 1.2 and 1.0 GHz.

## How to pick up

Read `NOTES.md`, "Current state and handoff", then `docs/PHASE_8.md` from "Saves work" on. Running: `docs/RUNNING.md`. Every
switch: `docs/ENV_VARS.md`. Device rules: `CLAUDE.md` (frame table, 85 C stop, no screen control, no pushes, visual checks are
the user's).
