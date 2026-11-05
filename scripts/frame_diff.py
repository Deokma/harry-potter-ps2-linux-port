#!/usr/bin/env python3
"""Check a replayed frame against the same frame in PCSX2, region by region.

Three EE RAM images (see diff_ram_regions.py for the formats):
    START   the state both started from (replay .in or savestate)
    REPLAY  the replay's result (.out)
    PCSX2   a savestate of the same frame in PCSX2

Works on 32-bit words. For each word:
    same         both changed it, to the same value
    close / far  both changed it, to different values; as floats, within 0.1 %
                 of each other or not (see --close)
    missed       only PCSX2 changed it
    extra        only the replay changed it

Usage:
    python -I scripts/frame_diff.py START REPLAY PCSX2 ELF [--close F] [--mask S:E ...]
        [--top N] [--json OUT]
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_elf import parse_elf
from diff_ram_regions import load_ram, regions

KINDS = ("same", "close", "far", "missed", "extra")
PAGE = 4096


def float_close(a, b, tolerance):
    x, y = struct.unpack("<ff", struct.pack("<II", a, b))
    if not (math.isfinite(x) and math.isfinite(y)):
        return False
    if x == y:
        return True
    scale = max(abs(x), abs(y))
    # 1e-30 and below: effectively zero for anything the game draws
    return abs(x - y) <= tolerance * scale or scale < 1e-30


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("start")
    ap.add_argument("replay")
    ap.add_argument("pcsx2")
    ap.add_argument("elf", type=Path)
    ap.add_argument("--close", type=float, default=1e-3)
    ap.add_argument("--mask", action="append", default=[], help="START:END (hex)")
    ap.add_argument("--top", type=int, default=30, help="regions listed")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)

    s, r, p = load_ram(args.start), load_ram(args.replay), load_ram(args.pcsx2)
    masks = [tuple(int(v, 16) for v in m.split(":")) for m in args.mask]
    table = regions(parse_elf(args.elf.read_bytes()))
    rows = {name: dict.fromkeys(KINDS, 0) for name, _, _ in table}
    rows["masked"] = dict.fromkeys(KINDS, 0)
    words = {k: [] for k in KINDS}

    def region_of(addr):
        for name, lo, hi in table:
            if lo <= addr < hi:
                return name
        return "kernel area"

    for page in range(0, len(s), PAGE):
        a, b, c = s[page:page + PAGE], r[page:page + PAGE], p[page:page + PAGE]
        if a == b == c:
            continue
        for off in range(0, PAGE, 4):
            x, y, z = a[off:off + 4], b[off:off + 4], c[off:off + 4]
            if x == y == z:
                continue
            ya, za = struct.unpack("<I", y)[0], struct.unpack("<I", z)[0]
            if y == z:
                kind = "same"
            elif y == x:
                kind = "missed"
            elif z == x:
                kind = "extra"
            elif float_close(ya, za, args.close):
                kind = "close"
            else:
                kind = "far"
            addr = page + off
            if any(lo <= addr < hi for lo, hi in masks):
                rows["masked"][kind] += 1
                continue
            rows[region_of(addr)][kind] += 1
            words[kind].append((addr, struct.unpack("<I", x)[0], ya, za))

    total = {k: sum(row[k] for name, row in rows.items() if name != "masked") for k in KINDS}
    real = total["same"] + total["close"] + total["far"] + total["missed"]
    print(f"words: {real} changed by PCSX2; of those {total['same']} same, {total['close']} close, "
          f"{total['far']} far, {total['missed']} missed; replay only: {total['extra']}")
    if sum(rows["masked"].values()):
        print("masked:", rows["masked"])
    listed = [(n, row) for n, row in rows.items() if n != "masked" and sum(row.values())]
    listed.sort(key=lambda item: -(item[1]["far"] + item[1]["missed"] + item[1]["extra"] + item[1]["close"]))
    print(f"{'region':24s}" + "".join(f"{k:>9s}" for k in KINDS))
    for name, row in listed[:args.top]:
        print(f"{name:24s}" + "".join(f"{row[k]:9d}" for k in KINDS))
    if args.json:
        args.json.write_text(json.dumps({"rows": rows, "words": words}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
