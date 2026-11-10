# Phase 8: the whole game

User, 2026-11-08: "continue porting resources, scenes, gameplay and game".

**Resources need no porting:** the recompiled game reads its models, textures, levels, cutscenes and text from the original
disc files (`/userdata/ps2bench/game/` on the device) exactly as on a PS2. What remains is making every part of the game run
right on the device, and the parts the runtime only stubs.

## Plan (in order)

| # | Item | Why | State |
|---|---|---|---|
| 1 | **Saves** (memory card) | Sessions end at 85 C after about 13 minutes; without saves no one gets past the first chapter. Also the way to test later scenes. | Loading side checked; saving not yet (below) |
| 2 | **Later scenes** | Checked only up to Ollivanders (about 4 minutes of a long game). | Needs saves or the user's play sessions |
| 3 | **Game speed** | About 73 % in play: vsync follows guest cycles (Phase 7). Try vsync on real time. | Not started |
| 4 | **Sound** | `docs/PLAN_AUDIO.md`: SPU2 mixing on the IOP thread. | Plan only |
| 5 | **Heat** | 85 C after about 13 minutes at 1.416 GHz; the user keeps the 85 C stop. | Open |

## 1. Saves

- **This game's memory card calls are native stubs** (`sceMcInit` ... at `0x2F9238` onward, `trial/analyzer.toml`): the runtime's
  `Kernel/Stubs/MemoryCard.cpp`, which keeps the card as a host folder, `mc0/` next to the ELF (on the device
  `/userdata/ps2bench/game/mc0/`), one folder per save. The IOP's MCSERV stand-in (`ps2xIOP/src/modules/mcserv.cpp`, whose GetDir
  returns no entries) is not on this game's path; a fix tried there was taken back out, unused.
- The game's save folder is `BASLUS-20826-HPATPS`, three slots ("Save Game 1-3"), an `icon.sys` (strings in the ELF).
- **Measured (`panel-135947`):** with a dummy `mc0/BASLUS-20826-HPATPS/test.bin`, main menu, Continue: the game says the save data
  in memory card slot 1 "appears to be corrupt". So it finds and reads the folder: the listing and reading paths work. Dummy removed.
- **Not checked: writing a save.** Where the game saves is not known yet; Start in the Ollivanders gameplay (`panel-141136`, 240 s)
  showed no pause menu within 10 s. At boot the game asks the card for its info three times (0) and changes to its folder (-4,
  not there yet). `PS2PORT_MC_LOG=1` prints every card command and its result (runtime patch).
- **Where the game saves (2026-11-09, static reading of the Ghidra decompile, not run yet):** the card writes (sceMcOpen
  modes 0x202/0x203 in `0x2da118`/`0x2daaf0`) are driven by the card screen state machine `0x264a20`, which has two front
  ends: the main menu's (`0x1b7278`) and an in-game one (`0x286fc8`). In-game, `0x251610` switches on a mode word at
  `0x38c194`; **mode 6 is the save screen** (`0x250e80` -> `0x286f60`), entered through `0x286f18` from two places:
  1. **the pause menu** (mode 3, `0x26c7f0`): its item index 1 opens the save screen (index 0 goes to mode 10, index 2
     toggles an option). Mode 3 is opened by `0x250998` when the pause button is pressed and `0x1acf58()` is 0; in control
     set 0 a flag (`*0x395728 & 1`) changes the button table, which may be why Start did nothing in Ollivanders;
  2. **a save-point object in the world** (`0x160980`): within 200 units (or facing it) the game shows context icon 0x1a,
     and the action button (button index at `0x39569c`) opens the save screen.
  Button indices are the game's own enumeration (`0x3956xx`, set in `0x2631b8`'s neighbour around `0x263xxx`), not pad bits.
- The user's PCSX2 cards (`D:\ps2port-data\memcards`): `Mcd001.ps2` formatted and empty, `Mcd002.ps2` not formatted. No saves to
  borrow. `scripts/mc_extract.py` lists and copies saves from a PCSX2 card image into an `mc0` folder, for when there are some.

## Device

`panel-141136` (the fourth run in about 20 minutes) ended with **THERMAL STOP: 85.3 C**: device runs stopped there (stop rule c).

## 4. Sound (started 2026-11-08, the user's choice)

- **The mixer** (`ps2xIOP/src/emulator/core/iop_memory.cpp`, private IOP patch): on the IOP thread, in step with the IOP's clock
  (one 48 kHz frame every 768 IOP cycles; also after every IOP slice, `ps2portSpuTick`). ADPCM decode, pitch with linear
  interpolation, ADSR envelopes, voice volumes (fixed; a sweep keeps the last fixed level), VMIX, master volumes, core 0 into
  core 1 through core 1's input volume. The registers the game reads (NAX, ENDX, ENVX) behave exactly as in the model without
  sound; a keyed-off voice's release sounds apart from them. Out through a ring (0.68 s) to `runner/audio_sdl.cpp`: SDL2 audio
  (ALSA on the device, 48 kHz, 1,024-frame callbacks), resampled at the mixer's measured rate, kept near 80 ms waiting.
  `PS2PORT_NO_SOUND=1` (no mixing), `PS2PORT_AUDIO=0` (no output), `PS2PORT_SPU_PCM=FILE`, `PS2PORT_SPU_DEBUG=1`.
