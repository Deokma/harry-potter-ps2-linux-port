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
