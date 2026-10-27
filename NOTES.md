# NOTES

Running log: decisions, open questions, commands that worked.
Phase reports live in `docs/PHASE_N.md`.

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
  straight from `D:\ps2port-data`.
- **D1.5 Measurement savestates only from the clean profile.** The bundle's
  "50 FPS" patch writes game RAM (`0x3A4FD8`), and a savestate would carry
  that value with patches off.
- **D1.6 Disc and bundle moved into `private/`** (user OK):
  `private/disc/SLUS-20826.iso`, `private/pcsx2/` (relative paths in its
  ini, so it still works).
- **D1.7 Report max and p95 per scene**, not just means (user addition a);
  the EE Cycle Rate sweep is only a coarse proxy, to be cross-checked with
  prototype instruction counts in Phase 3 (user addition d).

## Open questions

- Q0.3 BIOS: the user placed the bundle's SCPH-50004 (EUR v1.90) files in
  `D:\ps2port-data\pcsx2-clean\bios\` and selected it (2026-10-27). It is
  still unconfirmed whether that is a dump of their own console. It stays
  outside the repo; never copy or commit BIOS files.
- Q1.4 WSL2 install in progress on the user's side (reported 2026-10-27).
- Q1.5 Which scenes are in NTSC savestate slots 1 and 2 (made 22:34–22:35
  on 2026-10-27)?
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
  `DvdRead: Reading Sector NNNNNNN (NNN Blocks of Size 2048) …` via
  `Console.WriteLn` (goes to emulog with timestamps).

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
```
