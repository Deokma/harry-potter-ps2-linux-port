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

## Open questions

- Q0.2 Move the ISO and the PCSX2 bundle folder into `./private/`? Both are
  git-ignored where they are now. (No answer yet; left in place.)
- Q0.3 BIOS for PCSX2 profiling: the bundle has an SCPH-50004 (EUR v1.90)
  BIOS in `Engine/bios`. I have not read, copied or hashed it. Per the rules
  it must be a dump from the user's own console; needs confirmation
  before any PCSX2 run.
- Q0.4 The bundle's per-game settings enable "Widescreen 16:9" and
  "50 FPS" patches for SLES-52055. Phase 1 profiling uses a separate clean
  profile with patches off (PHASE_1.md B0).
- Q1.1 Approval of the Phase 1 measurement plan (PHASE_1.md Part B) and
  savestates for the listed scenes.
- Q1.2 What exactly is resident at EE 0x80000–0x95060 (likely EELOAD
  leftovers)? Check that it is unchanged across savestates.

## Facts worth remembering (verified)

- NTSC ISO = redump-clean (volume prefix MD5 a1e5106d…; trailing 160
  sectors zero). PAL CHD decodes with matching header SHA-1.
- Same boot ELF on both discs: SHA-256 8f86d879…a387, PCSX2 CRC C71A6146.
- PCSX2 GameIndex: only `EETimingHack` for both serials; no GS/clamp fixes.
- Stock frame pacing: 2 vsyncs per frame (`0x3A4FD8` = 2), per the
  "50 FPS" pnach.
- PCSX2 2.6.3 savestates are zips with zstd members; Python 3.14's
  `zipfile` reads them directly.

## Commands that worked

```bash
# Phase 0: disc inventory (ISO or CHD), output under private/extract/<SERIAL>/
python -I scripts/extract_disc.py "Harry Potter and The Sorceror's Stone.iso" --hash
python -I scripts/extract_disc.py "Harry Potter and the Philosopher's Stone/Game/HarryPotter.chd"
# Full CHD integrity check (decodes all 4.7 GB in Python: slow)
python -I scripts/extract_disc.py ".../HarryPotter.chd" --verify
# Extract more files by glob, or everything
python -I scripts/extract_disc.py IMAGE --extract "/MOVIES/*" --extract "*.HOG"
python -I scripts/extract_disc.py IMAGE --all

# Phase 1: static analysis (outputs under private/analysis/SLUS_208.26/)
python -I scripts/analyze_elf.py private/extract/SLUS-20826/files/SLUS_208.26
python -I scripts/analyze_irx.py private/extract/SLUS-20826/files \
    --json private/analysis/SLUS_208.26/iop_modules.json
python -I scripts/scan_code_in_data.py "Harry Potter and The Sorceror's Stone.iso" \
    --json private/analysis/SLUS_208.26/code_in_data.json      # ~1 min
python -I scripts/inspect_savestate.py STATE.p2s ELF \
    --patches-zip ".../Engine/resources/patches.zip" --json OUT.json
```
