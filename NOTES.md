# NOTES

Running log: decisions, open questions, commands that worked.
Phase reports live in `docs/PHASE_N.md`.

## Current state and handoff (2026-11-07)

**A fresh session needs only this section and `docs/PHASE_4.md`.** Older state is under
"History" below; read it only when a pointer here sends you there.

**Goal now (user, 2026-11-06):** a thin slice to the title screen on the RG40XX H with
real GLES picture, audio and input stubbed, no gameplay. No more scene measuring. Plan and
milestones M1–M5: `docs/PHASE_4.md` (approved; results and revised estimates at its end).
**M1 is done on the PC; see "Next" at the end of this section.**

**Rules that changed:** stop only if (a) a measured main frame exceeds its budget (50 ms in
the street, 33.3 ms elsewhere), (b) a decision from the user is needed, or (c) the device
misbehaves (reboot, over 85 C); otherwise go on to the next milestone without asking. **No
screen control at all**, so no new PCSX2 captures unless the user makes them. Don't push.
Keep tool output short.

**Done:**
- EE main frame through eegen (5,500 of 5,708 functions) and VU0 through `eegen/vu0gen.py`
  (13 of 18 entries, all live ones), checked against PCSX2 (PHASE_3 C7, C8). Main frame on
  the device at 1.416 GHz:

  | Frame | Guest instructions | Median | Budget | Same or close to PCSX2 |
  |---|---|---|---|---|
  | `frame-ntsc02r` | 2,861,449 | 24.5 ms | 33.3 ms | 94.9 % |
  | `frame-ntsc03r` | 2,749,423 | 24.1 ms | 33.3 ms | 99.4 % |
  | `frame-i01a` (street, 1 tick) | 6,805,423 | 38.4 ms | 50 ms | 96.8 % |
  | `frame-i01b` (street, 2 ticks) | 7,145,301 | 39.7 ms | 50 ms | 98.6 % |

- PC boot on PS2Recomp's runtime (x86 in WSL, software GS, not the device) shows the
  language screen from about 4 s. With movies stubbed, the boot goes on through the WB logo, the loading screen and the opening cutscene to the main menu.
- PS2Recomp bugs fixed so far: `JR` / `JALR` old register, macro code writing `vf0`,
  byte-at-a-time `memmove`, the pad stub's ID in pressure mode (`0x79`), CP0 `Count` that never advanced, IOP `sceCdGetReadPos`, the IOP allocator never reusing freed memory, holes in the function map (`0x2600e8`, `0x1af300` and others). Replay stand-in now has a real clock (CP0 Count) and scratchpad
  DMA, and resumes the main thread from its kernel-saved registers.

