# Phase 0: Setup and disc inventory

Status: **done, waiting for go-ahead.** Date: 2026-10-27.

> Update (Phase 1): both images have since moved into `private/`. The NTSC
> ISO is now `private/disc/SLUS-20826.iso` and the PCSX2 bundle is
> `private/pcsx2/`. Paths below are as they were during Phase 0.

## TL;DR

- Two images of the game are on this machine: an **NTSC-U ISO (SLUS-20826)**
  and a **PAL CHD (SLES-52055)** inside a portable PCSX2 2.6.3 bundle.
- **Both regions ship the byte-identical boot ELF** (2,513,400 bytes, SHA-256
  `8f86d879…a387`, PCSX2 CRC `C71A6146`), the same 8 IRX modules and the same
  `IOPRP270.IMG`. There is one program to recompile, whichever disc we use.
  The discs differ only in data (FMV languages, file placement) and in
  `SYSTEM.CNF` `VMODE` / a 1-byte `DISK.TXT` (`4` NTSC-U, `1` PAL).
- ELF first look: one PT_LOAD segment at `0x00100000`, 2.85 MB in memory,
  section headers present but **no symbol table** (stripped), **10 VU
  microcode overlays embedded in the ELF** (`.DVP.overlay.*`, `.vutext`). No
  other executables on either disc besides IOP modules. (Deeper analysis in
  Phase 1.)
- `scripts/extract_disc.py` reads ISO and CHD v5 directly in pure Python
  (no 7z/xorriso/chdman needed). Its output is deterministic and checked
  against an independent oracle (the CRC PCSX2 logs for this game).
- **Blocking for Phase 2:** WSL2 is not installed. C: has only 12.6 GB free,
  so the distro should go on D:. Commands are below.

## 1. Environment

| Item | Found |
|---|---|
| OS | Windows 11 Pro 10.0.26200 x64 → toolchain will live in WSL2 |
| CPU / RAM | Ryzen 5 5600H (6C/12T, AVX2) / 28 GB |
| GPU | RTX 3050 Laptop (Vulkan 1.4, driver 610.88) + Radeon iGPU |
| Free disk | C: 12.6 GB (**tight**), D: 86.9 GB, E: 437 GB |
| Python | 3.14.0 (stdlib has `lzma`, `zlib`, `compression.zstd`) |
| git | 2.55.0 (identity configured) |
| CMake | present, `D:\Programs\CMake` |
| WSL | **not installed** (firmware virtualization on, hypervisor present) |
| Compilers | none on Windows (no gcc / clang / MSVC); ninja, make also missing |
| 7z / xorriso / chdman | none; not needed (see D0.1 in NOTES.md) |
| qemu | none; will come from apt inside WSL |
| PCSX2 | 2.6.3 portable at `Harry Potter and the Philosopher's Stone/Engine/pcsx2-qt.exe`, Vulkan renderer |

## 2. Disc images found

Both sit in the repo root and are git-ignored (verified with
`git status --ignored`). Neither has been modified.

| | NTSC-U | PAL |
|---|---|---|
| File | `Harry Potter and The Sorceror's Stone.iso` | `Harry Potter and the Philosopher's Stone/Game/HarryPotter.chd` |
| Serial / version | SLUS-20826 v1.01 | SLES-52055 v1.01 |
| `SYSTEM.CNF` | `BOOT2 = cdrom0:\SLUS_208.26;1`, `VER = 1.01`, `VMODE = NTSC` | `BOOT2 = cdrom0:\SLES_520.55;1`, `VER = 1.01`, `VMODE = PAL` |
| Container | plain ISO, 2,594,963,456 bytes | CHD v5, 2,898,300,932 bytes; lzma/zlib/huff/flac, 4 KiB hunks |
| Sectors in image | 1,267,072 | 2,294,000 (4.70 GB logical) |
| ISO9660 volume | 1,266,912 sectors, created 2003-11-09 | 1,694,976 sectors, created 2004-02-07 |
| Files / dirs | 94 / 5, 1,693 MiB | 108 / 5, 3,310 MiB |
| Past volume end | 160 sectors | 599,024 sectors, all zero (padded image, not volume-sized) |
| Image hashes | MD5 `2e148fed9892e17ac4e06a38c21e378b`, SHA-1 `07563a366d5a9aa38fbcd6e41254549704d238b0`, CRC32 `3240fc33` | SHA-1 of the decoded image, from the CHD header: `410d4ed6b6f1768d3194ba3e7acdfcfbe1a64432` (full-decode check in §6) |

The PCSX2 log in the bundle reports `SLES-52055, Version 1.01, CRC C71A6146`
for the CHD. The extractor independently computes `C71A6146` for the boot ELF
from **both** discs, which cross-checks the CHD decoder, the ISO9660 walker
and the ELF extraction.

## 3. Boot ELF (identical on both discs)

