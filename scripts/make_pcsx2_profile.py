#!/usr/bin/env python3
"""Create a clean, portable PCSX2 profile for Phase 1 measurements.

Copies the PCSX2 program files from an existing portable install (no BIOS,
no settings, no user data) and writes inis/PCSX2.ini derived from that
install's ini: controller bindings and hotkeys are kept, measurement-
relevant settings are forced to stock values:

  - all patches / widescreen / cheats off, GameIndex fixes on
  - EE cycle rate 100 %, no cycle skip, MTVU off
  - native resolution, OSD performance + GS statistics on
  - savestates, GS dumps/screenshots, memcards, logs under DATA_ROOT
  - no BIOS configured: pick your own dump in Settings > BIOS

Existing memory cards in DATA_ROOT/memcards are never overwritten; if none
exist, copies of the source install's cards are placed there.

Usage:
    python scripts/make_pcsx2_profile.py [--source DIR] [--dest DIR]
                                         [--data-root DIR] [--memcards DIR]
"""

import argparse
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Top-level folders of a portable PCSX2 that hold user data, not program files
SKIP_DIRS = {"bios", "cache", "logs", "inis", "gamesettings", "textures",
             "memcards", "sstates", "snaps", "covers", "cheats", "patches",
             "videos", "utils", "inputprofiles", "debuggerlayouts",
             "debuggersettings"}
SKIP_FILES = {"updater.exe"}


def overrides(data_root):
    d = str(data_root).replace("\\", "/")
    return {
        "UI": {"StartFullscreen": "false", "HideMainWindowWhenRunning":
               "false", "SetupWizardIncomplete": "false",
               "ConfirmShutdown": "false", "StartPaused": "false",
               "PauseOnFocusLoss": "false"},
        "Folders": {"Bios": "bios", "Snapshots": f"{d}/snapshots",
                    "Savestates": f"{d}/savestates",
                    "MemoryCards": f"{d}/memcards", "Logs": f"{d}/logs",
                    "Videos": f"{d}/videos"},
        "Filenames": {"BIOS": ""},
        "EmuCore": {"EnablePatches": "false", "EnableCheats": "false",
                    "EnableWideScreenPatches": "false",
                    "EnableNoInterlacingPatches": "false",
                    "EnableGameFixes": "true", "EnableFastBoot": "true",
                    "EnableRecordingTools": "false",
                    "SaveStateOnShutdown": "false",
                    "BackupSavestate": "false",
                    "SavestateCompressionType": "2"},
        "EmuCore/Speedhacks": {"EECycleRate": "0", "EECycleSkip": "0",
                               "fastCDVD": "false", "IntcStat": "true",
                               "WaitLoop": "true", "vuFlagHack": "true",
                               "vuThread": "false", "vu1Instant": "true"},
        "EmuCore/GS": {"upscale_multiplier": "1", "OsdShowSpeed": "true",
                       "OsdShowFPS": "true", "OsdShowVPS": "true",
                       "OsdShowCPU": "true", "OsdShowGPU": "true",
                       "OsdShowResolution": "true", "OsdShowGSStats": "true",
                       "OsdShowFrameTimes": "true", "OsdShowSettings": "true",
                       "OsdshowPatches": "true", "GSDumpCompression": "2",
                       "LoadTextureReplacements": "false",
                       "DumpReplaceableTextures": "false"},
        "Logging": {"EnableFileLogging": "true"},
    }


def apply_overrides(lines, table):
    """Line-based INI edit; keeps duplicate keys (PCSX2 uses them)."""
    out, section, seen = [], None, set()

    def flush_missing(sec):
        for key, val in table.get(sec, {}).items():
            if (sec, key) not in seen:
                out.append(f"{key} = {val}")
                seen.add((sec, key))

    for raw in lines:
        line = raw.rstrip("\r\n")
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if section is not None:
                while out and out[-1] == "":
                    out.pop()
                flush_missing(section)
                out += ["", ""]
            section = stripped[1:-1]
            out.append(line)
            continue
        if "=" in line and section in table:
            key = line.split("=", 1)[0].strip()
            if key in table[section]:
                if (section, key) in seen:
                    continue  # drop duplicates of overridden keys
                out.append(f"{key} = {table[section][key]}")
                seen.add((section, key))
                continue
        out.append(line)
    if section is not None:
        flush_missing(section)
    for sec, keys in table.items():
        if not any(s == sec for s, _ in seen):
            out += ["", "", f"[{sec}]"] + [f"{k} = {v}" for k, v in
                                          keys.items()]
    return "\n".join(out).rstrip("\n") + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--source", type=Path,
                    default=REPO_ROOT / "private" / "pcsx2" / "Engine")
    ap.add_argument("--dest", type=Path,
                    default=Path("D:/ps2port-data/pcsx2-clean"))
    ap.add_argument("--data-root", type=Path,
                    default=Path("D:/ps2port-data"))
    ap.add_argument("--memcards", type=Path,
                    default=REPO_ROOT / "private" / "pcsx2" / "Saves" /
                    "memcards")
    args = ap.parse_args(argv)

    if not (args.source / "pcsx2-qt.exe").exists():
        sys.exit(f"no pcsx2-qt.exe in {args.source}")
    copied = 0
    for src in args.source.rglob("*"):
        rel = src.relative_to(args.source)
        if rel.parts[0].lower() in SKIP_DIRS or src.is_dir():
            continue
        if len(rel.parts) == 1 and rel.name.lower() in SKIP_FILES:
            continue
        dst = args.dest / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists() or dst.stat().st_size != src.stat().st_size:
            shutil.copy2(src, dst)
            copied += 1
    (args.dest / "portable.txt").touch()
    (args.dest / "bios").mkdir(exist_ok=True)
    for sub in ("snapshots", "savestates", "memcards", "logs", "videos",
                "reports"):
        (args.data_root / sub).mkdir(parents=True, exist_ok=True)

    src_ini = args.source / "inis" / "PCSX2.ini"
    lines = src_ini.read_text(encoding="utf-8").splitlines() \
        if src_ini.exists() else []
    ini = apply_overrides(lines, overrides(args.data_root))
    (args.dest / "inis").mkdir(exist_ok=True)
    (args.dest / "inis" / "PCSX2.ini").write_text(ini, encoding="utf-8",
                                                  newline="\r\n")

    cards = 0
    if args.memcards.is_dir() and \
            not any((args.data_root / "memcards").glob("*.ps2")):
        for card in args.memcards.glob("*.ps2"):
            shutil.copy2(card, args.data_root / "memcards" / card.name)
            cards += 1
    print(f"profile : {args.dest} ({copied} files copied)")
    print(f"ini     : {args.dest / 'inis' / 'PCSX2.ini'}")
    print(f"data    : {args.data_root} (memcards copied: {cards})")
    print("BIOS    : not configured; set it in Settings > BIOS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
