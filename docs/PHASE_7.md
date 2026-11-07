# Phase 7

This file starts with step 2. Everything before it in this repository (past New Game, the GS thread, the IOP thread, speeding up the game thread, the DMA chain copies) is recorded in `docs/PHASE_4.md`, from "Past New Game" on. The stop rule for this phase is in `CLAUDE.md`: a measured gameplay frame of <=50 ms continues, 50-55 ms reports and asks, >55 ms stops with options.

Starting point (`panel-225927`, the shop with Harry in play): 60.1-60.3 ms a frame; game thread 83 %, GS thread 82 % (89 % of its time in jobs, about 4.5 ms a frame of it waiting for the EE to run VIF1 handlers).

## Step 2: the GS thread in two stages

### Design (written before implementing)

1. **Stage A** (`GsThread`, the existing worker) keeps GIF and VIF1 jobs: VIF1, VU1 and the GIF arbiter. The arbiter's output (each GIF packet, in arbiter order) no longer goes to `GS::processGIFPacket` but is copied into a batch for stage B.
2. **Stage B** (`GsDraw`, new) takes the batches in order and runs `GS::processGIFPacket` on each packet: GS parsing and recording into the GLES backend. GL itself stays on the main thread (`LatchFrame`/`Present` under the frontend's lock, unchanged).
3. **Queue:** batches of packets, about 64 KB each, flushed when full, at the end of each stage-A job, and before stage A waits for the EE. Bounded at 16 MB of packet bytes queued, roughly three frames; stage A waits when it is full. Copying is needed because VU1 reuses the memory it kicks from.
4. **Order:** one ordered queue. The EE can only produce packets while stage A is stopped (a VIF1 handler it runs in place) or idle (FIFO writes), and it pushes its packets at once, so their order is kept.
5. **What the EE waits on:**
   - VIF1 and GIF registers, their FIFOs and VU1 memory: **stage A idle**, as now. Stage B may still be drawing.
   - Direct use of the GS frontend from EE stubs (`Kernel/Stubs/GS.cpp`: clears, store-image readback, draw environments): **both stages idle** (new `ps2portGsAcquireAll`).
   - GS privileged registers: no wait, as now.
6. **VIF1 interrupts:** stage A flushes its batch, then stops until the EE has run the game's handler, as now. **Stage B keeps drawing the queue meanwhile**, so the ~4.5 ms a frame of waiting should overlap with stage B's work instead of adding to it.
7. **Expected (estimate):** stage A gets VU1 (33 % of today's GS thread), VIF1 (12 %) and the copies, roughly 25-30 ms a frame; stage B gets GS parsing and recording (43 %), roughly 25-30 ms. The game thread (about 50 ms of work a frame) would then be the limit, with a floor near 50 ms.
8. `PS2PORT_GS_DRAW_THREAD=0` keeps one stage. `[m5t]` reports stage B's busy time.

### Result (`panel-234328`, the shop with Harry in play, 1.416 GHz)

**In the shop from 226 s on: 48.2-48.9 ms a frame (20.4-20.8 a second), was 60.1-60.3 ms. Measured.** The table says continue (<=50 ms). At 211-221 s the CPU zone reached 77-79 C, the cooling state rose to 1-2 for those seconds (the clock reading stayed 1,416 MHz) and frames were 51.8-52.7 ms; from 222 s the state was 0 again and frames 48.2-48.9 ms. Peak 78.6 C. The PC boot reaches the cutscene with the same picture as before the split (`display_nz` 85,975 both).

| Thread (226-261 s) | CPU, measured | Per frame (48.5 ms) | |
|---|---|---|---|
| Game thread (EE) | 98 % | about 47.5 ms | measured share, times the frame |
| Stage A (`GsThread`: VIF1, VU1, arbiter) | 59-67 % | in jobs 37.5 ms, of which 4.1 ms waiting for the EE | measured (`[m5t]`) |
| Stage B (`GsDraw`: GS parsing, recording) | 47-55 % | 16.5 ms inside its batch loop | measured; the two measures disagree, and the reason was not established |
| Main thread (GL) | 32-37 % | 15.5-18 ms | measured share |
| IOP thread (the silent SPU2 runs inside it) | 13 % | about 6 ms | measured share |

Per core (`scripts/core_load.py`, 226-257 s): cpu0 58 %, cpu1 71 %, cpu2 71 %, cpu3 61 %, 261 % of 400 % in all, matching the thread sum. Clock 1,416 MHz throughout, cooling state 0.

- **The limit is the game thread** (98 % busy). The EE waits on the GS side once a second now, not once a frame (`waits` 1/s, was 16-17), and the queue was never full.
- **The ~4.5 ms of VIF1 hand-offs now overlap:** stage A still waits about 4.1 ms a frame for the EE to run handlers, but stage B keeps drawing the batch flushed just before, and stage A has slack (it spends 77 % of the time in jobs, including those waits).
- **Expected floor (estimate):** about 47-48 ms, the game thread's own work, until that work shrinks. The GS stages have room: stage A about 33 ms and stage B well under 30 ms of work per frame.

### What would cut the game thread's work (about 47.5 ms a frame)

From the last profiles of the game thread (`docs/PHASE_4.md`, "Speeding up the game thread" and "DMA chain copies"); shares are of the game thread, savings are estimates:
- **DMA chain copies, about 14.7 % (about 7 ms):** a VIF1 interpreter that streams over the chain's pieces (PS2Recomp's stops at a VIF code cut by a buffer end) would remove them, with DMA completion reported when stage A has read the data.
- **Re-entries in PS2Recomp's code, part of its 12.8 %:** eegen entry points at its loop heads, so a function resumed after a checkpoint stays in eegen's code. The checkpoint exits come mostly from the 40 hand-off wakes a frame.
- **Scheduler and runtime, about 10 %:** `checkpointDue`, `lookupFunction`, `dispatchGuestBranch`, `writeIORegister`.
- **The remaining 15 refused functions,** and the hottest eegen functions (`EG_002d7878`, matrix work, 5-6 %) with float groups (off in the runner; Phase 3 measured 2 %).

## Step 3: cutting the game thread's work

The user's request: "Cut EE-thread work". **In the shop: 48.2-48.9 -> 45.1-45.5 ms a frame (22 a second), measured. The game thread is no longer the limit; stage A is.** The table says continue (<=50 ms).

### What changed

1. **VIF1 chains are copied by stage A, not the EE.** At the kick the EE only notes the chain's pieces; stage A copies them before running VIF1. Until it has, a read of D1_CHCR reports the channel busy and the completion (D_STAT, the DMAC handler cause) is not booked, as with the hardware's own DMA. The EE books it when it reads D1_CHCR or D_STAT, or at its next checkpoint after stage A's wake, and drains the DMAC handlers there. The EE never found the channel busy in play (`D1_CHCR reads busy` 0). Measured on its own: 48.2-48.9 -> 46.1-47.7 ms; libc went from 22 % to 8.7 % of the game thread (`panel-000951`).
2. **eegen resume entries.** A function with loop checkpoints gets `EG_xxx_r`; resumed at a loop head, it reloads its cached registers from the context and jumps there, instead of falling into PS2Recomp's re-entry code. 3,206 loop heads (`PS2PORT_NO_EG_RESUME=1` turns them off).
3. **eegen borrows delay-slot instructions too** (PS2Recomp's copy between `ctx->in_delay_slot = true` and `false`): 5,574 of 5,708 functions translate, among them `2cfa30` (7.2 % of the game thread before) and `2df7f0`. Harness, whole program on x86: **0 RDRAM bytes differ, exactly 2,861,449 guest instructions**, v0 as without borrowing. The harness runs without checkpoints, so it does not exercise the resume entries; the PC boot and the device runs do.

### Numbers (`panel-004329`, the shop, 227-258 s, 1.416 GHz, cooling state 0, CPU zone 72-73 C; peak 74.7 C)

| Thread | CPU, measured | Per frame (45.3 ms) |
|---|---|---|
| Game thread (EE) | 92 % | about 41.7 ms (was 47.5) |
| Stage A (VIF1, VU1, arbiter, now the chain copies) | 85 % | 43.2 ms in jobs (95 % of the time), 4.7 ms of it waiting for the EE |
| Stage B (GS parsing, recording) | 37 % | 17.8 ms in its batch loop |
| Main thread (GL) | 24 % | about 11 ms |
| IOP thread (with the silent SPU2) | 15 % | about 7 ms |

Per core: 66, 70, 70, 73 % busy.

- **The limit is now stage A** (95 % of its time in jobs). Its time: translated VU1 59 %, `processVIF1Data` 18 %, the chain copies 14.5 %, unpacking 4.5 %. **Expected floor with today's stage A: about 43 ms (estimate).**
- The game thread now: eegen guest code 46 %, libc 12.6 %, scheduler and runtime 12.1 %, PS2Memory 9.8 %, PS2Recomp guest code 8.1 % (was 18 %; most of what is left is `FUN_002c4fe0` resumed at a call's return address, which the loop-head entries don't cover).

### What would go further (estimates)

- **Stage A:** the chain copies to a stage of their own before it (about 5.5 ms a frame; the cores are 66-73 % busy); then VU1's translated code, the largest single item (about 22 ms).
- **Game thread:** resume entries at call return addresses too (`FUN_002c4fe0`, 5 %), the tag walk (`writeIORegister` 4 %), scheduler and runtime (12 %). These shorten the frame only once stage A does.

## Wrap-up (2026-11-08): coverage, soak, game speed, plans, controls

### Coverage (`panel-012350`, the full scripted path, 1.416 GHz; measured, `scripts/frame_stats.py`)

| Stretch | p50 ms | p95 ms | max ms | CPU zone | cooling max |
|---|---|---|---|---|---|
| **Gameplay, the shop (225-280 s)** | **43.6** | **54.1** | 65.2 | 73.4-75.1 C | 0 |
| Intro (report only) | 38.3 | 70.6 | 545.5 | | 0 |
| Menu | 47.7 | 50.7 | 69.8 | | 0 |
| Loading | 33.8 | 48.5 | 122.7 | | 0 |
| Quirrell cutscene (report only) | 36.8 | 66.9 | 434.5 | | 0 |
| Street / Hagrid (report only) | 38.9 | 67.8 | 327.8 | | 1 |
| Ollivanders | 34.3 | 53.9 | 83.2 | | 0 |
| Shop cutscene (report only) | 49.9 | 64.0 | 79.3 | | 1 |

Gameplay p50 43.6 ms: under the 50 ms line. The large maxima are loads and scene changes. Memory at the end: anon 150 MB, file 257 MB, Mali mappings 27 MB.

### Soak (`panel-013121`, the shop, 1.416 GHz; measured)

Planned 1,720 s (idle, then a scripted walk from 900 s). **The guard stopped it at 85 C after about 806 s**, so the walk never ran. Per 120 s:

| s | p50 ms | p95 ms | CPU zone | cooling max | anon / file / Mali MB |
|---|---|---|---|---|---|
| 0-120 (boot, menu) | 33.6 | 61.0 | 56.8-70.7 C | 0 | 129 / 186 / 27 |
| 120-240 | 42.0 | 64.4 | 68.0-77.7 C | 2 | 145 / 262 / 27 |
| 240-360 | 43.5 | 52.7 | 76.6-79.9 C | 1 | 145 / 264 / 27 |
| 360-480 | 43.5 | 54.3 | 79.0-81.5 C | 1 | 145 / 261 / 27 |
| 480-600 | 43.5 | 52.9 | 80.5-82.7 C | 2 | 145 / 262 / 27 |
| 600-720 | 43.5 | 55.0 | 81.8-83.9 C | 2 | 145 / 262 / 27 |
| 720-806 | 43.5 | 54.0 | 82.8-85.3 C | 2 | 145 / 262 / 27 |

- **No drift and no leak in 13 minutes:** p50 flat at 43.5 ms, the clock never under 1,416 MHz (cooling states 1-2 did not lower it), anon memory flat at 145 MB, the Mali mappings flat at 27 MB.
- **Heat is the limit:** the CPU zone climbs about 2 C a minute after the first five minutes and passes 85 C at about 13 minutes in this run. The kernel's passive trips (60/70 C) did not lower the clock.
- **Guard bug, fixed:** the guard's stop reached the shell but not the runner, which ran on at the default clock until it was killed by hand (87.4 C at that point, no reboot, uptime 4.5 h). `device_guard.sh` now starts the command in its own session (`setsid`), stops the whole group (TERM, then KILL 3 s later) and kills what outlived it after a timeout. Checked on the device: no process left over.

### Game speed (from the code; inference, not measured against a clock)

The game counts vblanks in its handler (`0x2cd788`, count at `0x328490`) and waits for 2 per frame (`0x2cd6a0`, the number at `0x3A4FD8`). The frame end (`0x2e2268`) stores the vblanks that passed (`0x3287F0`), and two functions read it (through `0x2e2a20` and `0x2ea250`): the game can scale by elapsed time. But the runtime's vsync follows charged guest cycles, and the EE never idles, so **the game always sees 2 vblanks a frame: 33.3 ms of game time for about 45 ms of real time, about 73 % speed** (slow motion, not dropped steps). Making vsync follow real time would show it 3 vblanks a frame; whether the game then steps further (variable step) or keeps its pace is not checked.

### Plans

`docs/PLAN_INPUT.md` (input, now implemented, below) and `docs/PLAN_AUDIO.md` (audio; plan only).

### Controls (user's request, 2026-11-08)

- `runner/pad_sdl.cpp`: the raylib gamepad calls `ps2_pad.cpp` makes (`IsGamepadAvailable`, `IsGamepadButtonDown`, `GetGamepadAxisMovement`), over SDL2's GameController API loaded at run time (`bench/src/sdl_dyn.c`, optional symbols). SDL on the main thread (drained every displayed vsync); the EE thread reads atomics. Positional mapping as in the plan; L2/R2 from the trigger axes; sticks with a 12 % dead zone.
- **On the device (measured, `play-015720`):** SDL 2.30.12 has no built-in mapping for the "Anbernic RG40XX-H Controller"; the runner then loads PortMaster's Knulli `gamecontrollerdb.txt` (526 mappings) and opens it as `ANBERNIC-keys` (`a:b4, b:b3, x:b5, y:b6`, D-pad on hat 0, two sticks). **Not checked yet: which physical button is SDL's `a`** (needs the user's hands; `PS2PORT_PAD_SWAP_FACE=1` swaps Cross/Circle and Square/Triangle). Every change is logged (`PS2PORT_PAD_LOG=1`).
- Quit: Menu + Start, or L1 + R1 + Select + Start, held for a second; SIGTERM too (SDL turns it into a quit event, handled now).
- Play: `sh scripts/play.sh [MINUTES [VAR=VALUE...]]` (`runner/device_play.sh` on the device, under the guard: 1.416 GHz, 85 C).
