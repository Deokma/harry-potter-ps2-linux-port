#!/usr/bin/env python3
"""Diff two EE RAM images region by region.

Each image is a 32 MB eeMemory.bin, a PCSX2 savestate (.p2s, its
eeMemory.bin is used) or a replay-harness state (.in / .out: header,
R5900Context, RAM, scratchpad). Regions: the EE kernel area, each
allocated section of the ELF, the heap in 1 MB slices and the main stack.
Masked ranges (known-volatile: timers, sound buffers...) are left out and
listed separately.

Usage:
    python scripts/diff_ram_regions.py A B ELF [--mask START:END ...]
        [--json OUT.json]
"""

import argparse
import json
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_elf import parse_elf

RAM = 32 << 20
HEAP_END = 0x01FF0000   # main stack base (64 KB stack at the top of RAM)


def load_ram(path):
    path = Path(path)
    if path.suffix == ".p2s":
        return zipfile.ZipFile(path).read("eeMemory.bin")
    data = path.read_bytes()
    if data[:4] == b"PNSP":
        ctx_size = struct.unpack_from("<I", data, 8)[0]
        return data[16 + ctx_size:16 + ctx_size + RAM]
    if len(data) != RAM:
        raise SystemExit(f"{path}: not a 32 MB RAM image")
    return data


def regions(elf):
    out = [("kernel area", 0x0, 0x100000)]
    end = 0x100000
    for s in sorted((s for s in elf["sections"] if s["flags"] & 2 and s["size"]),
                    key=lambda s: s["addr"]):
        out.append((f"ELF {s['name']}", s["addr"], s["addr"] + s["size"]))
        end = max(end, s["addr"] + s["size"])
    start = (end + 0xF) & ~0xF
    while start < HEAP_END:
        stop = min((start | 0xFFFFF) + 1, HEAP_END)
        out.append((f"heap {start:#09x}", start, stop))
        start = stop
    out.append(("main stack", HEAP_END, RAM))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("elf", type=Path)
    ap.add_argument("--mask", action="append", default=[],
                    help="START:END (hex), e.g. 0x328490:0x3284a0")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)

    a, b = load_ram(args.a), load_ram(args.b)
    masks = [tuple(int(v, 16) for v in m.split(":")) for m in args.mask]
    masked = lambda i: any(lo <= i < hi for lo, hi in masks)

    rows, masked_diff = [], 0
    for name, lo, hi in regions(parse_elf(args.elf.read_bytes())):
        diff = first = 0
        for i in range(lo, hi):
            if a[i] != b[i]:
                if masked(i):
                    masked_diff += 1
                    continue
                if not diff:
                    first = i
                diff += 1
        if diff:
            rows.append({"region": name, "start": lo, "end": hi, "bytes": diff, "first": first})
    total = sum(r["bytes"] for r in rows)
    print(f"{total} bytes differ outside masks ({masked_diff} inside masks), "
          f"{len(rows)} regions:")
    for r in rows:
        print(f"  {r['region']:24s} {r['bytes']:9d} bytes, first at {r['first']:#09x}")
    if args.json:
        args.json.write_text(json.dumps({"rows": rows, "masked": masked_diff}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