- **First run: almost silent** (peak 1-2 of 32,767). `[spu-mix]` showed core 0 playing a stereo stream (voices 0 and 1, pitch
  0x0EB3 = 44.1 kHz, envelope full, samples up to 15,257) and core 1's input volume (AVOL) at 0x7FFF, read as a sweep-format
  volume: -2/32,768. AVOL is a plain signed level. Fixed.
- **Measured after the fix (`panel-144549`, boot to the intro, 1.416 GHz):** RMS 2,000-4,200 per channel (-18 to -24 dBFS),
  peaks to 30,948, no clipping, left and right alike; silent only during boot and loading. The mixer made 29,000-33,000 frames
  a second of real time in the intro (the IOP's clock is that much behind real time, as the game is), so sound plays at about
  60-70 % speed and pitch until item 3 (game speed). The output ran dry for 81,628 frames by 60 s (boot and loading), none
  between 60 and 70 s.
- **Not done:** listening (the user, next session); Gaussian interpolation; reverb; noise and pitch modulation; ADMA streams;
  the cost on the IOP thread not measured yet (estimate in `PLAN_AUDIO.md`: a few % of a core).

## Testing on the PC (user, 2026-11-08: "first test on PC, later on the device")

- The PC runner (x86 in WSL) has no SDL2 there, so it uses the runtime's software renderer: `sh scripts/ps2recomp_trial.sh boot SECONDS PAD`
  with `PS2PORT_RUNNER=private/third_party/build-m2/x86/runner` (screenshots: PPM files named `.png` in `trial/boot-*/frames`).
- **Measured: the PC's software path is much slower than the device in 3D scenes**: frames about 70 ms in menus and loading,
  about 900 ms in the Hagrid street and the shop (the device: 45 ms). So the PC suits logic, loading, saves, sound data and
  movies, not timing or frame rates.
- Fixed on the way: the display throttle of Phase 7 (`ps2portDrawThrottle`) waited out its 1 s time-out every frame when no
  display takes the recorded frames (the PC's software path): it now acts only once a display has latched one.
- `PS2PORT_FRAME_LOG` now also logs the game's per-frame vblank count (0x328490, restarts each frame) and the vblanks the frame
  took (0x3287F0); `scripts/frame_stats.py` reports the game's speed from the latter (game time against real time).

## 3. Game speed (2026-11-08): first attempt, not good enough yet

- **Why 73 %:** an EE event (vblank, timer, alarm) fires once both its EE cycle and its host time have come
  (`EeScheduler::processDueDeadlines`). On the PC the host time comes last: real-time speed. On the device the cycles come last:
  the game's whole clock (vblanks, timers, the IOP's clock, sound) runs at the device's pace.
- **Tried: the EE's cycle count kept within 2 ms of the host's clock** (`PS2PORT_REALTIME=1`, `EeScheduler::accountCycles`; `=2`
  leaves the IOP's clock alone), with `PS2PORT_SLOWDOWN=70` making the PC's EE thread run at about 70 % of a PS2, as on the device.
  Measured on the PC (path to the shop, 260 s): game speed 100 % everywhere with it (4.4 vblanks a frame), 44-49 % without it in
  menus and boot (B), 57-59 % for the normal PC run (A; 4-17 % in 3D scenes, the software renderer).
- **But the first loading screen never ends with it** (both `=1` and `=2`): the loading bar barely moves in 4 minutes, where the
  normal run loads in seconds. The game's loader, not the IOP, is what breaks; not found yet (a time-out or retry measured in
  vblanks or timer ticks, or a loader thread that no longer gets the time the frame used to wait). Left off by default.
- **Next:** find what the loader waits for (its threads, semaphores and timer use during the first load, with and without the
  jump); or keep the clock and give the game real elapsed time only where it steps its world (the frame end, 0x2e2268, stores
  the elapsed vblanks that two functions read), which leaves loading, cutscenes and sound at the device's pace.

## 5. Movies

The game plays its `.PSS` movies (EA and WB logos, the main menu's background `FRONTEND.PSS`, and others: 157 MB) through
Sony's libmpeg (`sceMpegInit`, `sceMpegCreate`, `sceMpegDemuxPssRing` ...), which PS2Recomp stands in for natively
(`Kernel/Stubs/MPEG.cpp`) and decodes with FFmpeg when built with `PS2X_HAS_FFMPEG=1` (MPEG-2 video decoder and parser,
swscale to RGBA). Today the port stubs the movie functions (`runner/gen.sh` skip list, `scripts/ps2recomp_movie_stub.py`).
Plan: a minimal static FFmpeg 7.1 (`scripts/ffmpeg_build.sh`; the source downloaded from ffmpeg.org, its signature checked
against FFmpeg's release key), `PS2X_HAS_FFMPEG=1`, the skip list emptied, tested on the PC first. **Waiting for the user's OK
to build FFmpeg from that source** (the tool's permission check stopped it: code from an external download).

## Thermal facts (2026-11-09, read from sysfs on the device, measured)

- `cpu_thermal_zone`: power_allocator, trips **60 C passive, 70 C passive, 115 C critical** (hysteresis 0); cooling devices
  `thermal-cpufreq-0` and `thermal-devfreq-0` (GPU), 8 states each, both on trip 1 (70 C), weight 1024;
  sustainable_power 1000 mW, k_po 20, k_pu 40, k_i 0. GPU, VE, DDR zones: no trips. Battery zone separate (AXP2202).
- No fan. cpufreq: schedutil, 480-1704 MHz available (480, 792, 1008, 1200, 1320, 1416, 1512, 1600, 1704), scaling_max
  1512 MHz by default; GPU devfreq simple_ondemand 240-648 MHz.

### Movies, first PC test (2026-11-09): hangs at the first movie (measured), stopped by the item's call budget

- `runner/build.sh` had a missing space (`$ffmpeg-DPS2X_ENABLE_IOP_RPC_TRACE=1`): FFmpeg's include path was glued to the
  next flag, so `MPEG.cpp` failed to compile and the link failed. Fixed; the x86 runner links with FFmpeg.
- **PC boot, 80 s (`boot-20261109-120906`):** language screen at 5-8 s, then **black from 13.6 s to the end**; the EE stays at
  pc 0x2ddf6c, the DMA count stops changing (nothing drawn). With the stub the boot went on to the menu.
- **Why (reading the code; the exact blocking call is an inference):** the game opens its movies with
  `0x2dd8e0(..., 1, 2, 2, ...)`, mode 2 = libmpeg (modes 0/1 would drive the IPU directly; the runtime has no IPU). Per frame
  `0x2dc738` asks `0x2de0e0` (-> the game's own `sceMpegIsEnd` at `0x2fb220`, which reads `**(mp+0x40)`, not stubbed) and
  decodes through `0x2dde48` -> `sceMpegGetPicture` (stub). PS2Recomp's stub `GetPicture` **waits** (`waitExternal`) while it has
  no decoded frame and has not seen the CD stream's end; that end and the data counters come only from
  `notifyMpegCdStream*`, which PS2Recomp's native `sceCdSt*` stubs call. **This game's streaming runs through the emulated
  IOP, so those are never called**; and whether the game's data reaches `sceMpegDemuxPssRing` (directly or from a libmpeg
  callback the stub records but may not call) is not checked yet.
- Also found: the stub's `sceMpegReset` writes 0 to the fixed guest address `0x171904` (from another game): must go before
  movies are turned on.
- **Next (proposal):** a run with the stub's traces on (`AGRESSIVE_LOGS` for `MPEG.cpp` only) to see which libmpeg calls the
  game makes and whether DemuxPssRing gets data; then either feed EOF from the game's own stream state (the IOP stream
  driver's end, or `sceMpegIsEnd` semantics) and drop the `0x171904` write, or keep the stub. **Until then any new aarch64
  build must use `PS2PORT_MOVIE_STUB=1`** (the device's current `runner-aarch64` predates the change and still plays).

### Thermal run 1 (2026-11-09, `panel-121834`, 1.416 GHz, scripted path to the shop, measured)

Device binary: the device's own `runner-aarch64` of 2026-11-08 14:45 (md5 `17acdb0a`; local copy in
`build-m2/aarch64/device-1445/`). Before: CPU 52.5 C, clock 1.2 GHz idle, cooling 0, uptime 2,171 s. After: 63.2 C, uptime 3,388 s.

| s | CPU zone C | MHz min | cooling max | core busy % | |
|---|---|---|---|---|---|
| 0-90 | 52.5-64.5 | 1416 | 0 | 33-43 | boot, menu |
| 150-210 | 64.8-70.9 | 1416 | 0 | 68-78 | cutscenes, shop |
| 240-480 | 69.6-73.5 | 1416 | 0-1 | 54-80 | **gameplay, Harry idle in the shop: 47-60 ms frames** |
| 510-1200 | 57.8-62.7 | 1416 | 0 | 9-16 | **hung**: 0 game frames/s |

- **Hang at about 505 s, not heat:** `[guest-branch:missing-target] kind=IndirectCall op=JALR source=0x289a68 target=0x28a140`.
  0x28a140 is not in the function map (`functions.txt`, `register_functions.cpp`): another hole like `0x1af300` (fix recipe:
  `scripts/ps2recomp_gen_fixes.py` MID_ENTRIES or an own function, then regenerate and rebuild). It appears after about four
  minutes idle in the shop, so it would also stop real play. Frames had slowed from ~47 ms to ~60 ms at about 460 s just
  before (something started in the scene).
- **Heat:** in gameplay the CPU zone levelled at **69.6-73.5 C after 4-8 minutes**, cooling state at most 1, clock never
  lowered. The 2026-11-08 soak (same clock, same scene, older binary) was at 76.6-79.9 C at 240-360 s and reached 85 C at
  806 s: **about 7 C cooler today at the same point** (ambient or how the device lay; not controlled). Too short to judge
  60 minutes.
- Runs at 1.2 and 1.0 GHz not done (item budget; the hang has to be fixed first for sustained runs).

### Hole fix, guard bug, thermal run 2 (2026-11-09, measured)

- **Five merged functions fixed** (`scripts/ps2recomp_gen_fixes.py`, found with a lui/addiu scan for code addresses right
  after a `jr ra` that are not in the map): `0x28a140` (the game's float arithmetic handler, the shop hang) and
  `0x1ffdc0`, `0x1ffdc8`, `0x1ffdd0`, `0x1ffdd8` (one-line default callbacks). The script now adds a re-entry switch where
  PS2Recomp emitted none. aarch64 build md5 `5a9eec8d` (movie stub on). **On the device (`panel-133511`): no missing
  target in 1,120 s, the game went on past the scene change at ~650 s.**
- **Guard bug (safety):** the device's `sh` is dash; its `kill` rejects `--`, so `device_guard.sh`'s thermal stop and
  final cleanup did nothing (silenced by `2>/dev/null`), probably since the setsid change of 2026-11-08. `panel-133511`
  printed THERMAL STOP at 85.0 C and ran on to **86.7 C** until stopped by hand (no reboot). Fixed (no `--`), tested on the
  device with a CPU burner: stop while running after 7 s, exit 3, nothing left over.
- **Concurrent runs:** the user's play sessions ran while test runs were going: `play-122137` during thermal run 1 ended in
  the OOM killer (`Killed`, 18,717 `[gs-draw] packet failed: std::bad_alloc`): dropped GS packets explain the user's
  photo (missing and wrong textures, missing HUD, a stray line); `play-133836`, `play-134429` produced nothing. How they got
  past `device_detach.sh`'s BUSY check is not known yet.
- **Thermal run 2 (`panel-133511`, 1.416 GHz, started heat-soaked at 64-66 C):** 85 C at ~360 s, then a **plateau at
  83.3-86.7 C** to 1,120 s, frames 44.4-46 ms p50, cores 66-75 % busy. Cooling states stayed 1-2, which cap the clock at
  1.6/1.5 GHz (states count down from 1,704 MHz), so the kernel never lowered our 1.416 GHz: below the 115 C critical trip
  the guard is the only protection. Runs at 1.2 and 1.0 GHz still to do (device was in use).

### Pause menu: the shop opens in the hint box (2026-11-09, measured on the device and read from the code)

- **Tool:** `PS2PORT_WATCH=ITEM,...` (runner, `display.cpp`) prints `[watch]` lines at a game frame's end whenever a watched guest
  word changes: `0xADDR`, `*0xADDR[+0xOFF]` (through a pointer), or `btn` (bit i = button action i was pressed this frame).
- **How the game reads the pad** (`0x2b9cf8` -> `0x2ccc50`): the pad word goes into a table of 0x18-byte actions at
  `*(*0x38c130 + 0x10)`; +0x00 is the value, +0x10 a one-frame press edge. Actions: 0/1 left stick, 2/3 right stick, **4 Start**,
  5 Select, 6-9 L1/L2/R1/R2 pressure, 10 Up, 11 Down, 12 Left, 13 Right, 14 Triangle, 15 Circle, **16 Cross**, 17 Square, 18 L3,
  19 R3. The control set is `*0x3a4ff0` (2 in this game's default); `0x262758` assigns each game action an index: in set 2
  pause `*0x3956b0` = 4 (Start), dismiss `*0x395700` = 16 (Cross; 15 Circle if `0x262290()` == 1).
- **Measured (`panel-143607`, path to the shop, Start at 262 s):** the Start edge reaches the table (action 4 at 264.0 s), so the
  mapping is right. But from 221.7 s on, in the shop, **the mode word `0x38c194` is 5**, not 2 (play).
- **Mode 5 is the hint box** (`0x259c18(id)` opens hint `id` once, `0x250eb8` runs it): the world is frozen until, after 5 frames,
  the dismiss action (Cross) is pressed (`0x259670`: state `0x394e70` 1 -> 2 -> 3), then `0x250cf0` returns to mode 2. Pause
  (`0x250998`, mode 3) is only checked in mode 2 (`0x250a58`). **On the device the hint box is not drawn**, so the player sees
  the shop, nothing to dismiss, and Start does nothing. This is the "missing HUD part" of the user's test, a GS/GLES bug.
- **Pause works (`panel-152443`, measured):** the shop opens with hint 0x19 (invisible); Cross dismisses it and the Remembrall
  (mode 4) opens; Select leaves it and hint 0x1a opens (invisible); Cross dismisses it (mode 2 at 254 s); **Start opens the
  pause menu (mode 3 at 260.9 s)**; Down, then Cross opens **the save screen (mode 6 at 277.9 s)**, which asks the card for its
  info (196 `[mc] cmd 0x01`). Script: `CROSS@236+0.5,SELECT@243+1.2,CROSS@252+1.2,START@259+1.2,DOWN@268+1.2,CROSS@276+1.2`
  after the path to the shop. Writing a save not done yet (save screen's own steps unknown; its picture not checked).
- **Short taps are lost in slow frames:** in the Remembrall one frame in three takes ~680 ms; 0.3 s presses of Select and Start
  were never seen by the game. Scripted presses need >= 1.2 s; the fix for players is sticky presses in `runner/pad_sdl.cpp`
  (keep a press until one pad read has returned it). Not done.

### Ollivanders picture (2026-11-09): not a GLES bug (measured)

- **Tool:** `PS2PORT_GSDUMP=FILE:START:VSYNCS` (private runtime, `gs_frontend.cpp`; in `trial/ps2recomp-m1-runtime.patch`) writes
  a PCSX2-format GS dump of what the GS receives; gsplay replays it (checked on a PC dump of the loading screen).
- **Device dump of the hint frame** (`D:\ps2port-data\raw\own_hint.gs`, 30 vsyncs, 76 MB): **gsplay's software GS draws the
  same wrong picture as GLES** (white walls, pink floor, green shelves, diagonal line, no hint box). So the GS input is wrong;
  the GLES backend is not the cause.
- In the stream: **no 2D primitive at all** in hint mode (the hint draws through the game's VU1 2D API, `0x2ded18`/`0x2defe8`,
  MSCAL 0x03 for sprites), so the hint box is lost before the GS. The shop streams 82 textures a frame (8-bit textures uploaded
  as CT32, CLD 1 palettes); decoded right after upload they are all correct, and the upload/draw order is right. 7 % of the
  modulate pass's vertex colours are at 224-255 (near saturation): too-bright lighting is the main suspect for the white walls.
- **Next:** the PCSX2 reference of this scene (asked of the user: savestate slot 4, Ctrl+Shift+F8 and Shift+F8 dumps, F8
  screenshot, `file_dumps.py O01`); compare its stream (2D prims, vertex colours) with ours. `PS2PORT_VU1_LOG=1` (runner,
  built in `build-m2/aarch64/vu1log/`, not yet run) shows VU1 entries and kicks per second: does MSCAL 0x03 run in hint mode?
  Interpreters (`PS2PORT_NO_VU0=1 PS2PORT_NO_VU1=1`) are too slow to reach the shop in 330 s.

### Ollivanders picture fixed: the VIF1 DMA chain was cut at 4,096 tags (2026-11-09, measured on the device)

- **Reference:** `D:\ps2port-data\raw\N02_1.gs` (PCSX2, NTSC) is Ollivanders; gsplay's software GS draws it right (wood floor,
  red carpet, HUD, "TALK TO"). Its frame has passes ours lacked: render-to-texture shadows (2 targets), the lightmap multiplied
  by the base texture one colour channel at a time (3 passes, ALPHA 0x89 = Cd*As, FBMSK per channel), a darkening shadow pass
  from a CT16 target, additive light beams, alpha-blended draws, and the HUD.
- **Measured:** with `PS2PORT_VU1_LOG=1`, VU1's 2D programs (entries 3 and 0xf) never ran in play or hint mode, while the game's 2D
  API did build sprite batches every frame (`PS2PORT_WATCH` on its write pointer `0x3aa440`, `PS2PORT_WATCH_EVERY=10`).
- **Cause:** the runtime's DMA chain walk (`ps2_memory.cpp`, mode 1) stopped after `kMaxChainTags = 4096` tags and treated the
  transfer as done. The shop's VIF1 chain is longer, so everything the game links late (HUD, shadows, extra passes) was dropped.
  Now `1 << 20`, with a one-time `[dma] ... chain cut` warning (never printed in the test). In the runtime patch.
- **Result (`panel-201734`, build `a2fdbcaf`, now `build-m2/aarch64/runner-aarch64`; old one kept as `runner-aarch64.5a9eec8d`):**
  hint boxes with text, lightning bolt, button ring, Remembrall icon, wood floor, red carpet, light beams, Ollivander, and the
  pause menu (Continue, Save Game, ...), as in the user's PCSX2 screenshots. VU1 entries 3, 0xc, 0xf run; room-program kicks
  48k -> 60k a second.
- **Cost (stop rule):** shop frames **p50 66-67 ms, p90 78-80 ms** (hint and play), were 44.5 / 53.5 ms. Game thread 90 -> 65-70 %
  (waits now), GsDraw 36 -> 57 %, main (GL) 24 -> 36 %; GL runs per 5 s 5,490 -> 11,319, GL execute 196 -> 366 ms. Over 55 ms:
  stopped for the user's decision.

### Saves work: write, restart, Continue (2026-11-09, measured on the device)

- **Test tool:** `PS2PORT_PAD_SEQ=VALUE:BUTTON[:SETTLE],...` (runner, `display.cpp`): presses driven by the game's state, counted in
  game frames: wait until the word `PS2PORT_PAD_SEQ_WORD` (default the mode `0x38c194`; `*` = any) holds VALUE for SETTLE frames,
  screenshot (to `PS2PORT_FRAME_DIR`; `PS2PORT_FRAME_EVERY=0` keeps only these), press 3 frames, release 5. Timed scripts failed:
  the shop came 30 s earlier in one run, and the Remembrall's 700-900 ms frames swallowed taps.
  Save path used: `5:CROSS:10,4:SELECT:40,5:CROSS:10,2:START:10,3:DOWN:10,3:CROSS:5,6:CROSS:40,6:UP:20,6:CROSS:10,6:CROSS:60`.
- **Creating the save file** (first time, "No ... save data ... create one now? Yes"): the game writes `icon.sys` (964 B),
  `HPATPS.ICO` (117,762 B) and `BASLUS-20826-HPATPS` (8 + 58,464 B) into `mc0/BASLUS-20826-HPATPS/`: worked as it was.
- **Bug fixed (runtime, `Kernel/Stubs/MemoryCard.cpp`):** every later read of the save failed (-4) in a loop. The game (`0x2da3f8`)
  reads with the return value of `sceMcOpen` (0 = success) instead of the descriptor `sceMcSync` returns; on a PS2 mcserv hands out
  the lowest free descriptor, 0. The stub counted up from 1. Now lowest free from 0. Found and checked on the PC first (main menu
  reads the save; `PS2PORT_MC_LOG=1` now also prints open path/flags and read/close descriptors).
- **Saving (`panel-214117`):** "Select file for save" (3 slots), slot 1, "Are you sure ...? Yes" (No is the default), the game
  writes the file (8 + 58,464 B, flush, close), back to the pause menu.
- **Continue after a fresh boot (`panel-220243`):** "Select a Game to Load" lists the save, "Checking memory card ...", "Are you
  sure you wish to load this game?", then **play in Ollivanders at 154 s** (New Game reaches it at ~222 s).
- **Sticky presses (`runner/pad_sdl.cpp`):** a button that went down at a display poll is reported to the next two game reads even
  if released, so a tap is not lost in a slow frame. On by default (`PS2PORT_PAD_STICKY=0` turns it off). **Not yet tried by a
  player** (scripted presses use their own path).
- Notes: the save's time shows 15:45 where the device said 21:45 (clock stub's time zone, 6 h); the device's card now holds this
  save in slot 1 (copy in `D:\ps2port-data\saves\first-save-20261109`, the folder before slot 1 was written).
- Play binary now `1b2f41a8` (chain fix, card fix, sticky presses); before: `runner-aarch64.a2fdbcaf`, `runner-aarch64.5a9eec8d`.

### Input lag: the game ran seconds ahead of the screen (2026-11-10, measured on the device)

- **User report:** very large input lag, movement sometimes only after a couple of seconds; the menu with the fog "lags a lot".
- **Cause (in `panel-220243`, build `1b2f41a8`):** what the game draws waits for the display in a queue bounded in bytes (16 MB
  recorded since the last latch), not in frames. In the main menu with the fog the GPU needs about 60 ms a frame and the game
  makes 60 frames a second, so the game ran ahead until 16 MB were recorded and each latch then replayed about 5 s of frames:
  latch avg 4.6-5.6 s, 0.2 screen updates a second, game frames up to 33 ms with a 1 s stall (the throttle's time-out) about every
  15 frames, all CPU threads nearly idle. The same queue is behind every stretch where the GPU is the slow side.
- **Fix, frame sync (`ps2_memory.cpp` in the private runtime, `runner/display.cpp`):** the game's flip (its BGCOLOR write, the
  last register of the flip in `FUN_002e2268`, armed by the vsync wait's mark at `0x2cd6c8`) sends a marker through stage A and
  stage B behind everything the frame kicked. The main thread latches when a marker is recorded (one latch a whole frame; by guest
  vsync ticks, as before, only when the game has not flipped for 100 ms: loads). If the display is still busy with an earlier
  frame at the flip, the EE waits right there, before it reads the pad for the next frame, until its frame is latched (time-out
  250 ms, VIF1 interrupts served meanwhile); if the display is idle the EE is the slow side and never waits. The 16 MB bound stays
  as a guard. `PS2PORT_FRAME_SYNC=0` turns the waiting off (markers still run). The pad is now polled every 4 ms while the main
  thread waits, not once a latch.
- **New log line:** `[lag]` every 5 s with `PS2PORT_LAG_LOG=1` (on in `scripts/play.sh` runs and with `PS2PORT_M5_STATS`): flips and
  latches a second, flip to replayed (ms, avg and max), how often and how long the game waited, time-outs.
- **Measured, first version (EE waited at the next frame's mark; `panel-004716`, `panel-005158`):**

  | Where | Before (`1b2f41a8`) | With frame sync |
  |---|---|---|
  | Main menu with the fog | one screen update per ~5 s (latch 4.6-5.6 s) | 14-15 updates a second; flip to replayed 58-60 ms avg, 184 max; game waits 52-53 ms a frame |
  | Language screen | - | 59.7 frames a second, flip to replayed 16 ms |
  | Shop (Ollivanders) | 16.7-17.1 game frames/s, 21.8 latches/s | 14.5-18.7 game frames/s, one latch a frame, flip to replayed 19-29 ms avg; game waited 0-3 times per 5 s |
  | Continue to play | 154 s after boot (the menus ran at about a quarter speed) | 58-66 s after boot |

  No time-outs in the fog menu or the shop (one in 130 s of `panel-004716`). Loads take longer in frames than before (mode 0 to
  play 14-15 s, was 10.5 s): the loading screen flips up to 89 times a second on its own and is now paced by the 60 Hz swap.
- **Second version (the one in the play binary `9aba5f4c`):** waiting at the next mark left a finished frame waiting for a whole
  display frame (pad read to screen about 3 GPU frames where the GPU is the slow side); the wait is now at the flip itself and
  only while the display is busy (about 2 GPU frames). Measured in `panel-012130` over boot, language screen (59.7 frames/s, flip
  to replayed 12.7 ms, the wait taken 299 times per 5 s) and the New Game intro: no time-outs. **Not yet measured in the fog menu
  or the shop** (see the next point).
- **The user was at the device during these runs** (`PS2PORT_PAD_LOG=1` in `panel-011233` and `panel-012130` shows real button
  presses), so the scripted routes went different ways each run (New Game instead of Continue, the fog menu left early) and no GS
  dump of the fog menu was captured. Device tests stopped there.
- **Still open:** (1) the fog menu itself: about 60 ms of GPU a frame for about 6 runs and 1,500 vertices (the cost shows in the
  swap, 47 ms avg; `execute` is 14 ms a second), so it runs at about 15 frames a second and the game, which steps per frame, at
  about a quarter speed there. Needs a GS dump of that menu (`PS2PORT_GSDUMP=FILE:SECONDS:2` while the menu is up) replayed with
  `gsgles`; (2) the user's own play test of lag and of sticky presses; (3) game speed in play (the game sees 2 vblanks a frame:
  about half speed at 67 ms frames), unchanged; (4) without a press the language screen stays (seen once, `panel-005713`):
  correct, but scripted runs need a Cross first; (5) the PC runner stops in the New Game intro at the second subtitle (no
  audio there), so the main menu cannot be reached on the PC by New Game's route.

### Stick, heat stop, fog cost, screenshots (2026-11-10, measured on the device)

- **User report:** the stick is odd (left/right only works while already moving up/down), and "the game crashed" when choosing
  the wand.
- **The "crash" was the heat guard:** `play-012555` ended with `THERMAL STOP: 85.0 C` after 318 s (start 63.4 C: the device was
  warm from the test runs before it). Not a game crash. The 85 C stop stays (user).
- **Stick: wrong square roots.** The pad layer delivers the stick correctly (the play log shows X at +/-32767 with Y at 0). The
  game's code was translated with PS2Recomp's semantics, in which `sqrt.s` reads the register field `fs`; the R5900 reads `ft`.
  **16 of the game's 35 `sqrt.s` took the wrong register** (they computed the root of `f0`), among them the matrix-to-angles
  function at `0x2d9090` (`sqrt(m0^2 + m4^2)` became `|m4|`) and three vector lengths in `0x169628`. `runner/gen.sh` now
  generates with the R5900's own semantics for all four known differences (`eegen --hardware-semantics-of sqrt,cvt,branch,lqsq`:
  also `cvt.w.s` truncates instead of rounding to nearest, 810 sites; branches compare 64 bits; `lq`/`sq` mask the address).
  `PS2PORT_HW_SEMANTICS=...` chooses a subset, `PS2PORT_PS2RECOMP_SEMANTICS=1` the old ones (the replay harness keeps those).
- **Trap met on the way:** `runner/gen.sh` without `PS2PORT_MOVIE_STUB=1` builds a runner that stops in the first movie (the
  open movie work); the stub is now the script's default (`PS2PORT_MOVIE_STUB=0` for the movie work).
- **Check (`panel-152205`, scripted, numbers only):** new test presses `LSLEFT`/`LSRIGHT`/`LSUP`/`LSDOWN` in `PS2PORT_PAD_SEQ`
  (`VALUE:NAME:SETTLE:HOLD`, the left stick fully one way for HOLD game frames). In Ollivanders, share of pixels that changed
  between the screenshot at the start and at the end of each 45-frame push: control (R3) 0.6 %, **left 82.2 %, right 81.5 %**,
  up 69.4 %, down 74.7 %. No run of the old build with the same script exists (it has no scripted stick), and the user pressed
  buttons during this run too (8 presses in the log), so this shows that left and right alone now move the view, not a strict
  before/after. Shop frame rate unchanged (14.9-17.9 game frames a second). **The user's own test decides.**
- **Fog on the load-game screen:** a GS dump of it (`D:\ps2port-data\raw\fog-menu.gs`, 2 frames): 160 blended quads with a 64x64
  8-bit texture, linear filter, about 27 screens of overdraw. Replayed with gsplay on the device: **61 ms of GPU a frame, the
  same at 240, 420 and 648 MHz GPU clock**; with nearest sampling 27.5 ms; with one filtered fetch 21-27 ms. The cost was the
  shader's own bilinear of palette lookups (8 fetches). Fix (`gsgles/gs_gles_backend.*`): indexed textures up to 256x256 drawn
  with the linear filter are expanded with their palette into an RGBA texture (cache by both contents, 24 MB budget) and take
  one filtered fetch. gsplay: **21.6 ms a frame at 648 MHz, 28 ms at 240 MHz**; picture against the old path: 32 of 686,587
  bytes differ by more than 2 (max 16). A gameplay dump (`N02_1`): GPU 44.2 -> 35.4 ms per 4 frames, no byte differs by more
  than 2. In the game (`panel-152205`): the load-game screen runs at about 44 flips a second (was 16). `GSGLES_PAL_EXPAND=0`
  turns it off.
- **GPU clock:** the devfreq governor stays at 240 MHz under load (known since Phase 4, never applied to game runs).
  `runner/device_play.sh` and `runner/device_panel.sh` now raise the minimum to 648 MHz for the run and put it back
  (`GPU_MIN_HZ=0` leaves it). `N02_1` on gsplay: GPU 89.4 ms at 240 MHz, 44.3 ms at 648 MHz. Its effect on heat is not measured.
  New tool: `gsgles/device_gpuclock.sh` (one dump at several GPU clocks).
- **Frame sync, second version, measured where it was missing:** load-game screen at 240 MHz and the old shader
  (`panel-103207`): 16 flips a second, flip to replayed 54-57 ms, the game waits 54-57 ms a frame, no time-outs; shop: no waits.
- **Screenshots for the user's visual checks** (division of labour, user 2026-11-10): Menu + L1 on the device, Enter / SIGUSR1 /
  `PS2PORT_SHOT_AT` on the PC runner; `docs/VISUAL_CHECKLIST.md` has the how-to, the template and the list for chapter slice 1;
  `scripts/shots_fetch.sh` copies them to `private/screenshots/device/`; `scripts/png_stats.py` gives hash, lit share and the
  difference to a reference without opening a picture. The main thread copies the frame (once), a thread of its own writes the
  PNG (`runner/host/png_write.h`, stored deflate, about 690 KB); nothing runs on the EE thread. Checked: by time and by signal on
  the device (3 files, 512x447, 14.8-87.9 % lit) and by time on the PC (2 files). **The Menu + L1 press itself is not tested**
  (nobody to press it in a test run). Knulli's Function + L1: see the checklist (not bound outside the frontend's emulators; its
  command `knulli-screenshot` works while the runner owns the display).
- Play binary `e245057a`. Open: the user's test of the stick, of the lag and of Menu + L1; heat (85 C in 5-13 minutes of play);
  game speed; the opening narration needs Start to skip in scripted runs; `cvt.w.s` now truncates while float arithmetic still
  rounds to nearest (the R5900 rounds toward zero): a value that the console computes as exactly N may come out a hair under
  and truncate to N-1; nothing of the kind seen in boot, menus, load and the shop.

### Game speed and sound: the world already runs in real time; sound does not (2026-11-10, code, savestates, device logs, PC)

- **The game steps its world on a fixed 25 Hz clock driven by COP0 Count**, not by vblanks. The main loop (`0x250f28`) reads a
  stopwatch over Count (`0x2f1f58` -> `0x2f2050`) and runs a logic tick (counter `0x38c210`) while
  `tick * 250 < elapsed * 25` (constants at `0x327360` and `0x326e00`, read from a PCSX2 savestate), at most 10 ticks behind.
  The runtime answers Count from the host's clock (`ps2portReadCount`), so the logic follows real time whatever the frame
  rate. **Measured on the device** (`[m5m]` in `panel-152205` and `panel-220243`, play in Ollivanders): **25.0 ticks a second**
  in every 5-s window (24.8-25.3) while 15-18 frames a second were drawn. PCSX2's savestates (one per vblank) fit the same
  rate. The 0x3287F0 count of vblanks per frame, from which the earlier "game speed about 73 % / half" was computed, only
  feeds a debug statistics block (`0x2ea250`, behind a flag at `0x3392e8`). **So the game world is not in slow motion; the
  earlier figures were wrong.** What a low frame rate does cost is smoothness and input-to-screen time, not game speed.
- **Sound is what runs slow.** The IOP (the game's sound driver) and the SPU run on the EE's cycle count, and the EE runs at
  about 55-70 % of a PS2 on the device: the mixer made **24-28 k of 48 k samples a second** in play (`[m5h]`), and the output
  stretches that (slow, low). PC at `PS2PORT_SLOWDOWN=50`: 10.7 k.
- **Tried: the IOP on real time, the EE's clock left alone** (`PS2PORT_IOP_REALTIME=1`, private IOP `iop_emulator.cpp`): the
  IOP thread runs what the EE told it plus extra cycles whenever that falls behind the host's clock (capped at 100 ms after a
  stall), and the EE never waits for those. Sound: **44.6-47.1 k samples a second** on the PC at half speed. But the game's
  stream library needs the IOP in step with the EE: with the IOP ahead from the start the first load crawled; ahead during a
  load, the New Game load took 58 s instead of 38 s; held during loads and cutscenes but ahead after the menus, the next load
  still crawled (over 84 s). Getting back in phase means freezing the IOP (no sound, and the stream stalls) or jumping the EE's
  clock (what broke loads in the 2026-11-08 attempt). **Left off by default**; the code stays for the next attempt.
- **`PS2PORT_REALTIME=1`** (the EE's clock on real time) now holds still during loads and caps a jump at 100 ms. One PC run at
  half speed lost a cutscene skip with it. Off by default.
- **What would fix sound:** an EE fast enough for real time (frame time), or a stream-safe resync of the IOP (for example
  letting it run ahead only while no stream is open: the IOP-side stream state is not identified yet).
- **PC test speed:** `PS2PORT_PRESENT_EVERY=N` presents only every Nth vsync (the software present took about a quarter of the
  game's time). With `DRAW=0/1` the PC reaches the title in ~21 s and loads New Game in 7 s.
- New tools: `scripts/ram_words.py` (guest words from RAM images and PCSX2 savestates), `scripts/perf_symbols.py` (perf output
  of the runner by function), `scripts/ps2mc_extract.py` (lists or copies saves on a PCSX2 card image; the user's `Mcd001.ps2`
  holds none, `Mcd002.ps2` is not in that format).

### Device launcher, documentation (2026-11-10, not run on the device)

- `runner/knulli/HarryPotterPS2.sh`, a Knulli Ports launcher (GPU clock, 85 C stop, log), and `scripts/install_port.sh` to put it
  on the device. Written without device access: never run there (`docs/RUNNING.md`).
- `docs/ENV_VARS.md` (every switch), `docs/RUNNING.md` (the three ways to start, controls, saves, heat).
