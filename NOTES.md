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

## Open questions

- Q0.1 Which disc is canonical for development: NTSC-U SLUS-20826 (ISO) or
  PAL SLES-52055 (CHD)? Boot ELF and IOP modules are byte-identical, so
  this choice is about data and video mode only. (See PHASE_0.md.)
- Q0.2 Move the ISO and the PCSX2 bundle folder into `./private/`? Both are
  git-ignored where they are now.
- Q0.3 BIOS for PCSX2 profiling: the bundle has an SCPH-50004 (EUR v1.90)
  BIOS in `Engine/bios`. I have not read, copied or hashed it. Per the rules
  it must be a dump from the user's own console; needs confirmation.
- Q0.4 The bundle's per-game settings enable "Widescreen 16:9" and
  "50 FPS" patches for SLES-52055. These must be off for Phase 1 profiling.
- Q0.5 Is the NTSC ISO a clean dump? redump.org (HTTP only) could not be
  reached from here; compare MD5/SHA-1 by hand (values in PHASE_0.md).

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
```
