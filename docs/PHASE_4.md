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