| Field | Value |
|---|---|
| Size | 2,513,400 bytes |
| SHA-256 | `8f86d8793398c593c4e33c1b6c584441fc03dec3bff801380f97527e9071a387` |
| Machine / flags | MIPS (8), `e_flags 0x20924001` = MIPS III, mach R5900, EABI64, noreorder |
| Entry point | `0x002A2FA8` |
| Program headers | 1 × LOAD, file off 0x1000 → vaddr `0x00100000`, filesz 2,487,672, memsz 2,847,532, RWX |
| Sections (23) | `.text .vutext .reginfo .DVP.ovlytab .DVP.ovlystrtab` 10 × `.DVP.overlay..0x{0,800,…,3800}.*` `.data .vudata .rodata .sbss .bss .vubss .shstrtab` |
| Symbols | **none**: no `.symtab`/`.strtab`, no debug sections |

What this means for Phase 1 (preliminary, not yet measured):

- A single static image at 1 MB with no dynamic loader is the best case for
  static recompilation. Phase 1 still has to rule out code in data files
  (e.g. inside `.HOG`), jump tables, and self-modifying code.
- No symbols, so function boundaries must be recovered by analysis (call
  targets, prologues, `jr ra`). The function count is still unknown.
- VU microcode is statically present in the ELF (`.DVP.overlay` sections
  are what the SDK's `dvp-as` emits). That lets us inventory every VU program
  offline. Phase 1 will check whether the game uses VU1 for transform
  (likely) and VU0 macro mode.

## 4. Disc layout

Same directory tree on both discs; only placement and FMV count differ.
Full listings, sorted by LBA, are in `private/extract/<SERIAL>/layout.tsv`.

| Directory | NTSC-U files / MiB | PAL files / MiB | Contents |
|---|---|---|---|
| `/` | 12 / 2.9 | 12 / 2.9 | `SYSTEM.CNF`, boot ELF, 8 × IRX, `IOPRP270.IMG`, `DISK.TXT` |
| `/CUT` | 42 / 225 | 42 / 739 | cutscene `.HOG` archives (same names; PAL 3.3× larger, presumably multi-language audio) |
| `/MOVIES` | 7 / 156 | 21 / 539 | `.PSS` FMV (MPEG-2 + audio); PAL adds per-language `EA_*` / `Q_*` clips |
| `/RESOURCE` | 11 / 547 | 11 / 668 | level / global `.HOG` (`HOGDAY` 226 MB and `HOGNIGHT` 242 MB are the largest) |
| `/RESOURCE/DAY` | 11 / 496 | 11 / 881 | per-day `.HOG` |
| `/RESOURCE/NIGHT` | 11 / 266 | 11 / 480 | per-night `.HOG` |

By type (NTSC-U): `.HOG` 75 files / 1,534 MiB, `.PSS` 7 / 156 MiB, the rest
< 3 MiB. A recompiled runtime only needs to serve these files (or raw sectors)
to the game; it does not need to understand the `.HOG` format.

Placement detail: on NTSC-U the boot files sit at LBA 400,000+ with
cutscenes at the outer edge. On PAL the boot files are at LBA 391+. Since
one ELF runs with both layouts, it cannot hard-code LBAs. It must find files
through the filesystem (e.g. `sceCdSearchFile`) or through tables inside the
data files. Phase 1 will confirm which, because that decides whether the
runtime can serve plain files or must emulate sector reads.

### IOP side

| File | Bytes | Likely role (to confirm in Phase 1) |
|---|---|---|
| `IOPRP270.IMG` | 249,121 | IOP reboot image (ROMDIR, SDK 2.7.0): LOADCORE, SIFCMD, SIFMAN, THREADMAN, IOMAN, MODLOAD, FILEIO, CDVDMAN, CDVDFSV, LOADFILE, TIMEMANI, ROMDRV, EESYNC, SYSCLIB, STDIO |
| `SIO2MAN.IRX` / `PADMAN.IRX` / `MTAPMAN.IRX` | 6,641 / 43,813 / 11,013 | controller and multitap |
| `MCMAN.IRX` / `MCSERV.IRX` | 95,877 / 7,417 | memory card |
| `LIBSD.IRX` / `SDRDRV.IRX` | 28,557 / 8,065 | SPU2 sound library and its EE RPC server |
| `STREAM.IRX` | 122,624 | streaming (audio/data), probably game- or EA-specific; the largest IRX and the most work to HLE |

In a static-recompilation design none of these run. Their EE-facing RPC
interfaces get reimplemented natively in the runtime (HLE). This list is the
first draft of that work.

## 5. Things in the PCSX2 bundle that matter later

- **Patches are on**: `Engine/gamesettings/SLES-52055_C71A6146.ini` enables
  "Widescreen 16:9" and "50 FPS" patches. Phase 1 profiling must turn them
  off (or use a separate clean profile), because they change the workload.
- The bundle contains a BIOS (`Engine/bios/SCPH-50004_BIOS_V9_EUR_190.*`).
  I have not opened, hashed or copied it. It is git-ignored. Under the
  project rules, PCSX2 profiling must use a BIOS dumped from your own
  console. Please confirm (question 3 below).
- Texture dumps/replacements, memcards and a quicksave exist; not touched.

## 6. Extractor: `scripts/extract_disc.py`

```bash
python -I scripts/extract_disc.py IMAGE [--out DIR] [--force]
                                        [--extract GLOB]... [--all]
                                        [--hash] [--verify]
```

- Input: `.iso`, or `.chd` (v5, DVD; no parent CHDs, no CD CHDs).
- Default output: `private/extract/<SERIAL>/`
  - `manifest.json`: image info (incl. CHD codec statistics), volume
    descriptor, `SYSTEM.CNF` raw + parsed, boot ELF (SHA-256, PCSX2 CRC, ELF
    header / program headers / section names), IOP files with SHA-256,
    per-extension statistics, warnings.
  - `layout.tsv`: every file and directory with LBA, sectors, bytes and date.
  - `files/`: `SYSTEM.CNF`, the boot ELF, all `.IRX`, `IOPRP*.IMG`; more via
    `--extract` (glob) or `--all`.
- Reproducible: running twice gives byte-identical `manifest.json` and
  `layout.tsv` (checked).
- CHD support is a pure-Python port of libchdr's v5 map decoder, Huffman
  codec and FLAC hunk handling, plus stdlib lzma/zlib/zstd. Checks: map
  CRC16, per-hunk CRC16, and `--verify` (full decode compared against the
  SHA-1 in the CHD header).
  - Hunk codec usage in this CHD: huff 479,895; duplicate 308,303; lzma
    222,406; zlib 74,298; uncompressed 62,080; flac 18. The FLAC hunks
    include one in the volume-descriptor area, so FLAC support is required,
    not optional.
  - Full verify (`--verify`, decode all 1,147,000 hunks and compare SHA-1):
    **ok**, SHA-1 `410d4ed6…4432` matches the CHD header. Took 19.5 min in
    pure Python on this laptop.
- Edge cases met on these discs and handled:
  - The PAL disc sets the ISO9660 multi-extent bit (0x80) on 99 file records
    that have no continuation record (a mastering quirk). Real multi-extent
    files (continuation with the same name) are merged, and quirk records
    are treated as single-extent.
  - Image sizes beyond the ISO volume (padding) are reported, not treated as
    errors.
  - Dual-layer DVD9 is detected and warned about, not walked. These discs
    are single-layer.
- Speed: ~2 s for listing + ELF/IRX extraction from either image. A full
  CHD decode (`--verify`/`--hash`) is slow in pure Python, mostly in the
  Huffman codec (timing in the verify line above). For
  bulk CHD→ISO conversion `chdman extractdvd` is the faster tool once WSL is
  in place (`apt install mame-tools`).

## 7. What I need from you

1. **Canonical disc.** The code is identical, so this only affects data and
   video mode. My recommendation: **NTSC-U SLUS-20826 as the primary dev
   target** (60 Hz like the handheld's panel, so no 50→60 Hz judder; half the
   data size; plain ISO, so no decode step), with PAL SLES-52055 as a free
   second dataset for regression checks. Pick PAL if you want the European
   release or its languages.
2. **Move the images into `./private/`?** Proposed: `private/disc/SLUS-20826.iso`
   and `private/pcsx2/` (the whole bundle folder). Both are already ignored
   where they are; moving is only tidiness. PCSX2 would need the CHD path
   re-added in its game list.
3. **BIOS:** confirm the SCPH-50004 BIOS in the bundle was dumped from your
   own console, or point me at your own dump. PCSX2 profiling in Phase 1
   needs one.
4. **Install WSL2 + Ubuntu on D:** (needed from Phase 2; Phase 1 can mostly
   run with the Windows Python). In an **admin** PowerShell:

   ```powershell
   wsl --install --no-distribution
   ```

   Reboot. Then, in a normal PowerShell:

   ```powershell
   wsl --install -d Ubuntu-24.04 --location D:\WSL\Ubuntu-24.04
   ```

   If your WSL build rejects `--location`, install without it and then
   move the distro: `wsl --manage Ubuntu-24.04 --move D:\WSL\Ubuntu-24.04`.
   Create the Linux user when prompted. I will script the apt packages
   (cross gcc, qemu-user, SDL2, cmake/ninja, mame-tools) in Phase 2.
5. **Optional:** check the NTSC ISO hashes in §2 against redump.org
   (it only serves plain HTTP, which my fetch tool can't reach).

Phase 1 will start with a measurement plan for your approval (static ELF
analysis first, then PCSX2 profiling), as the brief asks.
