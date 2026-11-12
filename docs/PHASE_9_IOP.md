# Phase 9, worker IOP: where the IOP's and the mixer's work goes (2026-11-12, PC only)

Scope: the emulated IOP (the game's own IRX modules: LIBSD, SDRDRV, EA's STREAM.IRX, CDVD) on `IopThread`, the SPU2 mixer, the SDL
audio output, and the EE<->IOP hand-offs. **Every number here is from the PC** (WSL, x86 runner, `PS2PORT_SLOWDOWN=60` so the EE
runs at about the device's pace, three other workers loading the machine). Counts per second carry over to the device; host times
(ms/s) and samples do not. Nothing here ran on the device.

## Tools added

- `PS2PORT_IOP_COUNTERS=1` (sandbox `ps2xIOP/src/emulator/iop_emulator.cpp`, `core/iop_memory.cpp`): `[iopc]` lines every 5 s of
  host time, rates a second over that stretch: IOP instructions (counted, and interpreted) and cycles (and the share skipped as
  idle), batches, timeslices, idle skips, interrupts, instructions in timer / DMA-interrupt / guest callbacks; instructions by
  module, by IOP thread, by hot 256-byte stretch; import calls by library:ordinal; mixer calls, mixing steps, frames,
  voice-frames, ADPCM decodes, hardware reads, SPU status refreshes, SPU register writes, SPU DMAs; EE guards, catch-ups (how many
  waited, IOP cycles each, ms/s waited), IOP lock contention on both sides, `onEeCycles` calls and lag waits, run-aheads, reply
  guards, IOP idle waits; host ms/s in `runCycles`, thread timeslices, mixing, the PCM capture, SPU status refreshes; native LIBSD
  calls and fallbacks. Unset: one predictable branch per place.
- `scripts/iop_counters.py LOG FROM TO`: the mean of every `[iopc]` figure over a stretch of a run.
- `scripts/iop_disasm.py MODULE.IRX FROM TO`: a small R3000 disassembler (WSL has no MIPS objdump) for the hot spots the counters name.
- `scripts/iop_mixer_test.cpp` + `scripts/iop_mixer_test.sh`: a mixer unit test. It drives `iop_memory.cpp` alone through a fixed
  pseudo-random script (sample DMA, about 1,200 key-ons, key-offs, pitch, ADSR, volumes, refills, status reads, irregular clock
  steps; 168 s of sound) and compares the old file's output with the new one's byte for byte.

## Counter table (PC, device pace, gameplay in Ollivanders)

Before: run `iop-base2` (`boot-20261112-191323`, play at 226.9 s), means over 232-326 s; host times from `iop-base3` (play at
221.7 s). After: run `iop-check4` (`boot-20261112-232843`, play at 217.8 s), means over 236-326 s. Both with the counters on.

| | Before | After |
|---|---|---|
| IOP instructions interpreted / s | 727,000 | **560,000** (the same guest work counts 779,000 with the native LIBSD bodies: **28 % fewer interpreted**) |
| of them STREAM.IRX / LIBSD.IRX | 476,000 / 252,000 | 515,000 / 45,000 |
| IOP thread 7 (STREAM.IRX main loop, +0x3474), counted | 674,000 (93 %) | 721,000 (93 %) |
| IOP cycles / s (real time is 36.9 M) | 36.9 M, 98.0 % skipped as idle | 36.9 M, 97.9 % skipped as idle |
| import calls / s | 7,890 (libsd:10 sceSdGetAddr 4,315, libsd:6 sceSdGetParam 2,920) | 8,230 (4,546, 2,984) |
| native LIBSD calls / s (fallbacks to the guest code) | - | 6,560 (970: a call that would cross a timeslice end) |
| batches / timeslices / idle skips per s | 9,270 / 2,700 / 9,190 | 9,470 / 2,890 / 9,370 |
| instructions / s in timer callbacks, DMA interrupts, guest callbacks; interrupts taken | 960, 65, 7; 0 | 960, 170, 20; 0 |
| mixer: calls / mixing steps / frames per s | 51,000 / 1,800 / 48,000 | 53,000 / 1,800 / 48,100 |
| voice-frames / s (voices sounding per frame) | 10,000 (0.21) | 24,600 (0.51: a line of dialogue in this run) |
| ADPCM decodes, SPU status refreshes, SPU writes, SPU DMAs per s | 330, 41,800, 360, 0.6 | 810, 43,800, 380, 1.7 |
| EE guards / s; catch-ups that waited | 55; 27, about 620,000 IOP cycles each | 59; 8, about 330,000 each |
| EE game thread waiting in catch-ups (host) | **125-146 ms/s** | **3-11 ms/s** |
| IOP lock contended (EE side / IOP side) | 0 / 0 per s | 0 / 0 per s |
| `onEeCycles` / s, lag waits | 645, 1 | 690, 0 |
| host ms/s in `runCycles` | 250-340: the PCM capture 210-290, mixing proper about 8, interpreting 37-62 | **43**: capture hand-over 3, mixing proper about 6, interpreting 30 |
| IopThread samples (`PS2PORT_SAMPLE`, 90 s of play; GameThread 16,893 / 18,264) | 1,697 (51 % libc: the capture) | **823 (-51 %)**; without libc 826 -> 639 (-23 %) |

Validation (PC): New Game with no input, `HPM02 -> HPM03 -> HPM04A -> mode 2 -> mode 5`: before at 221.7 / 226.9 s (two runs; a
third, `iop-base`, was still in mode 2 at 260 s on the loaded machine), after at 229.3 s (`iop-check`, before the capture thread)
and 217.8 s (`iop-check4`). Mix rate in play 47.6-48.2 k of 48 k frames a second in every 10-s window (100 %). Loudness in play
rms 800-3,000 during the dialogue, 0 in the quiet shop, as before. The starved-stream rule fired 3-6 times a run (before: 2-8).
**Full PC speed (no slowdown, counters off, `iop-fast`): play at 183.6 s, mix 100 %.** Check mode (`iop-check3`, play at 227.6 s):
1,835,008 native LIBSD calls compared with the guest code (727,158 GetParam, 1,107,850 GetAddr), **0 mismatches** in registers and
instruction counts.

What the counters say:

1. **The IOP interprets little.** 98 % of its cycles are skipped to the next event (timer, wake-up, DMA end); the scheduler
   already did that, so idle loops are not interpreted. What is interpreted is almost all one thread: EA's `STREAM.IRX` main loop
   (thread 7), which on every timer tick asks LIBSD about every voice (`FUN_0000a498`: `sceSdGetParam(ENVX)` for 48 voices;
   `FUN_0000f5a8`: NAX through `sceSdGetAddr`). A third of all IOP instructions were those two small LIBSD functions.
2. **The mixer is cheap.** In Ollivanders 0.2-0.5 voices sound per output frame; mixing proper is 6-9 ms/s of PC time.
3. **On the PC the capture file was the IOP thread's biggest cost**: `runner/try_sound.sh`'s `PS2PORT_SPU_PCM` wrote to `/mnt/d`
   (WSL's 9p) from the mixer, 210-475 ms of every second, inside the IOP lock. The EE's catch-up waits (125-146 ms/s of the game
   thread) were mostly that: the EE waited for an IOP thread stuck in `fwrite`. Every earlier PC sound/timing run carried this; the
   device never captures. The PC results of the previous sessions (play reached at 215 s and so on) are still right: they
   included the same cost.
4. **Per interpreted instruction the interpreter is slow**: 30-60 ms/s of PC time for 0.56-0.73 M instructions, 50-85 ns each
   (`executeInstruction` 13 %, `step` 12 %, `read32` 9 % of IopThread samples after; `std::unordered_map` lookups for hardware
   registers 4.5 %).
5. **Loading is different**: in the New Game load the IOP got only 20-21 M cycles a second on the PC and its timeslices took
   400-490 ms/s for 0.9 M instructions: the time is in imports done natively inside the timeslices (CD reads from the disc image on
   `/mnt/d`, guest callbacks), not in interpretation. A PC artifact in part (9p reads); not looked at further.
6. **SDL audio output** (`runner/audio_sdl.cpp`): one callback per 1,024 frames (47 a second), a linear resampler over ~1,000
   frames; well under 1 % of a core by inspection. Not opened in PC runs (no display), so not counted; unchanged.

## What changed (sandbox PS2Recomp; patches `D:\ps2port-data\workers\iop-iop.patch`, `iop-runtime.patch`)

1. **LIBSD's `sceSdGetParam` and `sceSdGetAddr` in native code** (`iop_emulator.cpp`, `ps2portLibsdNative`). Same memory reads
   in the same order (the register table in LIBSD's data, then the SPU2 register; for NAX the double read), every register the
   guest code leaves (at, v0, v1, a0-a3, hi, lo) set the same, and the same number of instructions and cycles charged, so IOP time
   runs exactly as before. Used only when the function's code matches the one it was written from (relocated words masked; logged
   once as `[iop-hle] libsd:N at ADDR: native`), no load is pending, the call fits in the current timeslice, and no DMA interrupt or
   scheduled callback falls due inside it; otherwise the guest code runs. `PS2PORT_IOP_HLE=0`: off. `PS2PORT_IOP_HLE=2`: check
   mode (each call also runs the guest code on a copy of the CPU and compares; `[iop-hle] checked N ..., mismatches M`).
2. **Import stubs found without a call per instruction**: `step()` tests the word for `jr ra` (decode's own first test) before
   calling `IopImportRegistry::decode`. Same behaviour.
3. **The mixer's frame loop visits only the sounding voices** (`iop_memory.cpp`), in the same order; silence when none. Unit test:
   8,074,616 frames, 98.7 % of the samples non-zero, **old and new output identical** (md5 `0b742648adb34f9754eab8bb230ec12b`).
4. **The PCM capture is written by a thread of its own** (`PS2PORT_SPU_PCM`, PC tests): the mixer only appends to a buffer.
   `try_sound.sh` still reads the mix rate from the file's size (flushed after every write).
5. `PS2PORT_IOP_COUNTERS` (above).

## Tried and dropped

- **A 32 KB stdio buffer for the capture** instead of the thread: the capture cost rose to 400-475 ms/s (9p's cost is per byte,
  and the machine was loaded); replaced by the writer thread.
- **Larger IOP batches while the IOP is far behind** (to shorten the EE's catch-up waits): not done. Once the capture left the IOP
  thread the waits fell to 3-11 ms/s, and batch boundaries are where replies from the EE get in (the STREAM.IRX race, the 8 ms
  reply bound): not worth the risk without a device measurement.
- **A flat array for the SPU2 registers** instead of `std::unordered_map` (4.5 % of IopThread samples): a header change
  (`iop_memory.h`, class layout) for a small gain; left.

## Why not more, and what would cut more

- After this the interpreted work is EA's `STREAM.IRX` loop itself (515,000 instructions a second: `+0xa500` 83 k, `+0x10f00`
  60 k, `+0x4600` 44 k, `+0x12e00` 32 k). Cutting it means either a faster interpreter (a decoded-instruction cache per address,
  inlined RAM reads, the per-step `checkInterrupt` / `takeDmaStart` / queue checks folded into one event check: perhaps 2x per
  instruction) or native versions of the module's voice-poll functions (`FUN_0000a498`, `FUN_0000f5a8`, `FUN_00010fa4/fc4`),
  game-specific and riskier.
- On the device the IOP thread was 10-14 % of a core before the mixer existed (Phase 4, `panel-161110`); its share now is not
  known. **The first thing to measure on the device is one play session with `PS2PORT_IOP_COUNTERS=1`**: the hand-off line shows
  how long the game thread waits on the IOP there.

## Risks

- The native LIBSD calls depend on this LIBSD.IRX's code (checked word by word at the first call; another version falls back to
  the guest code and says so). The PAL disc's LIBSD has not been checked.
- An IOP interrupt can no longer be taken in the middle of these two functions (none is taken in play: 0 a second). DMA interrupts
  and scheduled callbacks due inside a call make it fall back, so their timing is as before.
- The counters cost a `std::map` insert per import and a clock read per timeslice when on; nothing when off.

## Proposals for other workers' files (not edited)

- Heat / device scripts worker: add `PS2PORT_IOP_COUNTERS=1` to one device play or profile run (`scripts/play.sh` or
  `scripts/device_panel.sh` environment) and read the `[iopc] hand-off` and `[iopc] host` lines from the play log; with
  `PS2PORT_SAMPLE`, compare IopThread with `private/therm/prof1`.
- EE worker: nothing needed from this side. If the device shows catch-up waits like the PC's before (100+ ms/s), the place is
  `Ps2portIopGuard::Ps2portIopGuard` (iop_emulator.cpp, mine) and the `PS2PORT_REALTIME` jumps that raise the IOP's target in
  whole frames (`ps2_runtime.cpp`, EE worker): spreading the real-time catch-up over the frame instead of one jump would let the
  IOP thread keep up instead of the game thread waiting for it.
