#!/usr/bin/env python3
"""Write a function map CSV for a stripped PS2 EE ELF.

Same format as PS2Recomp's Ghidra exporter (`Name,Start,End,Size`, End
exclusive), so it can be passed as `general.ghidra_output`. Function
starts come from analyze_elf.py: jal targets, code pointers that look like
entries, prologues right after a `jr ra`, and the ELF entry. Each function
ends where the next one starts, or at the end of its code section.

The map describes the game's code, so write it under private/.

Usage:
    python scripts/export_function_map.py ELF OUT.csv
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_elf import analyze_code, parse_elf


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("out", type=Path)
    args = ap.parse_args(argv)
    data = args.elf.read_bytes()
    fmap = analyze_code(parse_elf(data), data)["_function_map"]
    starts, ranges = fmap["starts"], fmap["ranges"]
    rows = []
    for i, s in enumerate(starts):
        lo, hi = next((lo, hi) for lo, hi in ranges if lo <= s < hi)
        nxt = starts[i + 1] if i + 1 < len(starts) else hi
        end = min(nxt, hi)
        rows.append((s, end))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", newline="\n") as f:
        f.write("Name,Start,End,Size\n")
        for s, e in rows:
            f.write(f"FUN_{s:08x},0x{s:08x},0x{e:08x},{e - s}\n")
    sizes = sorted(e - s for s, e in rows)
    print(f"{len(rows)} functions -> {args.out} "
          f"(median {sizes[len(sizes) // 2]} B, max {sizes[-1]} B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
