# NOTES

Running log: decisions, open questions, commands that worked.
Phase reports live in `docs/PHASE_N.md`.

## Current state and handoff (updated 2026-11-01)

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
- **PS2Recomp trial (PHASE_3 Part B), user-approved 2026-11-01:**
  - Step 1 (recompile) done: every function translated; 98.1 % without
    the per-instruction fallback, 99.93 % with the `jalr` change.
  - Step 2 (PC boot, 2026-11-02): with two fixes, all 7 IOP modules load
    and the disc directory is cached; then the game sleeps forever because
    PS2Recomp has no IOP→EE RPC (PHASE_3 B3). Waiting on the user: build
    that feature in the trial copy, or stop step 2 here.
  - Step 3 (device speed) needs `g++-aarch64-linux-gnu` in WSL (user's
    apt step) and the port-2222 SSH restarted on the device's new address.
  - PS2Recomp lives in `private/third_party/PS2Recomp` (commit
    `e5636981`; all trial changes in
    `private/third_party/trial/ps2recomp-trial.patch`); its output and
    boot runs in `private/third_party/trial/` (derived from the game,
    never committed).

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
- Screen-control mechanics (PCSX2 opens on monitor "P24FBA-RAGL"):
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
| WSL2 (since 2026-10-28) | Ubuntu 26.04.1, WSLg. GCC 15.2 native and `aarch64-linux-gnu`, qemu-user 10.2, CMake 4.2.3, Ninja 1.13.2, SDL 2.32.10, Mesa 26.0.8 (llvmpipe). Repo at `/mnt/d/Projects/anbernic/PS2Recompiler`. sudo needs the user's password |
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
```
