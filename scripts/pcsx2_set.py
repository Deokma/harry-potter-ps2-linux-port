#!/usr/bin/env python3
"""Set individual keys in the clean PCSX2 profile's PCSX2.ini.

Close PCSX2 first: it rewrites its ini on exit.

Usage:
    python scripts/pcsx2_set.py SECTION.KEY=VALUE... [--ini PATH]
    e.g. python scripts/pcsx2_set.py EmuCore.CdvdVerboseReads=true
         python scripts/pcsx2_set.py "EmuCore/Speedhacks.EECycleRate=0"
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_pcsx2_profile import apply_overrides


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("settings", nargs="+")
    ap.add_argument("--ini", type=Path,
                    default=Path("D:/ps2port-data/pcsx2-clean/inis/PCSX2.ini"))
    args = ap.parse_args(argv)
    table = {}
    for s in args.settings:
        if "=" not in s or "." not in s.split("=", 1)[0]:
            sys.exit(f"expected SECTION.KEY=VALUE, got {s!r}")
        path, value = s.split("=", 1)
        section, key = path.rsplit(".", 1)
        table.setdefault(section, {})[key] = value
    lines = args.ini.read_text(encoding="utf-8").splitlines()
    args.ini.write_text(apply_overrides(lines, table), encoding="utf-8",
                        newline="\r\n")
    for section, keys in table.items():
        for k, v in keys.items():
            print(f"[{section}] {k} = {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