**Not in git (`private/`, `D:\ps2port-data\`):**
- `D:\ps2port-data\raw\`: uncompressed GS dumps for gsplay; `trial/gsplay/`: its builds and frames.
- `private/third_party/trial/harness/`: the replay harness's generated `gen/`, `gen_eg/` (hot set), `gen_eg_fg/`
  (whole program), `gen_vu0/`, `build/`. Its sources are in `harness/`, in git.
- `private/third_party/PS2Recomp/` (commit `e5636981`), changes in
  `trial/ps2recomp-trial.patch`, `ps2recomp-vu-jr-fix.patch`, `ps2recomp-libc-memmove.patch`,
  `ps2recomp-count-emitter.patch`, `ps2recomp-iop.patch`, `ps2recomp-m1-runtime.patch` (the whole diff of the runtime files against that commit, so it
  already holds the trial patch's hunks for those files; it passes `git apply --cached --check`);
  its generated code in `trial/out-b/`.
- `trial/reference/`: PC reference pictures and GIF logs (`menu.png`, `language.png`, `menu-gif.log`); `trial/boot-*/`: each boot's frames and logs.
- `trial/rec/frame-{ntsc02r,ntsc03r,i01a,i01b}.{in,out,vu0code,vu0data}`: checked frames.
- `private/extract/SLUS-20826/files/`: disc files, 1.7 GB (`RESOURCE`, `MOVIES`, IRXs, ELF).
- `private/ghidra/` (`hp.cvs`, `hp.toml`, `SLUS_208.26.c`, `SLUS_208.26.gzf`). The decompile
  has no game symbols but is good for reading control flow.
- `D:\ps2port-data\`: `snapshots/{F01,I01,N01,N02,N03,PILOT}` GS dumps (F01 has the menu),
  `savestates/` (slots 1–3 are the user's; `next-frame-2026-11-05/`,
  `i01-street-2026-11-05/` are ours), `pcsx2-clean/`.

**Rebuild (WSL, repo root), when only harness sources changed; `H=private/third_party/trial/harness`:**
```
REUSE=1 EG=1 EG_REUSE=1 sh harness/build.sh x86 count frame-v0     # binary: $H/build/harness-x86-count-frame-v0-eg
REUSE=1 EG=1 EG_REUSE=1 EG_DIR=$H/gen_eg_fg HARNESS_DEFS=-DHARNESS_LEAN_DISPATCH sh harness/build.sh aarch64 time frame-v4
```
Generator check before any device timing (from `trial/rec`), must print 0 bytes and the exact count:
```
HARNESS_SKIP=0x2cd6a0 HARNESS_STOP_TIMER=2 HARNESS_VU0=3 timeout 100 ../harness/build/harness-x86-count-frame-v0-eg frame-ntsc02r 1
```
Device (Git Bash, settings in `.env`, key-based SSH on port 2222; after a reboot the user
restarts SSH, command under History):
```
sh scripts/harness_device_run.sh $H/build/harness-aarch64-time-frame-v4-eg private/third_party/trial/rec/frame-ntsc02r 20 1416000 -- HARNESS_SKIP=0x2cd6a0 HARNESS_STOP_TIMER=2 HARNESS_PMU=1 HARNESS_VU0=3
```
PC boot (WSL, repo root; a runtime rebuild after a header change takes about 10 min), output
in `trial/boot-<time>/{frames,log.txt}`:
```
(cd private/third_party && cmake --build build-runtime -j 12 2>&1 | grep "error:")
PS2PORT_STATS=1 sh scripts/ps2recomp_trial.sh boot 60 "CROSS@15+0.3/5"
```
Current PC build (`-O1`, no LTO; full compile 11.5 min, a change about 70 s; `build-runtime/` is out of date):
```
(cd private/third_party && cmake --build build-fast -j 12 2>&1 | grep "error:")
PS2PORT_RUNNER=$PWD/private/third_party/build-fast/ps2xRuntime/ps2EntryRunner PS2PORT_IOP_TRACE=1 PS2PORT_STATS=1 sh scripts/ps2recomp_trial.sh boot 120 "CROSS@5+0.3"
```
Configured like `build-runtime` in `scripts/ps2recomp_trial.sh`, with `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -DNDEBUG"` and the same for C.
After regenerating code, run `scripts/ps2recomp_movie_stub.py` and `scripts/ps2recomp_gen_fixes.py` on `trial/out-b` and `ps2xRuntime/src/runner`.
More commands at the end of this file ("Commands that worked").

**Traps:**
- The GPU stays at 240 MHz under load (`simple_ondemand`; `performance` isn't available). Raise `/sys/class/devfreq/gpu/min_freq` for a run, and restore it.
- A swap paces everything to the display on this fbdev driver, whatever the swap interval says. Time GPU work with `GSPLAY_FINISH=1 GSPLAY_NOSWAP=1`.
- Editing source from a heredoc script turned `\n` into real newlines more than once. Use the Edit tool or a script written with the Write tool.
- `trial/harness/gen/` holds one variant at a time: v0 for x86 objects, v3 for aarch64 (left at
  v3). Regenerate with `make_harness.py` before a batch has to recompile.
- The x86 `-eg` binary has eegen's hot set (225 functions), the aarch64 one the whole
  program. Test stop logic against whole-program behaviour: eegen calls directly, so table
  changes don't reach it. That caused the one device reboot (cause not fully established).
- PS2Recomp-only builds need `HARNESS_SKIP=0x2cd6a0`, or the vsync wait spins forever.
- The runtime builds from `ps2xRuntime/src/runner/`, a copy of `trial/out-b/`. Edit both, or a fix to generated code
  is never compiled. One such edit cost a 12 min link and a wrong conclusion.
- A full rebuild is about 35–45 min; run long builds in the background.
- `scripts/ps2recomp_trial.sh boot` works only in WSL. From Git Bash it exits 126 after 0 s.

**Status log (Phase 4, plan approved 2026-11-06):**
- Hygiene done: Ghidra files in `private/ghidra/`; `git ls-files` audited, no game code or
  assets (docs hold addresses and a few values). **Every device run goes through
  `scripts/device_guard.sh`** (85 C, time limit, exit 3 on a thermal stop); `scripts/device_run.sh`
  runs any binary that way, `-g` stops the frontend for the display.
- Thread spike: **pass on x86 and device**, results in `docs/PHASE_4.md`.
- M1 so far, PC only: the black screen is fixed and the language screen appears. Textures
  upload from the VIF1 interrupt handler (`0x2D3398`), which the runtime never ran. It now
  runs in place when a flagged VIFcode completes. Details in `docs/PHASE_4.md`.

**Phase 4's goal is met: the title menu on the device, natively, through the Mali-G31** (`docs/PHASE_4.md`, "M5 result"). On the menu: 60 vsyncs/s, game thread 28 % of a core, about 12 ms per game frame, peak 69.9 C. M1 to M5 results: "M1 result", "M3 result", "M4 result", "M2 result", "M5 result".

**Past New Game (2026-11-07, user: "Get past New Game"): done, and stopped by rule (a).** On the device the cutscenes play and at 250 s Harry stands in a Diagon Alley shop under player control; 260 s with no stall or crash (`panel-122931`). **A gameplay frame takes 107 ms (9.3 per second) against 33.3 ms**, cutscene frames 67-117 ms, game thread at 100 %. Details, profile and what it would take: `docs/PHASE_4.md`, "Past New Game". **Next is the user's call.**
- Game frames are now counted exactly (`[m5f]` lines): a failed PollSema returning to `0x2cd6c8` is one entry into the vsync wait. The old `[m5]` "game frames" from the vblank count's resets undercounts about tenfold in 3D scenes (right on the menu); don't use it there.
- `[m5m]` lines: palettes, targets, depth buffers, textures held. Render targets and depth buffers are still never evicted (34.8 + 29.8 MB at 260 s, growing slowly).
- Profiling on the device: add `PS2PORT_SAMPLE=samples.bin PS2PORT_SAMPLE_FROM=A PS2PORT_SAMPLE_TO=B` to `device_panel.sh`; files land in `panel-*/`; then in WSL `python3 -I scripts/sample_threads.py $X/samples.bin private/third_party/build-m2/aarch64/runner --nm aarch64-linux-gnu-nm --lib private/gsprof/lib --top 20 --thread GameThread | c++filt` (`private/gsprof/lib` holds the device's libc and libmali).
- The private runtime and IOP changes are saved in `trial/ps2recomp-m1-runtime.patch` and `trial/ps2recomp-iop.patch` (`git diff --ignore-cr-at-eol` in the checkout, from WSL).

**Thread split (2026-11-07, user: "Thread split"): done for the GS side, stopped by rule (a).** VIF1, VU1, GIF and GS recording run on `GsThread` (`ps2_memory.cpp`, on unless `PS2PORT_GS_THREAD=0`). In play on the device: **67.7 ms a frame (14.8/s), was 107-113 ms**; game thread 100 %, GS thread 67 % (`panel-142010`). Details: `docs/PHASE_4.md`, "Thread split". Two bugs fixed on the way: the EE scheduler lost idle time when woken early (stalled loading at half the bar), and the silent SPU2 now runs on IOP cycles. `PS2PORT_THROW_LOG=1` logs C++ throws. **Next is the user's call**; the game thread is the limit (guest code and runtime about 60 %, IOP 14 %, copies most of libc's 16 %).

**IOP on its own thread (2026-11-07, user: "IOP on own thread"): done, stopped by rule (a).** `IopThread` (`iop_emulator.cpp`, on unless `PS2PORT_IOP_THREAD=0`), one IOP lock, EE calls catch the IOP up first, IOP calls to the EE thread are queued and waited for. In play: **63.9-64.3 ms, was 67.7**; game thread 100 %, GS 72 %, IOP 10 %; peak 77.9 C (`panel-161110`). Details: `docs/PHASE_4.md`, "IOP on its own thread". The game thread is now limited by guest code and the runtime (`Ps2IsSpecialAddress`, scheduler checks, timers, copies). **Next is the user's call.**
Open: real pad and audio, movies, the game's clock isn't locked to real time while its thread is busy, float groups off in the runner, temperature with three busy cores.
**The GS path's CPU cost is over budget and is the main risk for gameplay:** 35-100 ms per 3D frame on one A53, against a 33 ms frame. GL now runs on the main thread, which takes off the driver's share. Direct vertex handoff costs about 1 ms and can save at most
14-17 ms of it, so it won't close the gap alone: parsing is a quarter to a third, the rest is VRAM and textures, libc and the driver ("M4 result"). A thread split is the other half. Keep calls down: read a file once in a larger piece, apply several edits from one script.

To see the menu on the PC, from the repo root in WSL:
```
PS2PORT_GS_DRAW_WINDOW=3/9 PS2PORT_RUNNER=$PWD/private/third_party/build-fast/ps2xRuntime/ps2EntryRunner PS2PORT_STATS=1 sh scripts/ps2recomp_trial.sh boot 130 "CROSS@5+0.3,START@40+3"
```
Dumps on the device (uncompressed dumps are in `D:\ps2port-data\raw\`, from `scripts/gsdump_unpack.py`):
```
sh gsgles/build.sh gles-aarch64
DEVICE_TIMEOUT=500 sh scripts/device_run.sh -g private/third_party/trial/gsplay/gsplay-gles-aarch64 gsgles/device_table.sh /d/ps2port-data/raw/N02_1.gs -- sh device_table.sh 8 N02_1 N02_1
```
On the panel, from Git Bash (`/dev/fb0` copied at the times given, screenshots and `[m5]` lines in `panel-TIME/` on the device):
```
DEVICE_TIMEOUT=150 sh scripts/device_detach.sh -g private/third_party/build-m2/aarch64/runner-aarch64 runner/device_panel.sh -- sh device_panel.sh 110 '"CROSS@5+0.3,START@40+3"' '"70"'
```
The whole game, in WSL unless it says Git Bash (`gen.sh` once, `build.sh` after any change, a full build takes minutes):
```
sh runner/gen.sh
sh runner/build.sh x86 && sh runner/try.sh 130 "CROSS@5+0.3,START@40+3"
sh runner/build.sh aarch64 && aarch64-linux-gnu-strip -o private/third_party/build-m2/aarch64/runner-aarch64 private/third_party/build-m2/aarch64/runner
# Git Bash:
DEVICE_TIMEOUT=170 sh scripts/device_detach.sh -g private/third_party/build-m2/aarch64/runner-aarch64 runner/device_boot.sh -- sh device_boot.sh 120 '"CROSS@5+0.3,START@40+3"' 2/20
```
VU1 on the frames (WSL for the first two, Git Bash for the device):
```
python3 -I eegen/vu1gen.py private/vu1/slot2/micro.bin private/vu1/vu1_gen.inc --report
sh vu1/build.sh x86 && private/vu1/vu1play-x86 private/vu1/slot2 --mode check --passes 2
DEVICE_TIMEOUT=400 sh scripts/device_run.sh private/vu1/vu1play-aarch64 private/vu1/vu1-frames.tar vu1/device_table.sh -- sh device_table.sh check
```
The device has no `taskset`: `VU1PLAY_CORE=N` pins vu1play itself. Its Wi-Fi drops under load without a reboot: use `scripts/device_detach.sh` for long runs, and check uptime before calling a drop a reboot. Rebuild for the device with `sh vu1/build.sh aarch64`.
Reference pictures and logs: `trial/reference/`. Carry into M2: `docs/PHASE_4.md`, "Carry into M2".

## History (older state; where it differs, the section above wins)

**Where we are:**
- Phase 0 done (`docs/PHASE_0.md`).
- Phase 1 done (`docs/PHASE_1.md`). Part C is a **conditional go**.
  - C8 (2026-11-01) records that every condition is met on the device.
  - It recommends upgrading to **go for Phase 3**. That's pending the
    user's review.
- **Phase 2 done** (`docs/PHASE_2.md`).
  - Part A: the bench suite.
  - Part B: the device run on the **RG40XX H** (Knulli), with results
    against the thresholds, two driver traps and the renderer rules (B5).
- **Phase 3 approved by the user on 2026-11-01** (go, with the three
  conditions in PHASE_1 C8). Step 1, comparing an existing recompiler with
  a minimal one, is `docs/PHASE_3.md` Part A. The choice of recompiler is
  the user's decision (brief: ask before big decisions). Before the
  renderer design is frozen, PAL S03, S05 and S06 must be captured
  (routine below).
- **PS2Recomp trial (PHASE_3 Part B) done 2026-11-02. User chose the
  hybrid route (D3.4); milestone 1 (PHASE_3 Part C) in progress.**
  - Step 1: every function translates; 98.1 % without the fallback.
  - **Milestone 1 status (2026-11-02):** replay states for NTSC slots 2
    and 3 built (`scripts/make_replay_state.py`, frame = one call of
    `0x2635D0`, sp 0x1FFEBD0); the harness now replays a whole program
    against an idle-hardware model (`harness/hwmodel.inc`), with
    PS2Recomp's `LibC.cpp` for math/libc stubs and counted defaults for the
    rest. **Result (C2): the gameplay frame runs correctly on the device
    but takes 124 ms at 1.416 GHz (69 M guest instr/s, 3.7x over the
    33.3 ms budget) with the quick-optimized variant: a stop under plan
    adjustment 3.** Why (C3): 34 % i-cache stalls, 12 host instructions per
    guest instruction, FPU 43.6 % of the mix. The user chose "Prototype hot
    set".
  - **Our generator `eegen/` (2026-11-02/03, PHASE_3 C4–C5):** the whole
    program (5,500 of 5,708 functions; 127 natives and 81 with VU0 macro /
    MMI / COP0 instructions stay PS2Recomp's) is byte-identical to the
    reference with the same 8,540,728 guest instructions, on x86 and on the
    device. **Device: 61.2 ms per frame at 1.416 GHz** (62.5 ms without float
    groups; v4 was 124 ms). Still 1.8x over 30 fps and 1.22x over 20 fps for
    the main frame alone; options A–E and the recommendation (cost VU0
    microprograms, interrupts, threads, IOP first) in C5.
  - **PCSX2 diff done (2026-11-05, PHASE_3 C7; replaces all frame numbers above,
    C2 to C6 measured a frame the game never runs).** True frame: **2,861,449
    guest instructions, 24.5 ms on the device (slot 2); 2,749,423, 24.1 ms (slot
    3): inside 33.3 ms for the main frame alone.** Against PCSX2, 94.9 % and 99.4 %
    of the words it changes come out the same or within 0.1 %; not byte-exact, the
    rest isn't traced. What was wrong: CP0 Count unset (game ran 11 catch-up ticks,
    real frame runs 1), wrong order (replay now resumes the main thread from its
    kernel-saved context and stops at the next wait), scratchpad DMA not copied,
    8,084 VU0 programs per frame not 1,288 (vertex skinning, translated:
    `eegen/vu0gen.py`; interpreted it would take 894 ms). Also from C6: PS2Recomp
    bugs fixed, `JR` reading the old register and macro code writing `vf0`; C4's
    exponent-255 values came from skipped VU0.
  - **Street scene done (2026-11-06, PHASE_3 C8):** the intro's street shot, where the
    stock game drops to 20 frames/s: 6.8–7.1 M guest instructions, **38.4 and 39.7 ms
    on the device, inside the stock game's 50 ms**, 1.2x over 33.3 ms. 96.8 % and
    98.6 % match PCSX2 (same or within 0.1 %). PS2Recomp's `memmove` stub had been
    56.6 % of the frame (92 ms); fixed. All four frames byte-identical across
    generators and x86 / device.
  - **Not counted yet:** interrupt handlers, other EE threads (a loader thread runs in
    the street), VU1, the renderer, IOP, SPU2. Heavier scenes need the PAL savestates.
    Recommended next: VU1 and the renderer, then the rest of the EE, then the heavier
    scenes. **Waiting on the user (condition 3).**
  - **Reaching the street shot:** boot the disc, Cross (Return) on the language screen,
    castle at about 18 s, starry sky at about 24 s; Shift+F12 for slow motion; pause
    (F11) when the OSD reads 9.99 FPS at 50 % speed. Then step and save as below.
    States in `D:\ps2port-data\savestates\i01-street-2026-11-05\`.
  - **PCSX2 stepping that works:** System menu for load / save (slots shown with
    dates; F2 opens a selector that takes the keys), click the title bar before
    every frame-advance key (Home, bound in the clean profile), one vblank per
    press, the screen doesn't redraw while paused. From a state PCSX2 repeats itself
    byte for byte. The EE's registers and the kernel's thread table can be read from
    the save (`cpuRegs`; TCBs at `0x1A698`, 0x4C each). States kept in
    `D:\ps2port-data\savestates\next-frame-2026-11-05\`.
  - **The check for generator changes:** `HARNESS_SKIP=0x2cd6a0
    HARNESS_STOP_TIMER=2 HARNESS_VU0=3` on `frame-ntsc02r`: RDRAM identical to
    `rec/frame-ntsc02r.out`, exactly 2,861,449 guest instructions. Older results
    are in `trial/rec-vu0/` and can't be reproduced.
  - **Trap, device:** a replay that never ends can take the device down (it
    rebooted once on 2026-11-05, cause not established). `harness_device_run.sh` now
    has a timeout (`HARNESS_TIMEOUT`, 120 s). Run new stop logic on x86 with the
    whole-program list first: eegen calls directly, so table changes don't reach it.
  - **Local files without git history, changed 2026-11-05:**
    `trial/harness/{hwmodel.inc,main.cpp,shim.cpp,harness.h,make_harness.py,eg_pipeline.sh,frame_pipeline.sh,eg_bisect.py}`,
    PS2Recomp's `ps2_vu1_lower.cpp` (`trial/ps2recomp-vu-jr-fix.patch`) and `LibC.cpp`
    (`trial/ps2recomp-libc-memmove.patch`).
  - The user's Ghidra export is in the repo root (`hp.toml`, `hp.cvs`,
    `SLUS_208.26.c`, `.gzf`; game-derived, **must not be committed**, and `hp.*`
    aren't git-ignored). 5,345 of its 5,362 functions start where ours do. The
    decompile helps reading only, it has no game symbols.
  - Traps found: PS2Recomp emits nothing for a delay-slot `nop` (count
    convention); its 127 native stubs must stay native (`--native`); GCC
    fuses fmul+fadd on aarch64 unless `-ffp-contract=off`.
  - Build traps (C6): `trial/harness/gen/` holds one variant at a time, v0 for x86
    objects and v3 for aarch64 ones: regenerate it before rebuilding a batch
    (`make_harness.py`, now left at v3). The x86 `-eg` binary has the hot set
    (225 functions), the aarch64 one the whole program. A non-EG build deletes
    a directory's `eg_*` objects: use a copy of the directory.
  - Step 2: the PC boot reaches the game's main loop after 5 fixes (build
    flag, guest memory layout, IOP→EE RPC ~230 lines, our function map,
    `PollSema`). Frames are black: VU1 runs code that never arrived
    (stop 6, open).
  - Step 3 on the device: PS2Recomp's code as is ~63 M guest instr/s at
    1.416 GHz (bar 295); with code generation reworked (scalar registers,
    direct RAM access, no per-loop scheduler call, lean calls) ~290 M/s on
    loop code, ~200 M/s on straight-line code. By the rule: option B.
  - PS2Recomp lives in `private/third_party/PS2Recomp` (commit
    `e5636981`; all trial changes in
    `private/third_party/trial/ps2recomp-trial.patch`); its output, boot
    runs, recordings (`trial/rec/`) and the device harness
    (`trial/harness/`) in `private/third_party/trial/` (derived from the
    game, never committed). The harness and recordings are also on the
    device in `/userdata/ps2bench/harness/`.

**Device access (RG40XX H, 192.168.1.4 since 2026-11-01 (was 192.168.0.165), Knulli):**
- Settings live in `.env` (git-ignored; template `.env.example`): host,
  port 2222, key, `BENCH_DIR`, frontend stop / start, `XDG_RUNTIME_DIR`.
- **After every device reboot** the user runs, in PowerShell, typing the
  device password:
  `cmd /c "ssh root@192.168.1.4 sh -s < D:\Projects\anbernic\PS2Recompiler\scripts\device_ssh_setup.sh"`.
  That starts the key-login SSH on port 2222. Claude never types
  passwords.
- Run the scripts from Git Bash: `bash scripts/deploy.sh`,
  `BENCH_QUICK=1 bash scripts/run_remote.sh`. Build in WSL:
  `wsl -e bash -lc 'cd /mnt/d/Projects/anbernic/PS2Recompiler && scripts/build_bench.sh'`.
- **Driver traps** (PHASE_2 B2):
  - never sub-update a GL buffer per draw: it hung the device;
  - no GPU timer queries around framebuffer copies.
- The device's root password was posted in chat on 2026-11-01. The user
  said they will change it. Never use it.

**Build environment:** WSL Ubuntu 26.04 (see Environment). sudo there needs
the user's password, and Claude never types passwords, so installing any
new package is the user's step.

**Independent of the device:** before the Phase 3 renderer design is
frozen, capture PAL S03, S05 and S06 (PHASE_1 C7).
- They need savestates from the user (`docs/SAVESTATES.md`).
- Use the per-scene routine below.
- The PAL disc is `private\pcsx2\Game\HarryPotter.chd`, with its own slot
  numbering.

**Per-scene routine (what worked on 2026-10-27, Claude doing the clicks):**
- Screen control is agreed **only** via
  `request_access(["PCSX2 (clean profile)"])`. If it fails, stop and ask.
  Never fall back to SendKeys / SendInput.
- Load: F2 (the slot selector shows the slot), then F3. Read the OSD.
- Dumps: hold Ctrl+Shift+F8 for 5 s, then Shift+F8 ×3 at different views.
  Walk with W/A/S/D (`hold_key`, 2–3 s) between the singles.
- EE sweep, repeated for 75 / 60 / 50 %, then back to 100 %:
  1. F11 (pause).
  2. Click the Settings window, open the EE Cycle Rate dropdown, pick the
     value.
  3. Click the game title bar, F11 (resume), F3 (reload).
  4. Zoom on the OSD FPS line once a second for 10–18 s.
  5. The OSD shows `CR=-1/-2/-3` while underclocked.
- `python -I scripts/file_dumps.py <ID>`, then `gsdump_stats.py`, then fill
  in `observations.md`. Then add a §B2x to PHASE_1 and commit.

**Scene status:**

| ID | Source | Status |
|---|---|---|
| PILOT | NTSC cutscene (Harry / Quirrell), no slot | measured (§B2a) |
| N01 | NTSC slot 1: cutscene, Ollivander dialogue | measured + EE sweep (§B2c, B2g) |
| N02 | NTSC slot 2: gameplay, Ollivanders, Harry alone | measured + EE sweep (§B2d, B2g) |
| N03 | NTSC slot 3: gameplay, Ollivanders + Ollivander | measured + EE sweep (§B2b, B2g) |
| I01 | NTSC fresh boot: in-engine intro, Privet Drive (extra, no slot) | measured (§B2e); **stock game runs at 20 fps in the street shot** |
| F01 | NTSC fresh boot: main-menu FMV + EA logo | measured (§B2f) |
| A01 | NTSC slot 1, 108 s with verbose disc reads | measured (§B2h), `reports/A01_reads.json` |
| S01–S08 | PAL slots 1–8 | need savestates from the user first (`docs/SAVESTATES.md`) |

**Known quirks:**
- PCSX2 must be the **clean profile** (patches off, EE 100 %).
  `EECycleRate = 0` and `CdvdVerboseReads = false` were checked in the ini
  at the end of the 2026-10-27 run. Savestate backup:
  `D:\ps2port-data\savestates\backup-2026-10-27\`.
- The clean profile uses the bundle's SCPH-50004 EUR BIOS. Whether it is a
  dump of the user's own console is still unconfirmed.
- Screen-control mechanics (PCSX2 opens on the second monitor, "LG TV" on
  2026-11-05, so `switch_display` first; call `open_application` once only, a
  second call starts a second instance):
  - **While the VM runs, mouse clicks on PCSX2 windows (menus, Settings)
    are ignored.** Pause with F11 first. Hotkeys still work.
  - **Pad keys need a hold of ≥ 0.2 s.** A bare key tap can be missed by
    the game's pad polling.
  - `hold_key` takes one chord, so you can't walk during a multi-frame
    dump.
  - Window drags that end over the desktop are refused. Keep the Settings
    window overlapping the game window and switch focus by clicking each
    one's visible part.
  - After a menu click, click the game title bar before pressing pad keys.
  - Don't click inside the game image: the mouse is bound to the right
    stick and the left button is Square.

## Hard rules (from the brief)

- Game data never enters git: ISO / CHD / ELF / IRX / BIOS / extracted assets
  stay in `./private/` (ignored). `.gitignore` covers this, and
  `.githooks/pre-commit` refuses any staged ELF, CHD, ISO9660 image or
  file > 2 MiB. Enable the hook once per clone:
  `git config core.hooksPath .githooks`
- Never download game files or BIOS images.
- qemu-user timings are not device timings. Label them.
- Nothing "runs on the device" until it ran on the device.
- Stop after each phase and write `docs/PHASE_N.md`.
- Code generated from the game's ELF (recompiler output) is derived
  from the game: it goes to `private/` and is never committed.

## Environment (2026-10-27)

| Item | Value |
|---|---|
| OS | Windows 11 Pro 10.0.26200, x64 |
| CPU | AMD Ryzen 5 5600H, 6C/12T, AVX2 |
| RAM | 28 GB visible |
| GPU | RTX 3050 Laptop (driver 32.0.16.1088) + Radeon iGPU |
| Free disk | C: 12.6 GB (tight), D: 86.9 GB, E: 437 GB |
| Python | 3.14.0 (has `lzma`, `zlib`, `compression.zstd`) |
| git | 2.55.0.windows.5 |
| CMake | present (`D:\Programs\CMake`) |
| WSL2 (since 2026-10-28) | Ubuntu 26.04.1, WSLg. GCC 15.2 native and `aarch64-linux-gnu` (C, and C++ since 2026-11-02), qemu-user 10.2, CMake 4.2.3, Ninja 1.13.2, SDL 2.32.10, Mesa 26.0.8 (llvmpipe). Repo at `/mnt/d/Projects/anbernic/PS2Recompiler`. sudo needs the user's password |
| Missing on Windows | gcc / clang (MSVC exists in VS 18 but is unused), 7z, xorriso, chdman |
| PCSX2 | 2.6.3 portable, inside the local game bundle folder |

## Decisions

- **D0.1 Pure-Python disc reader.** `scripts/extract_disc.py` parses ISO9660
  itself and reads CHD v5 directly (lzma/zlib/huff/flac/zstd hunks, logic
  ported from libchdr). Reason: zero installs for Phase 0, no 7z/xorriso/
  chdman on this machine, and the same script will run unchanged in WSL.
  Correctness checks: CHD map CRC16, per-hunk CRC16, optional full SHA-1
  (`--verify`) against the CHD header, PCSX2's ELF CRC cross-check.
- **D0.2 Output layout.** `private/extract/<SERIAL>/{manifest.json,
  layout.tsv, files/...}`. Output is deterministic (verified byte-identical
  across runs) so it can be diffed.
- **D0.3 Line endings.** `.gitattributes` forces LF: scripts will run in WSL
  and on the device.

- **D1.1 Primary disc NTSC-U SLUS-20826, PAL SLES-52055 secondary**
  (user decision, 2026-10-27). Same boot ELF; PAL savestates are OK for
  per-frame workload measurements, frame pacing scaled 25 → 30 fps.
- **D1.2 Phase 1 static tooling is home-grown Python** (`analyze_elf.py`,
  `analyze_irx.py`, `scan_code_in_data.py`, `inspect_savestate.py`): enough
  for a census and estimates without installing Ghidra/rabbitizer. Phase 3
  will need a real R5900 decoder; evaluate existing ones then.
- **D1.3 Bulk data lives in `D:\ps2port-data\`** (user decision): clean
  PCSX2 profile, savestates, GS dumps/screenshots, memcard copies, logs,
  reports. It is outside the repo (git can't see it); `.gitignore` lists
  `/ps2port-data/` in case it's ever linked in. Nothing large goes to C:.
- **D1.4 The user does every PCSX2 click; no screen control** (user
  decision). Steps live in `docs/MEASURE_RUNBOOK.md`; files are read
  straight from `D:\ps2port-data`. *Superseded 2026-10-27 (late):* the
  user agreed to screen control through the "PCSX2 (clean profile)" app
  only, and Claude ran the N01–N03 / I01 / F01 / A01 sessions.
- **D1.5 Measurement savestates only from the clean profile.** The bundle's
  "50 FPS" patch writes game RAM (`0x3A4FD8`), and a savestate would carry
  that value with patches off.
- **D1.6 Disc and bundle moved into `private/`** (user OK):
  `private/disc/SLUS-20826.iso`, `private/pcsx2/` (relative paths in its
  ini, so it still works).
- **D1.7 Report max and p95 per scene**, not just means (user addition a);
  the EE Cycle Rate sweep is only a coarse proxy, to be cross-checked with
  prototype instruction counts in Phase 3 (user addition d).
- **D1.8 Extra scene I01** (Claude, 2026-10-27): the in-engine intro after
  a fresh boot (Privet Drive street). It was captured because it is the
  only outdoor scene reachable without PAL savestates. It has no savestate,
  so no EE sweep. F01 covers the main-menu FMV and the EA logo, the two
  movies the boot shows.
- **D1.9 Part C written without the PAL scenes** (user decision,
  2026-10-27). The verdict is conditional go on NTSC data. PHASE_1 C5
  lists what S01–S08 could change, and C7 makes S03 / S05 / S06 a gate
  before the Phase 3 renderer design.

- **D2.1–D2.9 Benchmark design:** see `docs/PHASE_2.md` §A2.
  - `bench_cpu` is static.
  - `bench_gl` links only against libc and loads SDL2 with `dlopen`.
  - `_GNU_SOURCE` is confined to one file, and builds use no FMA.
  - FPU-mode switches are noinline asm with memory clobbers.
  - T6 gets exact guest counts from a counting build.
  - Device access is SSH key only.

- **D3.1 Trial changes to PS2Recomp stay in a private patch**
  (`private/third_party/trial/ps2recomp-trial.patch`), not in the repo:
  PS2Recomp is GPL-3.0 and the recompiler choice is still open.
- **D3.2 The device harness is private too** (it builds against PS2Recomp's
  headers and game-derived code). Game RAM recordings for it live in
  `private/third_party/trial/rec/` and, for runs, on the user's own device.
- **D3.3 Step 3 figures are taken at the `performance` governor
  (1.512 GHz) and scaled to 1.416 GHz** for the bar, as in Phase 2; the
  governor is restored after each run.

- **D3.4 Recompiler route (user decision, 2026-11-02): hybrid.** Fork
  PS2Recomp's runtime, write our own code generator. **The port is
  GPL-3.0.** Conditions:
  1. First milestone, before any other Phase 3 work: a real gameplay
     frame's main update runs on the device through the new generator,
     state from the PCSX2 savestates (NTSC slots 2–3 first, then PAL S03,
     S05, S06 once captured), verified byte-for-byte against PCSX2 or the
     PC run, measured at the 1.416 GHz budget clock.
  2. Report instr/s, instruction-cache behaviour and the IOP interpreter's
     share of time (STREAM.IRX etc.); say whether native HLE of STREAM.IRX
     is needed.
  3. If the frame misses the budget even with the optimized generator,
     stop and list concrete further options before building anything
     else.
  4. PS2Recomp sources, the patch, generated code and recordings stay in
     `private/`. Document which parts of our code derive from PS2Recomp
     (`docs/PROVENANCE.md`).

## Open questions

- Q0.3 BIOS: the user placed the bundle's SCPH-50004 (EUR v1.90) files in
  `D:\ps2port-data\pcsx2-clean\bios\` and selected it (2026-10-27). It is
  still unconfirmed whether that is a dump of their own console. It stays
  outside the repo; never copy or commit BIOS files.
- Q2.1 Which firmware will the device run (muOS / Knulli / ROCKNIX)? Its
  glibc version (≥ 2.34 for `bench_gl`), its frontend stop / restart
  commands for `BENCH_PRE` / `BENCH_POST`, and its GL driver (Mali blob or
  Panfrost) all depend on it.
- Resolved Q1.4: WSL2 Ubuntu 26.04 installed, and the build packages were
  added by the user (2026-10-28).
- Q1.6 Screen control: the computer-use tool only resolves apps from a
  list read at session start, so it can't target the portable PCSX2 in
  this session. A per-user Start-menu shortcut
  `%APPDATA%\Microsoft\Windows\Start Menu\Programs\PCSX2 (clean profile).lnk`
  was created (2026-10-27) so a new session can see it. Delete it when no
  longer wanted.
- Resolved Q1.5: NTSC slot 1 = in-engine cutscene (Ollivander dialogue),
  slots 2 and 3 = gameplay in Ollivanders (from embedded screenshots).
- Resolved: Q1.2 (0x70000–0x96000 is static: ≤23 of 38,912 words differ
  across three states); Q1.3 (pilot validated `gsdump_stats.py`, PHASE_1
  B2a).

## Facts worth remembering (verified)

- NTSC ISO = redump-clean (volume prefix MD5 a1e5106d…; trailing 160
  sectors zero). PAL CHD decodes with matching header SHA-1.
- Same boot ELF on both discs: SHA-256 8f86d879…a387, PCSX2 CRC C71A6146.
- PCSX2 GameIndex: only `EETimingHack` for both serials; no GS/clamp fixes.
- Stock frame pacing: 2 vsyncs per frame (`0x3A4FD8` = 2), per the
  "50 FPS" pnach.
- PCSX2 2.6.3 savestates are zips with zstd members; Python 3.14's
  `zipfile` reads them directly.
- The user's bundle ran with EE cycle rate 130 % (`EECycleRate = 1`) and
  MTVU on; the clean profile uses 100 % and MTVU off.
- PCSX2 2.6.3 hotkeys (source-checked): `GSDumpSingleFrame` fires on key
  release, 1 frame; `GSDumpMultiFrame` records while held and stops on
  release. Dumps and screenshots go to the Snapshots folder.
- GS dump format (v2.6.3): u32 0xFFFFFFFF, u32 header size, 9×u32
  GSDumpHeader, serial, screenshot, GS freeze data (u32 version, 15 env
  u64s, 2×12 context u64s at offset 124, …, 4 MiB VRAM, 4×(GIFTag + u32
  reg), f32 q), 8 KiB privileged regs; packets 0 = transfer (u8 path,
  u32 size), 1 = vsync (u8 field), 2 = FIFO read (u32), 3 = priv regs.
  Path index 0 (old PATH1) carries VU1 memory up to its end after EOP.
- Real 2.6.3 dumps log **every** GIF transfer as index 3 (all paths go
  through the GIF unit), so the source path is unknown; only index 0
  (old-style PATH1) has trailing VU1 memory after EOP.
- This game keeps one display buffer (512×448 CT16 at FBP 140) and starts
  each frame by copying the finished 640×448 CT32 frame into it with 16
  sprites. `gsdump_stats.py` splits frames at that "present".
- The NTSC disc runs in NTSC mode with the EUR BIOS (SMODE1 CMOD = 2,
  448 lines); the pilot cutscene ran at a steady 2 vsyncs per frame.
- GS dump size: a ~3 s multi-frame dump of a cutscene was 93 MB zstd,
  parsed in ~20 s.
- `EmuCore.CdvdVerboseReads = true` makes PCSX2 log every disc read as
  `DvdRead: Reading Sector NNNNNNN (NNN Blocks of Size 2064) …` via
  `Console.WriteLn` (goes to emulog with timestamps). 2064 is the raw DVD
  block; the user data is 2048.
- Boot order (NTSC, fast boot):
  1. language select;
  2. EA logo, a 24 fps movie;
  3. WB logo, in-engine;
  4. a loading screen;
  5. the in-engine intro (I01);
  6. the main menu, which is a 24 fps PSS movie with the menu drawn over
     it.
- The OSD's "DRW" (PCSX2's own HW draws) isn't the same count as
  `gsdump_stats.py` "draws" (state-change runs). Compare like with like.
- A 5 s Ctrl+Shift+F8 hold records ~130–230 vsyncs (60–80 frames), not
  300. Recording slows the host, and capture starts late.
- In this game's gameplay, EE time is roughly 50–60 % of a 2-vsync frame
  (EE sweep: 30 fps at 60 %, 20 fps at 50 %). The I01 street shot is
  already at 20 fps at 100 % (PHASE_1 B2g).
- Streaming: about 44–66 kB/s steady from the level `.HOG` and the
  `/CUT/*.HOG` cutscene archive, which looks like one stereo ADPCM stream.
  The mean is 85 kB/s, with bursts of about 0.7 MB/s at scene changes
  (PHASE_1 B2h).
- RG40XX H CPU (`/proc/cpuinfo`): Cortex-A53 (part 0xd03), features
  `fp asimd aes pmull sha1 sha2 crc32`, so PS2Recomp's ARM flags
  `-march=armv8-a+fp+simd+crypto+crc` are safe. Governor `schedutil`.
- PS2Recomp (commit `e5636981`):
  - its runtime needs no BIOS image: `PS2RomDevice` makes up `rom0:`
    with only a `ROMVER` file;
  - its fallback warning lists every indirect jump of a function, not
    only the unresolved ones, and its report counts each fallback function
    twice. Count unique function names in the log instead.
  - building its runtime with GCC on x86-64 needs `-msse4.1` (its CMake
    sets flags only for MSVC and ARM). raylib is fetched with full history
    (~500 MB); `FETCHCONTENT_SOURCE_DIR_RAYLIB` with a shallow 5.5 clone
    avoids that. Copying the 5k generated files over `/mnt/d` takes ~5 min.
  - its IOP side stores SPU2 registers but emulates no SPU2: sound modules
    run, nothing plays.
  - WSL has `perf` (`/usr/lib/linux-tools/.../perf`) for profiling.

## Commands that worked

```bash
# Phase 0: disc inventory (ISO or CHD), output under private/extract/<SERIAL>/
python -I scripts/extract_disc.py private/disc/SLUS-20826.iso --hash
python -I scripts/extract_disc.py private/pcsx2/Game/HarryPotter.chd
# Full CHD integrity check (decodes all 4.7 GB in Python: ~20 min)
python -I scripts/extract_disc.py private/pcsx2/Game/HarryPotter.chd --verify
# Extract more files by glob, or everything
python -I scripts/extract_disc.py IMAGE --extract "/MOVIES/*" --extract "*.HOG"
python -I scripts/extract_disc.py IMAGE --all

# Phase 1: static analysis (outputs under private/analysis/SLUS_208.26/)
python -I scripts/analyze_elf.py private/extract/SLUS-20826/files/SLUS_208.26
python -I scripts/analyze_irx.py private/extract/SLUS-20826/files \
    --json private/analysis/SLUS_208.26/iop_modules.json
python -I scripts/scan_code_in_data.py private/disc/SLUS-20826.iso \
    --json private/analysis/SLUS_208.26/code_in_data.json      # ~1 min
python -I scripts/inspect_savestate.py STATE.p2s ELF \
    --patches-zip private/pcsx2/Engine/resources/patches.zip --json OUT.json

# Phase 1: measurement tooling
python -I scripts/make_pcsx2_profile.py          # (re)build D:\ps2port-data\pcsx2-clean
python -I scripts/pcsx2_set.py EmuCore.CdvdVerboseReads=true   # PCSX2 closed
python -I scripts/file_dumps.py S02              # file new dumps under snapshots\S02
python -I scripts/gsdump_stats.py D:/ps2port-data/snapshots/S02 --scene S02
python -I tests/test_gsdump_stats.py             # parser self-test
# VU1 innermost loops: cycles and reciprocals per iteration -> cycles/vertex
python -I scripts/vu1_loops.py private/extract/SLUS-20826/files/SLUS_208.26 \
    --json private/analysis/SLUS_208.26/vu1_loops.json
# disc reads (CdvdVerboseReads log) per file and per 5 s window
python -I scripts/cdvd_reads.py D:/ps2port-data/reports/A01_emulog.txt \
    private/extract/SLUS-20826/layout.tsv --after "Loaded state from slot 1" \
    --json D:/ps2port-data/reports/A01_reads.json

# Phase 2 (in WSL, from /mnt/d/Projects/anbernic/PS2Recompiler)
scripts/build_bench.sh                 # both builds, self-tests (qemu), quick CPU runs
scripts/build_bench.sh --gl-smoke      # + bench_gl --quick in a WSLg window (llvmpipe)
qemu-aarch64 bench/build/aarch64/bench_cpu --selftest
BENCH_HOST=root@<ip> scripts/deploy.sh
BENCH_HOST=root@<ip> BENCH_QUICK=1 BENCH_PRE='...' BENCH_POST='...' scripts/run_remote.sh
BENCH_HOST=root@<ip> BENCH_GOVERNOR=performance BENCH_PRE='...' BENCH_POST='...' scripts/run_remote.sh
# From Git Bash, call WSL with path conversion off:
#   MSYS_NO_PATHCONV=1 wsl -e bash -c 'cd /mnt/d/Projects/anbernic/PS2Recompiler && scripts/build_bench.sh'

# Phase 3 trial (in WSL, from the repo root; all output under private/third_party/)
python3 scripts/export_function_map.py ELF private/third_party/trial/function_map.csv
scripts/ps2recomp_trial.sh all          # clone + build tools, analyzer, runs A and B
scripts/ps2recomp_trial.sh c            # run C: needs trial/ps2recomp-trial.patch applied
scripts/compile_generated.sh private/third_party/trial/out-b \
    private/third_party/trial/obj-x86 g++ -msse4.1   # x86-64 compile check, ~25 min
scripts/ps2recomp_trial.sh runtime b    # build the runner from run B (needs the trial patch)
scripts/ps2recomp_trial.sh boot 180 "CROSS@20+0.3/5"   # boot in a WSLg window, frames + log
PERF=1 scripts/ps2recomp_trial.sh boot 60            # same, sampled with perf
# record one guest call for the device harness (.in/.out/.targets)
PS2PORT_SNAP_PC=0xADDR PS2PORT_SNAP_NTH=1 PS2PORT_SNAP_FILE=/abs/path/rec \
    scripts/ps2recomp_trial.sh boot 60
# harness (private/third_party/trial/harness): sources for that recording, then build
python3 -I harness/make_harness.py REC private/third_party/trial/out-b \
    private/third_party/trial/harness
sh harness/build.sh aarch64 time    # or x86, count
# milestone 1: replay state for the gameplay frame from a savestate (game data -> private/)
python -I scripts/make_replay_state.py "D:/ps2port-data/savestates/SLUS-20826 (C71A6146).02.p2s"     private/third_party/trial/ctx_offsets.json private/third_party/trial/rec/frame-ntsc02     --pc 0x2635d0 --sp 0x1ffebd0 --gp 0x3a7570
# whole-program reference + quick-optimized aarch64 build (WSL, ~45 min)
sh harness/frame_pipeline.sh frame-ntsc02
# device run at the pinned budget clock (Git Bash)
sh scripts/harness_device_run.sh HARNESS REC 20 1416000 -- HARNESS_SKIP=0x2cd6a0 HARNESS_PMU=1
# time profile on the device: add HARNESS_SAMPLE=/userdata/ps2bench/harness/x.samples to the run,
# scp it back, then (WSL) per function / hot list:
python3 -I scripts/sample_profile.py HARNESS SAMPLES --nm aarch64-linux-gnu-nm --hot OUT.hot --cover 100
# eegen (our generator) on a function list, into the harness (game-derived output -> private/)
python3 -I eegen/eegen.py private/extract/SLUS-20826/files/SLUS_208.26 private/third_party/trial/function_map.csv \
    private/third_party/trial/harness/gen_eg --functions LIST \
    --ps2recomp-table private/third_party/trial/out-b/register_functions.cpp --skip 0x2cd6a0
REUSE=1 EG=1 sh harness/build.sh x86 count frame-v0   # only eegen's objects rebuild
# whole program through eegen: x86 check + aarch64 build (WSL, ~35 min); binaries end in -eg-TAG
sh harness/eg_pipeline.sh frame-ntsc02 LIST TAG
# RAM diff by region (e.g. replay result vs a PCSX2 savestate one frame later)
python -I scripts/diff_ram_regions.py A B private/extract/SLUS-20826/files/SLUS_208.26 --mask 0x...:0x...
# VU0 in the replay: HARNESS_VU0= 0 skipped, 1 interpreter, 2 recorded results,
# 3 translated with interpreter fallback, 4 differential check of every call.
# HARNESS_TIMES=1 per-iteration times, HARNESS_DUMP_OUT=FILE the result in .out format
python3 -I eegen/vu0gen.py private/third_party/trial/rec/frame-ntsc02.vu0code \
    private/third_party/trial/harness/gen_vu0/vu0_programs.inc --entries 0x1,0x4,0x7,0xa,0xd,0x10,0x13,0x16,0x19,0x1c,0x1f,0x22,0x25,0x28,0x2b,0x2e,0x31,0x34
# relink after harness changes (gen/ must hold the right variant if a batch has to recompile)
REUSE=1 EG=1 EG_REUSE=1 sh harness/build.sh x86 count frame-v0
REUSE=1 EG=1 EG_REUSE=1 EG_DIR=private/third_party/trial/harness/gen_eg_fg HARNESS_DEFS=-DHARNESS_LEAN_DISPATCH \
    sh harness/build.sh aarch64 time frame-v4
# which saves hold a whole game frame, and the make_replay_state.py arguments for each
python -I scripts/frame_states.py STATE.p2s STATE.p2s ...
# an aligned frame: the state one vblank after the save (main thread still in the wait), its kernel-saved
# registers (TCB's sp, here 0x1ffe670), vblank count satisfied, a Count that gives the elapsed time the game read
python -I scripts/make_replay_state.py "D:/ps2port-data/savestates/next-frame-2026-11-05/slot2+1.p2s" private/third_party/trial/ctx_offsets.json \
    private/third_party/trial/rec/frame-ntsc02r --pc 0x2cd6f0 --sp 0 --gp 0 --from-block 0x1ffe670 --poke 0x328490=2 --count 268444254
# x86 check (from trial/rec), device run (from Git Bash); HARNESS_DMA_LOG=1 lists DMA kicks
HARNESS_SKIP=0x2cd6a0 HARNESS_STOP_TIMER=2 HARNESS_VU0=3 ../harness/build/harness-x86-count-frame-v0-eg frame-ntsc02r 1
sh scripts/harness_device_run.sh private/third_party/trial/harness/build/harness-aarch64-time-frame-v4-eg private/third_party/trial/rec/frame-ntsc02r 20 1416000 -- HARNESS_SKIP=0x2cd6a0 HARNESS_STOP_TIMER=2 HARNESS_PMU=1 HARNESS_VU0=3
# against PCSX2: start, replay, the state one game frame later
python -I scripts/frame_diff.py private/third_party/trial/rec/frame-ntsc02r.in private/third_party/trial/rec/frame-ntsc02r.out "D:/ps2port-data/savestates/next-frame-2026-11-05/slot2+2.p2s" private/extract/SLUS-20826/files/SLUS_208.26
```
