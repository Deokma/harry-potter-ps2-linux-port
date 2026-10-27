# NOTES

Running log: decisions, open questions, commands that worked.
Phase reports live in `docs/PHASE_N.md`.

## Phase 2 status (2026-10-28, supersedes the Phase 1 handoff below)

- **Phase 2 Part A is done** (`docs/PHASE_2.md`): `bench/` builds natively
  and for aarch64 in WSL (`scripts/build_bench.sh`). Its self-tests pass,
  natively and under qemu, with identical checksums, and every test ran
  in quick mode. Timings from qemu or the desktop are not device timings.
- **Part B waits for the device.**
  1. You enable SSH key login on the device.
  2. `scripts/deploy.sh`, then `scripts/run_remote.sh`.
  3. I compare the results with PHASE_2_DEVICE_BENCH and update PHASE_1
     Part C.
- WSL now has the build tools (the user installed them on 2026-10-28;
  this resolves Q1.4). `bench_gl` needs glibc 2.34 or newer on the device.

## Current state and handoff (updated 2026-10-28 00:30)

**Where we are:**
- Phase 0 done (`docs/PHASE_0.md`).
- **Phase 1 done.** It now waits for the user's review and go-ahead.
  - Part A: static analysis.
  - Part B: every NTSC scene measured (`docs/PHASE_1.md` §B2a–B2h). The
    PAL scenes S01–S08 are deferred (user decision, 2026-10-27).
  - Part C: **conditional go**, written from the NTSC data only (§Part C).
    C5 marks what the missing PAL scenes could change.
- `docs/PHASE_2_DEVICE_BENCH.md` thresholds are filled in from Part C.
- **Stop here.** Don't start Phase 2 until the user says go.

**Next steps once the user says go:**
1. Phase 2, per `docs/PHASE_2_DEVICE_BENCH.md`:
   - `bench/`, the aarch64 cross toolchain, `deploy.sh` / `run_remote.sh`;
   - the critical tests are T1 (draw rate), T2 (fill) and T4 / T5
     (uploads).
2. Before the Phase 3 renderer design is frozen, capture PAL S03, S05 and
   S06 (Part C, C7). Use the same per-scene routine as for N01–N03
   (below). The PAL disc is `private\pcsx2\Game\HarryPotter.chd`, with its
   own slot numbering.

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
| Missing | WSL (not installed), gcc/clang/MSVC, ninja, make, 7z, xorriso, qemu, chdman |
| Virtualization | enabled in firmware, hypervisor present (WSL2 can run) |
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

## Open questions

- Q0.3 BIOS: the user placed the bundle's SCPH-50004 (EUR v1.90) files in
  `D:\ps2port-data\pcsx2-clean\bios\` and selected it (2026-10-27). It is
  still unconfirmed whether that is a dump of their own console. It stays
  outside the repo; never copy or commit BIOS files.
- Q1.4 WSL2 install in progress on the user's side (reported 2026-10-27).
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
```
