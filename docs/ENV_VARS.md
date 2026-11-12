# Switches (environment variables)

The runner, the GS player and the device scripts are configured by environment variables. Defaults are what a play session
uses. Pass them to a play session as `sh scripts/play.sh MINUTES VAR=VALUE ...`, to a PC run as `sh runner/try.sh SECONDS PAD
VAR=VALUE ...`. "Private" means the code is in the PS2Recomp checkout under `private/` (patch files in
`private/third_party/trial/`).

## Playing

| Variable | Default | What it does | Where |
|---|---|---|---|
| `PS2PORT_DISPLAY` | off (on in device scripts) | GLES display on the device's panel, SDL pad and audio. Without it: the software GS (PC). | `runner/display.cpp` |
| `PS2PORT_HEADLESS` | off (on in scripts) | No window of PS2Recomp's own (raylib stand-in). | private runtime |
| `PS2PORT_FRAME_SYNC` | 1 | The game stays at most one frame ahead of the screen (input lag fix). 0: the old byte-bounded queue. | `display.cpp`, private `ps2_memory.cpp` |
| `PS2PORT_PAD_STICKY` | 1 | A button tapped between two game reads still reaches the game. 0: off. | `runner/pad_sdl.cpp` |
| `PS2PORT_PAD_SWAP_FACE` | auto | Swap A/B and X/Y (default swapped for the ANBERNIC pad, whose mapping follows the labels). | `pad_sdl.cpp` |
| `PS2PORT_PAD_DB` | PortMaster's database | SDL controller mappings to load first. | `pad_sdl.cpp` |
| `PS2PORT_AUDIO` | 1 | 0: no sound output. | `runner/audio_sdl.cpp` |
| `PS2PORT_NO_SOUND` | off | 1: the SPU mixes nothing at all. | private IOP |
| `PS2PORT_SHOT_DIR` | `/userdata/screenshots` (device), `private/screenshots` (PC) | Where Menu + L1 (device), Enter or SIGUSR1 (PC) save a PNG. | `display.cpp` |
| `PS2PORT_SHOT_AT` | none | Screenshots at these seconds after start: `20,45`. | `display.cpp` |
| `PS2PORT_REALTIME` | 1 | The EE's clock (and with it the IOP's: sound driver, SPU mixer, vblanks) follows the host's clock: when the EE thread runs slower than a PS2 or waits on the host, time jumps ahead (at most 100 ms at once); held while the game loads (mode 0). Sound at the right speed and pitch. 0: off (sound at the EE's pace, about half speed on the device). `=2`: the IOP keeps to the EE's own cycles (experiment). | private `EeScheduler.cpp` |
| `PS2PORT_SPU_STARVE` | 1 | A voice that played a DMA-refilled ring (a streamed dialogue or music) and goes round it three times with no refill stops as at an end block, so a starved line ends instead of looping and the cutscene goes on. 0: off. | private IOP `iop_memory.cpp` |
| `PS2PORT_SPU_REKEY` | off | 1: keep a second key-on of a voice still playing a streamed ring from the same start (dropped by default). | private IOP |
| `PS2PORT_CD_LATENCY_US` | 2000 | When a CD read's completion callback runs on the IOP, in microseconds after the request (was 3.5 us). | private IOP |
| `PS2PORT_MOVIE_DIRECT` | 1 | When EA's disc stream gives the movie decoder nothing, the decoder reads the movie file from the disc folder itself, 64 KB at a time, and hands the movie's sound packets to the game's audio callback. 0: off (the movie then waits for the stream, which stalls here). | private `MPEG.cpp` |
| `GSGLES_PAL_EXPAND` | 1 | Palette textures drawn with the linear filter are expanded to RGBA once (one texture fetch instead of eight). 0: old shader path. | `gsgles/gs_gles_backend.cpp` |
| `GPU_MIN_HZ` | 648000000 | The GPU's minimum clock for a device session (its governor otherwise stays at 240 MHz). 0: leave it. Given to `scripts/play.sh` as `GPU_MIN_HZ=0`, the script takes it (before 2026-11-12 it went to the runner and did nothing). | `runner/device_play.sh`, `device_panel.sh`, the Ports launcher |
| `CPU_KHZ` | 1416000 | `scripts/play.sh` only: the CPU clock's ceiling for the session (one policy for all four cores): 480000, 792000, 1008000, 1200000, 1320000, 1416000 or 1512000. The Ports launcher takes the same name (it left the firmware's 1512000 before). | `scripts/play.sh` (as `DEVICE_KHZ` to the guard), launcher |
| `CPU_GOV` | (the guard's `performance`) | The CPU governor under that ceiling, e.g. `schedutil` (the clock may drop when the cores idle: menus, loads). The guard puts the old one back. | `device_play.sh`, `device_panel.sh`, launcher |
| `THROTTLE_MC`, `THROTTLE_KHZ` | off, 1200000 | A soft step before the 85 C stop: above `THROTTLE_MC` milli-C the CPU ceiling drops to `THROTTLE_KHZ`, and comes back 6 C lower (`[heat]` lines in `play-*/freq.log`). The 85 C stop is unchanged. | `device_play.sh` |
| `PS2PORT_PRESENT_CAP` | 0 (off) | At most N presents a second. A frame that comes sooner waits, the display counts as busy, so the game waits at its next flip (frame sync) instead of drawing frames that are never shown: less work in menus and loading screens that flip 60 times a second. Play already runs below 30. | `runner/display.cpp` |
| `PS2PORT_PAD_POLL_MS` | 4 | How often the main thread reads the pad while it waits for a frame. | `display.cpp` |
| `PS2PORT_DISPLAY_POLL_US` | 500 | Only with a runtime that lacks the frame event (`ps2portFrameWaitEvent`, heat patch for `ps2_memory.cpp`): how often the main thread looks for a new frame. With the event it sleeps until one is recorded. | `display.cpp` |

## Logs and statistics

| Variable | What it writes | Where |
|---|---|---|
| `PS2PORT_M5_STATS=1` | Every 5 s on the device: frame times, latch and swap, CPU per thread, GPU work, memory, sound mix rate (`[m5]`, `[m5f]`, `[m5t]`, `[m5m]` with logic ticks per second, `[m5w]`, `[m5h]`). On in `device_panel.sh`. | `display.cpp` |
| `PS2PORT_LAG_LOG=1` | Every 5 s: game flips and latches per second, flip to replayed (ms), how long the game waited for the display, and at how many latches the live display registers had already moved on to the next flip (`[lag]`). Since 2026-11-12 also work that buys nothing: flips recorded but never shown on their own, presents with no new flip, the main thread's waits a second (`event` or `poll`), and how long `PS2PORT_PRESENT_CAP` held frames back. On in play sessions. | `display.cpp` |
| `PS2PORT_FRAME_LOG=FILE` | One line per game frame: ms since start, frame ms, vblank count, vblanks of the last frame. | `display.cpp` |
| `PS2PORT_FRAME_DIR=DIR`, `PS2PORT_FRAME_EVERY=N` | A picture of the shown frame every N presents (device: PPM; PC: PPM named `.png`). | `display.cpp`, private runtime |
| `PS2PORT_PAD_LOG=1` | Every change of the pad's buttons and sticks (`[pad]`). On in play sessions. | `pad_sdl.cpp` |
| `PS2PORT_MC_LOG=1` | Every memory card command and its result (`[mc]`). On in play sessions. | private `MemoryCard.cpp` |
| `PS2PORT_WATCH=0xA,0xB`, `PS2PORT_WATCH_EVERY=N` | Guest words at the game's frame end when they change (every Nth frame). `0x38c194` is the game mode, `0x38c210` the logic tick. | `display.cpp` |
| `PS2PORT_GSDUMP=FILE:START:VSYNCS` | A GS dump in PCSX2's format (for `gsplay`), from START seconds, for VSYNCS frames. | private `gs_frontend.cpp` |
| `PS2PORT_VU1_LOG=1` | VU1 program entries per frame. | `display.cpp` |
| `PS2PORT_SPU_PCM=FILE` | The sound mix as raw 16-bit stereo 48 kHz PCM (also how to measure the mix rate on the PC). | private IOP |
| `PS2PORT_MPEG_LOG=1` | libmpeg events: the first 64 of each kind, then every 64th (callbacks registered, no-data callback calls, DemuxPssRing, GetPicture, decoded pictures with their mean colour, the end flag, reading from the disc folder). | private `MPEG.cpp` |
| `PS2PORT_SPU_DEBUG=1`, `PS2PORT_SPU_TRACE=1` | What goes into each SPU core's mix; SPU2 and DMA register use. | private IOP |
| `PS2PORT_NATIVE_STATS=1` | How calls into eegen's and the vector units' native code went, at exit. | `runner/native_hooks.cpp` |
| `PS2PORT_THROW_LOG=1` | Who throws C++ exceptions (the runtime and the IOP swallow many). | `display.cpp` |
| `PS2PORT_STACKS=S1,S2` (`PS2PORT_STACKS_THREAD=Game`) | The host call stack of each thread whose name starts with the given text, at those seconds (`[stack]`, names: `scripts/stack_symbols.py LOG RUNNER`). Where a blocked thread waits, without a debugger. PC. | `display.cpp` |
| `PS2PORT_SAMPLE=FILE` (`_FROM`, `_TO`) | Every thread's program counter each CPU tick, for `scripts/sample_threads.py`. | `display.cpp` |
| `PS2PORT_STATS=1`, `PS2PORT_THREADS=1`, `PS2PORT_TIMESHARE=FILE`, `PS2PORT_CALLCOUNT=FILE` | PC runs: present and DMA counters; EE threads and semaphores; host time per guest function; every function entry. | private runtime |
| `PS2PORT_GIF_LOG=FILE`, `PS2PORT_GIF_RAW=FILE:FROM:COUNT` | GIF packets as a log; the packets themselves. `scripts/ps2recomp_trial.sh` sets the log unless `PS2PORT_GIF_LOG=` (empty): writing it slows the GS side, and so the EE and the IOP, so turn it off for timing and sound. | private `ps2_memory.cpp` |
| (always) | `[stream-open] PATH`: every file the game opens through EA's stream library (cutscenes `CUT/*.HOG`, movies, music), so a log shows which cutscene was playing. | private `RPC.cpp` |
| `PS2PORT_STATS=1` (PC) | Also `[clock]`: the EE's vblank count, the time `PS2PORT_REALTIME` added, and how long the EE side waited on the host, by place (scheduler, GS queue, display, frame sync, GS worker, IOP). | private runtime |
| `PS2PORT_PEEK=0xA,0xB` | PC: guest words with each stats line (`PS2PORT_STATS=1`), also when game frames have stopped. | private runtime |
| `PS2PORT_IOP_TRACE`, `PS2PORT_IOP_ALLOCS` | IOP RPC calls (with every MultiStream command of a packet, `[ee-rpc] cmds`), IOP modules as they load (`[iop] module`); IOP memory allocations. | private runtime and IOP |
| `PS2PORT_IOP_PEEK=0xA,0xB` | PC: IOP words with each stats line (`PS2PORT_STATS=1`). | private runtime |
| `PS2PORT_SPU_VOICES=1`, `PS2PORT_SPU_V01=1` | Playing voices (address, loop, flags, pitch) every 5 s of IOP time; core 0 voices 0 and 1 (the dialogue pair): key on/off, end blocks, ENVX changes. | private IOP |
| `PS2PORT_IOP_WATCH16=ADDR` | IOP halfword stores to a physical address, with pc and ra (e.g. `0x1F9001A0`, key-on). | private IOP |
| `PS2PORT_IOP_RPC_LATENCY=1` | IOP calls to the EE whose answer took over 20 ms of IOP time, and the stages of slow ones. | private IOP and runtime |
| `PS2PORT_IOP_WATCH=0xA[,0xB...]` | Every IOP byte store to up to four addresses, with the IOP pc and ra (`[iop-watch]`). | private IOP |
| `PS2PORT_IOP_SEMA=ID` | Every SignalSema of one IOP semaphore, with the caller. | private IOP |
| `PS2PORT_SNAP_PC=0xADDR`, `PS2PORT_SNAP_NTH=N`, `PS2PORT_SNAP_FILE=FILE` | A snapshot of RAM and context the Nth time the EE reaches ADDR (replay harness input). | private runtime |

## Test runs (scripted, nobody at the pad)

| Variable | What it does | Where |
|---|---|---|
| `PS2PORT_PAD="CROSS@5+0.3,START@40+3"` | Timed presses: BUTTON@start+hold[/repeat], seconds since the first pad read. | private `ps2_pad.cpp` |
| `PS2PORT_PAD_SEQ=VALUE:BUTTON[:SETTLE[:HOLD]],...` | Presses driven by the game's state: wait until the word `PS2PORT_PAD_SEQ_WORD` (default the mode, `0x38c194`; `*` = any) holds VALUE for SETTLE game frames, then hold BUTTON for HOLD frames (default 3). Buttons as in `PS2PORT_PAD`, plus `LSLEFT`, `LSRIGHT`, `LSUP`, `LSDOWN` (left stick fully one way). Continue on the device: `9:START:60,2:DOWN:60,2:CROSS:40,2:CROSS:300,2:UP:40,2:CROSS:20` after `PS2PORT_PAD="CROSS@5+0.3"`. | `display.cpp` |
| `PS2PORT_RUN_SECONDS=N` | End the run after N seconds. | private runtime |
| `PS2PORT_PRESENT_EVERY=N` | PC: present only every Nth vsync (the software present takes a quarter of the game's time in 3D scenes). | private runtime |
| `PS2PORT_GS_DRAW_WINDOW=ON/PERIOD` | PC: rasterize only ON seconds of each PERIOD (`0/1`: never; for logic tests). `runner/try.sh` sets it from `DRAW`. | private `gs_cpu_backend.cpp` |
| `PS2PORT_SLOWDOWN=P` | PC: the EE thread runs at about P % of a PS2's speed, to imitate the device. It busy-waits to do so: the game thread's CPU share on the PC is then not its work. | private `EeScheduler.cpp` |
| `DISPLAY_ON=1`, `SLOWDOWN`, `DRAW`, `PRESENT_EVERY` | `scripts/heat_run.sh`: per-thread CPU and wakeups of a PC run (`scripts/heat_threads.py`); `DISPLAY_ON=1` runs the device's display loop on SDL's offscreen driver and llvmpipe. | `scripts/heat_run.sh` |

## Experiments (off by default; see `docs/PHASE_8.md`)

| Variable | What it does | State |
|---|---|---|
| `PS2PORT_IOP_REALTIME=1` | The IOP (sound driver, SPU) follows the host's clock in play modes (2-5) while the EE keeps its own. Sound at 45-47 k of 48 k samples a second on the PC at half speed (10.7 k without). | The next load after the menus crawls (the stream library needs the IOP in phase with the EE). Not for play. |
| `PS2PORT_IOP_REALTIME=2` | The IOP's clock is never held for the EE (`=1` holds it during loads). | Tried for the movie stream stall: no change. |
| `PS2PORT_IOP_THREAD=0`, `PS2PORT_GS_THREAD=0`, `PS2PORT_GS_DRAW_THREAD=0` | Run the IOP, the GS side (stage A) or GS drawing (stage B) on the game's thread, as PS2Recomp did. | For bisecting. |
| `PS2PORT_NO_EG=1`, `PS2PORT_NO_VU0=1`, `PS2PORT_NO_VU1=1`, `PS2PORT_EG_FROM/TO=HEX`, `PS2PORT_NO_EG_RESUME=1` | Turn eegen's code or the translated vector units off (or limit eegen to a range) and use PS2Recomp's code or interpreter instead. | For bisecting a difference. |

## Code generation and build

| Variable | What it does | Where |
|---|---|---|
| `PS2PORT_HW_SEMANTICS=sqrt,cvt,branch,lqsq` | Which of the R5900's own semantics eegen uses where PS2Recomp's differ (default all four). | `runner/gen.sh` |
| `PS2PORT_PS2RECOMP_SEMANTICS=1` | PS2Recomp's semantics, as the replay harness expects. | `runner/gen.sh` |
| `PS2PORT_MOVIE_STUB` | 0 (default since 2026-11-11): movies play. 1: the five movie functions are stubbed (no movies, black menu background), as before. | `runner/gen.sh` |
| `PS2PORT_RUNNER=PATH` | PC: which runner `scripts/ps2recomp_trial.sh boot` starts (`runner/try.sh` sets it). | `scripts/ps2recomp_trial.sh` |

## Device scripts

| Variable | What it does | Where |
|---|---|---|
| `BENCH_HOST`, `BENCH_DIR`, `BENCH_SSH_OPTS` | The device's SSH address, work folder and options (from `.env`). | `scripts/env.sh` |
| `DEVICE_KHZ`, `DEVICE_TIMEOUT` | CPU clock pinned for a run (default 1416000), time limit in seconds. | `scripts/device_detach.sh` |
| `GUARD_LIMIT_MC` | Temperature stop in milli-degrees (default 85000). | `scripts/device_guard.sh` |
| `RSS_LIMIT_KB` | Stop a test run above this much anonymous memory (default 420000). | `runner/device_panel.sh` |
| `GSPLAY_FINISH`, `GSPLAY_NOSWAP`, `GSPLAY_ENV`, `OUT_TAG` | gsplay timing runs: glFinish per vsync, no swaps; extra variables and a captured frame in `gsgles/device_gpuclock.sh`. | `gsgles/` |
