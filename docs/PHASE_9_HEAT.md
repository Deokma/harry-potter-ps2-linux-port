# Phase 9, heat worker (2026-11-12): work that buys nothing, threads that poll, clocks

Problem: in play the 85 C guard ends a session after 5-13 minutes (target 30+ minutes; the 85 C stop stays, user's decision).
Heat is the total work of all four cores plus the GPU. This worker had **no device access**: every number below is either read
from existing device logs (`private/therm/`, said so) or measured on the PC (said so; PC numbers are not device numbers), or an
estimate (said so).

## 1. What the device logs already say (measured on the device, earlier runs)

| Where (log) | Per-core busy (`scripts/core_load.py`) | CPU zone | Notes |
|---|---|---|---|
| boot, menus, 0-90 s (r1, r2, prof1) | 37-43 % each (about 160 % of 400 %) | 52-77 C | |
| cutscenes, 100-200 s | 55-67 % each | 63-82 C | |
| play in the shop, 240-480 s (r1) | 68-71 % each (about 280 %) | 69.0-72.7 C | cool day |
| play in the shop, 240-1120 s (r2) | 60-73 % each | **83.2-86.7 C plateau** | warm start (64 C) |
| **game hung, no frames** (r1, 502-1199 s) | 11-17 % each (about 55 %) | 58-62 C | IopThread 20 %, main 4 %: the floor when the game does nothing |

Per thread in play (`[m5]` of prof1/r2 and `docs/STUDY_BUILD_OPT.md`): GameThread 90-97 %, GsThread (stage A) 79-83 %, GsDraw
(stage B) 28-36 %, main/GL 23 %, IopThread 18-20 % of a core. Clock 1,416 MHz throughout; cooling states 1-2 only cap at
1.6/1.5 GHz, so **the kernel never lowers our 1.416 GHz below 115 C**: the guard is the only thing that acts.

Two threads (game, stage A) are close to a full core; the rest are far from it. The game makes as many frames as it can (its own
cap is 30 a second in play: 2 vsyncs; 60 in menus), so at a fixed clock **making the code faster raises the frame rate, not the
idle time**: heat only falls when work per second falls (a lower clock, a frame cap, or idle threads that stop waking).

## 2. Measuring on the PC

`scripts/heat_threads.py` samples `/proc/PID/task/*/{schedstat,status}` over time windows (CPU over wall, run-queue wait,
voluntary switches = wakeups, involuntary). `scripts/heat_run.sh SECONDS NAME WINDOWS` runs New Game with no input after the menu
at `PS2PORT_SLOWDOWN=60`, no GIF log; `DISPLAY_ON=1` runs the device's display loop (`runner/display.cpp`) on SDL's offscreen
driver with llvmpipe, so the `[m5*]` and `[lag]` lines are logged. Windows: 40-60 s (title, menus), 100-140 s (New Game
cutscenes), 225-275/280 s (play, mode 5 from about 218-256 s).

Traps found on the way: (1) `PS2PORT_SLOWDOWN` busy-waits on the game thread, so the game thread's CPU share on the PC is not its
work (only its wakeups mean something); (2) other workers run processes named `runner` on the same machine: the sampler now takes
only a process that starts after it from this worktree's binary (`--exe`); a first set of "after" tables had sampled another
worker's process and was thrown away; (3) with `DISPLAY_ON=1` llvmpipe makes the display the slow side (frames about 100 ms), so
the main thread is mostly rendering there, unlike the device (23 %).

### Per thread, PC at device pace, play window (wakeups a second; CPU % of one PC core)

| Thread | before: CPU % | before: wakeups/s | after: CPU % | after: wakeups/s | what it is |
|---|---|---|---|---|---|
| GameThread (display off) | 89.8 (slowdown spin) | 559 | 88.3 (slowdown spin) | 459 | EE: guest code, runtime |
| GameThread (display off), cutscenes 100-140 s | 96.2 | 226 | 93.3 | **37** | |
| GameThread (display off), menus 40-60 s | 95.9 | 320 | 86.0 | 124 | |
| GsThread (stage A) | 35.1 | 1,752 | 39.6 | 1,741 | VIF1, VU1, arbiter: one wake per job/hand-off |
| GsDraw (stage B) | 42.2 | 1,113 | 45.5 | 1,118 | GS parsing, recording: one wake per batch |
| IopThread | 6.2 | 684 | 5.7 | 698 | IOP interpreter, SPU |
| main, device display loop (display on) | 80.1 (llvmpipe) | 413 | 83.3 (llvmpipe) | **266** | latch, GL, swap, pad |
| main loop's own waits (`[lag]`, display on) | | **171-312 (poll)** | | **12-43 (event)** | |

Before = sandbox baseline (`sandbox-base`), after = the three proposal patches plus `display.cpp`; same route, one run each,
machine shared with three other workers (run-queue wait 1-6 %). Stage A/B differ by about 3-4 points between runs at the same
code: noise. Time the EE spent blocked waiting for the IOP thread (`[clock] iop`): 7.05 s before, 7.15 s after in the display-off
runs; one earlier baseline run had 47.9 s, and in it the game thread woke **4,000 times a second** (the 50 us poll while it
waited, see site 2).

## 3. Spin, poll and wait sites (every one found), with cost and verdict

| # | Site | Thread | What it did | Measured (PC) / device estimate | Verdict |
|---|---|---|---|---|---|
| 1 | `runner/display.cpp` `ps2portDisplayFrame`, wait loop (was `sleep_for(500 us)`) | main | Looked for a recorded flip every 0.5 ms, up to 50 ms, pad read every 8th look | PC 171-312 looks/s with the display busy rendering; device estimate about 1,000/s (frames 45-60 ms, latch+swap about 20 ms, the rest polled at 0.5 ms); each costs a timer wake, a mutex (`ps2portDrawWantsLatch`) and two loads: about 1-2 % of one A53 core (estimate) | **Fixed** (own file + GS patch): sleeps on an event until a flip is recorded or stage B wants a latch; wakes only to read the pad every `PS2PORT_PAD_POLL_MS` (4). PC: 12-43 waits/s, thread wakeups 413 -> 266/s. Without the GS patch the old poll stays (`PS2PORT_DISPLAY_POLL_US`). |
| 2 | `ps2xIOP/src/emulator/iop_emulator.cpp:220` `ps2portEeWait`: `caught.wait_for(50 us)`, and `caught.notify_all()` after every 4,096-cycle IOP batch (line 1110) | GameThread (EE, the limiting thread) | Woke every 50 us and after every IOP batch while waiting for the IOP to catch up | PC: GameThread wakeups play 559 -> 459/s, cutscenes 226 -> 37/s, menus 320 -> 124/s; 4,000/s in the baseline run where the EE waited 47.9 s on the IOP (20 % of the run: 20,000 polls a second while waiting). Device: unknown how long the EE waits on the IOP (`PS2PORT_STATS=1`, `[clock] iop`, experiment 8) | **Proposal patch** `heat-for-iop.patch` (IOP worker): the EE says which `done` it needs, the IOP thread wakes it only when reached or when it stops for a hold; an IOP call queued for the EE wakes it at once; 2 ms safety time-out. |
| 3 | `iop_emulator.cpp:1091` IOP thread `work.wait_for(1 ms)` loop | IopThread | Looks every 1 ms while an IOP thread awaits an EE reply (or with the IOP-realtime experiment on) | Bounded: a reply wait is capped at 8 ms of IOP time / 250 ms; IopThread 684-698 wakeups/s in play in all (PC) | Harmless; left. |
| 4 | `iop_emulator.cpp:1101` `while (replyWaiters > 0) yield();` | IopThread | Yields while the EE waits to take the IOP lock with a reply | Only as long as one `lock()` of a lock the IOP thread does not hold there: microseconds | Harmless; left. |
| 5 | `ps2xRuntime/src/lib/ps2_memory.cpp:340` `ps2portFrameWait`: `shownCv.wait_for(2 ms)` | GameThread | EE at its flip waits for the display, looking every 2 ms (VIF1 interrupts served meanwhile) | Device: shop 0-3 waits per 5 s (none to speak of); fog menu 52 ms a frame: about 26 looks a frame, about 400/s, only there | Harmless in play; left (the 2 ms look serves VIF1 interrupts). |
| 6 | Guest `sceGsSyncPath` (`0x2f68e0`, called at frame end from `0x2e2268`): loops on D1_CHCR, D2_CHCR, VIF1_STAT, VPU_STAT, GIF_STAT | GameThread | A busy-wait in guest code for the GS side to go idle | Each pass reads VIF1_STAT, which waits on a condition variable until stage A is idle (`ps2portGsAcquire`), and a D1_CHCR busy read services the worker first: at most one or two passes a frame. PC: `D1_CHCR reads busy 0`/s in play. Device: read `[m5t] ... D1_CHCR reads busy` (experiment 8) | Harmless; no change. |
| 7 | Guest vsync wait `0x2cd6a0`: PollSema until it fails (`0x2cd6c8`), then WaitSema | GameThread | Drains a semaphore, then blocks | The block is the EE scheduler's condition variable (`EeScheduler.cpp:2051`, `:2293`): no spin. In menus the EE idles there (game thread 28 % on the title, Phase 4) | Harmless; an idle guest loop that already sleeps. |
| 8 | `EeScheduler.cpp:2051`, `:2293`; GS stage A/B `workCv`, `idleCv`, `irqDoneCv`, `space`, `latched` | EE, GS threads | Condition-variable waits without time-outs (or 1 s safety) | GsThread/GsDraw wake once per job/batch (1,100-1,800/s on the PC), each doing real work | Harmless. |
| 9 | `runner/audio_sdl.cpp` SDL callback | SDL audio | 1,024 frames a callback: 47 calls a second | | Harmless. |
| 10 | `scripts/device_guard.sh` once a second: `$(hottest)` with `$(cat zone)` per zone | shell on the device | 1 + N processes (N thermal zones, about 5) every second | estimate 1 % of one core (fork+exec 1-3 ms each on the A53) | **Fixed**: zones read with `read`, result in a variable: no process per check. |
| 11 | `runner/device_play.sh` memory watcher: `pidof`, `cut`, `awk` every 2 s | shell | 3 processes every 2 s | estimate 0.3-0.5 % of a core | **Fixed**: builtins only (pid looked up once); the loop now also writes `freq.txt` (temperature, clocks, per-core). |
| 12 | `runner/device_panel.sh` `awk` over `/proc/PID/smaps` every 2 s | shell (test runs) | walks every mapping of an 88 MB process | estimate tens of ms each: 1-2 % of a core in every profiled test run | **Fixed**: every 30 s. |
| 13 | `EeScheduler.cpp` `PS2PORT_SLOWDOWN` | GameThread | busy-wait | PC only (test switch) | Not on the device. |

## 4. Work computed and thrown away

- **Flips drawn but never shown** (two flips recorded between two latches: both frames' commands replayed by the GPU, one seen):
  PC with the display on, 0-3 per 5 s at 8-12 flips/s (now counted in `[lag]`: `flips never shown`). Rare; the device's numbers
  come with the next play log.
- **Presents with no new flip** (`due()`: a guest vsync after 100 ms without a flip latches the live registers and swaps again):
  PC 6-28 per 5 s, because llvmpipe frames take about 100 ms; on the device play frames are 45-67 ms with none over 100 ms in the
  logs, so in play this should be near 0, but every frame of the Remembrall (700-900 ms) and the loads get extra presents of an
  unchanged or half-recorded frame. Counted now (`presents with no new flip`); left as it is (loads need it to show progress).
- **Double presents:** none: one latch per recorded flip (latches/s = flips/s within the counts above).
- **Sound mixed and dropped:** none seen: the mixer runs at 48,000 frames/s, `output ran dry 0` (PC).
- **The IOP's floor:** 18-20 % of a core even with the game hung (r1): the interpreted IOP modules and the SPU mixing at 48 kHz
  whether anything plays. Biggest single "idle" cost found. Not a spin: the IOP skips to its next event when all its threads
  sleep (`runCycles`). Cure is the IOP worker's (static translation of the IRXs, `docs/STUDY_BUILD_OPT.md` E: 18 -> about 5 %).
- **Menus and loading screens:** up to 60 flips a second (89 on the loading screen before frame sync). `PS2PORT_PRESENT_CAP=N`
  (new, off by default) holds a frame that comes sooner than 1/N s and keeps the display "busy", so the game waits at its next
  flip instead of drawing frames nobody sees. Default off: whether menu animations are per frame or per 25 Hz tick (half speed
  vs. choppier at 30) is the user's visual check; play is already below 30 so it does nothing there today.
  PC check with `PS2PORT_PRESENT_CAP=10` (display on, 40-90 s, menus and the first load): latches 4.4-10.1/s, never over the cap;
  game flips held to the latches in most 5-s windows (e.g. 10.0/10.0, cap held 1.2-2.7 s per 5 s), **but in some windows the game
  flipped past the cap** (14.0 flips/s, 7.2 latches/s, 34 flips never shown): there the frame-sync wait does not hold the EE (it is
  skipped while a VIF1 handler runs or the display counts as idle), so those frames are drawn and thrown away. One more reason to
  keep it off until it is tried on the device.

## 5. What changed (this worker's files)

- `runner/display.cpp`: event wait (weak hooks `ps2portFrameRecordedEvents`, `ps2portFrameWaitEvent`; the old poll when absent),
  `PS2PORT_PRESENT_CAP`, `PS2PORT_PAD_POLL_MS`, `PS2PORT_DISPLAY_POLL_US`; `[lag]` adds flips never shown, presents with no new
  flip, the main thread's waits a second and how (`event`/`poll`), cap hold time. Frame sync and its at-most-one-frame lag unchanged.
- `scripts/device_guard.sh`: restores clock and governor also when the guard itself gets TERM/INT/HUP (it did not before: a
  stopped guard left the clock pinned and the governor on performance); no process per temperature check. Tested under dash with
  a fake sysfs: normal end, thermal stop (exit 3), TERM to the guard (exit 143, command group gone, clock back), hot at start.
- `scripts/play.sh`: `CPU_KHZ=N` (checked against the device's clocks). `runner/device_play.sh`, `runner/device_panel.sh`:
  `GPU_MIN_HZ` and `CPU_GOV` taken from the VAR=VALUE list (**`sh scripts/play.sh 30 GPU_MIN_HZ=0` did nothing before**: it went to
  the runner); `device_play.sh` writes `play-*/freq.txt` (as `device_panel.sh`'s, plus GPU clock) and prints temperature per
  minute at the end; `THROTTLE_MC`/`THROTTLE_KHZ` soft step (off by default). Tested under dash with a fake sysfs.
- `runner/knulli/HarryPotterPS2.sh`: **pinned nothing before, so it would have run at the firmware's 1.512 GHz ceiling**, hotter
  than every test; now `CPU_KHZ` (default 1416000), `CPU_GOV`, `GPU_MIN_HZ`, all put back on exit; a `[heat]` line every 30 s.
- `scripts/heat_threads.py`, `scripts/heat_run.sh` (new). `docs/ENV_VARS.md`.

Proposal patches (in `D:\ps2port-data\workers\`, against `sandbox-base`; the sandbox is left with all applied):

| Patch | Owner | What | Measured effect (PC) |
|---|---|---|---|
| `heat-for-gs.patch` (`ps2_memory.cpp`, +31 lines) | GS worker | An event count + condition variable, bumped where a flip is recorded (both places) and where stage B asks for a latch; `ps2portFrameRecordedEvents()`, `ps2portFrameWaitEvent(seen, us)`. No struct layout change. | main loop 171-312 -> 12-43 waits/s; main thread wakeups 413 -> 266/s; New Game to play and frame sync unchanged |
| `heat-for-iop.patch` (`iop_emulator.cpp`, `ps2_iop_host.cpp`, +42/-6) | IOP worker | `ps2portEeWait` with a needed `done`, targeted notify, call-queued wake, 2 ms safety time-out | GameThread wakeups play 559 -> 459/s, cutscenes 226 -> 37/s; EE blocked on the IOP 7.05 -> 7.15 s (no change); sound and cutscenes unchanged |
| `heat-runtime.patch` | this worker (`main.cpp`) | empty: nothing in `main.cpp` needed changing | |
| (none) `heat-for-ee.patch` | EE worker | no EE change proposed: the EE-side sites (6, 7) are harmless | |

**Honest size of all this:** removing the polling saves wakeups, not much CPU: about 1,000 wakeups a second on the main thread and
a few hundred on the game thread on the device (estimate), roughly 1-3 % of one core, under 1 % of the SoC's power (estimate).
It does not by itself buy 30 minutes. The clock does (section 6).

## 6. Clocks

What the scripts set (read from the scripts):

| Script | CPU | GPU | Restore |
|---|---|---|---|
| `scripts/play.sh` -> `device_detach.sh` -> `device_guard.sh` | governor `performance`, ceiling `DEVICE_KHZ` (1416000; now `CPU_KHZ=`) | `device_play.sh`: min 648 MHz (`GPU_MIN_HZ`) | guard: clock and governor at the end and now on TERM/INT/HUP; GPU by `device_play.sh`'s EXIT trap |
| `runner/device_panel.sh` (tests) | as above | as above | as above |
| Ports launcher | was: nothing (firmware schedutil, ceiling 1512 MHz); now ceiling 1416000 | min 648 MHz | EXIT trap (also after TERM/INT/HUP) |

Facts: one cpufreq policy for all four cores (480, 792, 1008, 1200, 1320, 1416, 1512, 1600, 1704 MHz); GPU 240-648 MHz
(`simple_ondemand` stays at 240 under this load); trips 60/70 C passive, 115 C critical; cooling states only cap from 1704 MHz
down (state 2 = 1512), so at 1.416 GHz the kernel does nothing until state 3.

**Which threads could run slower:** stage B (30-55 %), GL (23 %), IOP (18 %) are far from a full core, but with one policy they
cannot get their own clock. The game thread and stage A are both near 100 %, so frame time scales with 1/f:

| Ceiling | Frame time (estimate, from 55-67 ms in Ollivanders at 1.416) | CPU power vs 1.416 (estimate) | Plateau (estimate, from 83-87 C warm) |
|---|---|---|---|
| 1.416 GHz | 55-67 ms | 1.00 | 83-87 C (measured, r2) |
| 1.32 GHz | x1.07: 59-72 ms | about 0.88 | about 78 C |
| 1.2 GHz | x1.18: 65-79 ms | about 0.76 | about 73 C |
| 1.008 GHz | x1.40: 77-94 ms | about 0.55 | about 65 C |

Assumptions (not measured): dynamic power ~ f x V^2 with V about 1.10 V at 1.416, 1.05 at 1.32, 1.00 at 1.2, 0.94 at 1.008 GHz
(typical for this SoC family; read the real ones, experiment 1), dynamic 70 % / leakage 30 % of CPU power at 80 C, temperature
rise above ambient proportional to SoC power, GPU and memory unchanged. Frame times at 1.2 GHz are over the stop table's 55 ms:
the user's call, not this worker's.

**GPU clock:** 648 MHz is needed in the fog menu (21.6 ms at 648 vs 28 ms at 240, gsplay, measured). In play the GPU needs about
9-11 ms a frame at 648 MHz and about 22 ms at 240 MHz (N02_1 dump, 89.4 vs 44.3 ms per 4 frames, measured): under half of a
45-67 ms frame either way, so play probably does not need 648 (estimate; frame sync waits only if the display is busy at the
flip). Pinning the minimum keeps the GPU at its top voltage even idle. Experiments 4-5.

**Soft step:** `THROTTLE_MC=80000` lowers the ceiling to 1.2 GHz above 80 C and back below 74 C: the session goes on slower
instead of stopping at 85 C (the stop stays). Off by default (it changes frame times mid-session).

## 7. Device experiment plan (for the user; not run by this worker)

Each run: let the device cool below 50 C first (`cat /sys/class/thermal/thermal_zone0/temp` over SSH), go to the same place
(Continue into Ollivanders), stand still. Read afterwards: the per-minute temperature rows printed at the end (from
`play-*/freq.txt`), `THERMAL STOP` or not and when, and the `[lag]` lines (game flips/s = frames/s, `main thread waits/s ... (event)`
once the GS patch is in). Per-core load: `python3 -I scripts/core_load.py FREQ_TXT 240 1800` after fetching the folder.

1. Real CPU voltages, once (SSH on the device): `grep . /sys/class/regulator/regulator.*/microvolts /sys/class/regulator/regulator.*/name`
2. Baseline, 1.416 GHz, 30 minutes: `sh scripts/play.sh 30`
3. 1.32 GHz: `sh scripts/play.sh 30 CPU_KHZ=1320000`
4. 1.2 GHz: `sh scripts/play.sh 30 CPU_KHZ=1200000`
5. GPU governor left alone in play: `sh scripts/play.sh 30 GPU_MIN_HZ=0`
6. GPU at its middle clock: `sh scripts/play.sh 30 GPU_MIN_HZ=420000000`
7. schedutil under the 1.416 ceiling (menus and loads may run slower clocks): `sh scripts/play.sh 30 CPU_GOV=schedutil`
8. Where the waits are (one 10-minute run): `sh scripts/play.sh 10 PS2PORT_M5_STATS=1 PS2PORT_STATS=1` then read `[m5]` cpu per
   thread, `[m5t] ... D1_CHCR reads busy`, `[clock] ... iop` (ms the EE waited for the IOP thread)
9. Soft step instead of a stop: `sh scripts/play.sh 30 THROTTLE_MC=80000`
10. Menus at 30 presents a second (visual check of menu animation speed, title and pause menu): `sh scripts/play.sh 10 PS2PORT_PRESENT_CAP=30`

Compare 2-4 by the minute at which 80 C is reached and the plateau; 5-6 by the plateau at the same frame rate; 7 by temperature
in the menus and the frame rate in play (schedutil must not lower play's frame rate). Experiments 2-4 decide the clock.

## Validation (PC, x86, sandbox with all patches applied)

- Build links; New Game reaches play (mode 5) at 219-248 s in every run at `PS2PORT_SLOWDOWN=60` (display off and on).
- `sh runner/try_sound.sh 240 heat-check PS2PORT_SLOWDOWN=60`: 47,709-48,190 of 48,000 frames/s in every 10 s from 91 s to 232 s
  (cutscenes and play; mode 5 at 219 s); the boot and menus before are below as in earlier runs.
- `[lag]` with the display on: frame sync on, one latch per flip, flips never shown 0-3 per 5 s, no change in the waits.
- Shell scripts: `sh -n` and `dash -n` pass; guard and `device_play.sh` tested under dash with a fake sysfs (section 5).

## 8. Risks

- Event wait: a missed notify would delay a latch by at most the pad-poll time-out (4 ms), never block; checked on the PC (New
  Game to play, `[lag]` frame sync unchanged, at most one frame between game and screen). Untested on the device.
- IOP patch: a missed wake falls back to the 2 ms time-out (the EE waits up to 2 ms longer once); sound and cutscenes checked on
  the PC only.
- `PS2PORT_PRESENT_CAP` holds the game at its flip (frame-sync time-out 250 ms): caps under 4/s would hit time-outs; in some stretches the game flips past it anyway (section 4). Off by default.
- `CPU_GOV=schedutil` may lower play's frame rate if the governor misreads the pipeline's waits.
- The launcher now pins 1.416 GHz: lower than the firmware's 1.512 GHz ceiling it would have used: cooler, not slower than any test.
- All device estimates (wakeup cost, power ratios, plateaus) are estimates until the experiments run.
