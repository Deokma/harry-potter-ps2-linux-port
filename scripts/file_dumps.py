#!/usr/bin/env python3
"""Move new GS dumps and screenshots into a per-scene folder.

PCSX2 writes GS dumps (*.gs, *.gs.zst, *.gs.xz) and screenshots (*.png,
*.jpg) to its Snapshots folder. After finishing a scene, run this once to
move everything still loose in that folder into <snapshots>/<SCENE>/, so
each scene's captures stay together.

Usage:
    python scripts/file_dumps.py SCENE [--snapshots DIR]
    e.g. python scripts/file_dumps.py S02
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

PATTERNS = ("*.gs", "*.gs.zst", "*.gs.xz", "*.png", "*.jpg", "*.jpeg")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("scene", help="scene id from docs/SAVESTATES.md, "
                                  "e.g. S02, N01, F01, PILOT")
    ap.add_argument("--snapshots", type=Path,
                    default=Path("D:/ps2port-data/snapshots"))
    args = ap.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9_-]{2,16}", args.scene):
        sys.exit("scene id: 2-16 letters/digits, e.g. S02")
    dest = args.snapshots / args.scene.upper()
    dest.mkdir(parents=True, exist_ok=True)
    moved = []
    for pattern in PATTERNS:
        for f in sorted(args.snapshots.glob(pattern)):
            if f.is_file():
                shutil.move(str(f), dest / f.name)
                moved.append(f.name)
    for name in moved:
        print(f"  {name}")
    print(f"{len(moved)} file(s) moved to {dest}")
    if not moved:
        print("nothing to move: did PCSX2 write dumps somewhere else? "
              "(Settings > Folders > Snapshots)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
