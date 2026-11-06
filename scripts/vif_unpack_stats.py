#!/usr/bin/env python3
"""What UNPACKs a flattened VIF1 stream (scripts/vu1_extract.py) asks for: python -I scripts/vif_unpack_stats.py stream.bin

Prints vectors by format, write mask, mode and cycle shape. Numbers only.
"""
import collections
import struct
import sys

SIZES = {(0, 0): 4, (0, 1): 2, (0, 2): 1, (1, 0): 8, (1, 1): 4, (1, 2): 2, (2, 0): 12, (2, 1): 6, (2, 2): 3,
         (3, 0): 16, (3, 1): 8, (3, 2): 4, (3, 3): 2}


def main():
    data = open(sys.argv[1], "rb").read()
    pos = 0
    cycle = mode = mask = 0
    hist = collections.Counter()
    masks = collections.Counter()
    while pos + 4 <= len(data):
        cmd = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        op = cmd >> 24 & 0x7F
        imm, num = cmd & 0xFFFF, cmd >> 16 & 0xFF
        if op == 0x01:
            cycle = imm
        elif op == 0x05:
            mode = imm & 3
        elif op == 0x20:
            mask = struct.unpack_from("<I", data, pos)[0]
            pos += 4
        elif op in (0x30, 0x31):
            pos += 16
        elif op == 0x4A:
            pos += (num or 256) * 8
        elif op in (0x50, 0x51):
            pos += (imm or 65536) * 16
        elif op & 0x60 == 0x60:
            vn, vl = op >> 2 & 3, op & 3
            count = num or 256
            cl, wl = cycle & 0xFF, cycle >> 8 & 0xFF
            cl, wl = cl or 1, wl or 1
            source = count
            if cl < wl:
                source = count // wl * cl + min(count % wl, cl)
            size = (source * SIZES[(vn, vl)] + 3) & ~3
            pos += size
            enabled = bool(op & 0x10)
            key = (f"V{vn + 1}-{(32, 16, 8, 5)[vl]}", "masked" if enabled else "plain", f"mode{mode}", f"cl{cl}wl{wl}",
                   "unsigned" if imm & 0x4000 else "signed")
            hist[key] += count
            if enabled:
                masks[f"{mask:#010x}"] += count
    total = sum(hist.values())
    print("vectors", total)
    for key, n in hist.most_common(16):
        print(f"{n:8d} {100 * n / total:5.1f}%  " + " ".join(key))
    print("masks:", " ".join(f"{k}:{v}" for k, v in masks.most_common(8)))


if __name__ == "__main__":
    main()
