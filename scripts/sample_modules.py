#!/usr/bin/env python3
"""Sample addresses by module and, where a library exports symbols, by function.

    python3 -I scripts/sample_modules.py SAMPLES MAPS LIBDIR [--nm NM] [--top N]

SAMPLES holds raw 64-bit addresses (GSPLAY_SAMPLE), MAPS the /proc/self/maps saved beside it (SAMPLES.maps). LIBDIR holds copies of
the device's libraries, by file name, for their exported symbols; a stripped library shows only as a module, with symbols as near as its
exports allow, marked "~".
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("maps")
    ap.add_argument("libdir")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--top", type=int, default=16)
    args = ap.parse_args()
    data = open(args.samples, "rb").read()
    samples = struct.unpack(f"<{len(data) // 8}Q", data)
    regions = []
    for line in open(args.maps):
        parts = line.split()
        if len(parts) < 6:
            continue
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        regions.append((lo, hi, int(parts[2], 16), parts[5]))
    regions.sort()
    starts = [r[0] for r in regions]
    symbols = {}

    def load(path):
        name = os.path.basename(path)
        if name in symbols:
            return symbols[name]
        local = os.path.join(args.libdir, name)
        table = []
        if os.path.exists(local):
            out = subprocess.run([args.nm, "-D", "--defined-only", "-n", local], capture_output=True, text=True).stdout
            for line in out.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[1] in "TtWw":
                    table.append((int(parts[0], 16), parts[2]))
        symbols[name] = table
        return table

    modules = collections.Counter()
    functions = collections.Counter()
    for pc in samples:
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0 or pc >= regions[i][1]:
            modules["?"] += 1
            continue
        lo, hi, off, path = regions[i]
        name = os.path.basename(path)
        modules[name] += 1
        table = load(path)
        if table:
            address = pc - lo + off
            j = bisect.bisect_right([t[0] for t in table], address) - 1
            functions[f"{name}: {'~' + table[j][1] if j >= 0 else '?'}"] += 1
    total = len(samples)
    print(total, "samples")
    for name, n in modules.most_common(10):
        print(f"{100 * n / total:6.2f}%  {name}")
    print("by exported symbol:")
    for name, n in functions.most_common(args.top):
        print(f"{100 * n / total:6.2f}%  {name}")


if __name__ == "__main__":
    main()
