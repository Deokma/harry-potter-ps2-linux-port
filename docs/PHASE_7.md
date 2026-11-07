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
