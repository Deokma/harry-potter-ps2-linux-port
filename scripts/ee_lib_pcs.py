#!/usr/bin/env python3
"""Hottest offsets inside one shared library for one thread, from a PS2PORT_SAMPLE file (EE worker, for a stripped libc):
    python3 -I scripts/ee_lib_pcs.py SAMPLES THREAD LIBNAME [TOP]
Prints offset (as in the library file, for objdump --start-address) and sample count, binned to 64 bytes.
"""
import collections
import struct
import sys

samples, thread, lib = sys.argv[1], sys.argv[2], sys.argv[3]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 15
data = open(samples, "rb").read()
n = len(data) // 16
pairs = struct.unpack(f"<{n * 2}Q", data[: n * 16])
tids = set()
for line in open(samples + ".threads", errors="replace"):
    p = line.split()
    if len(p) >= 2 and p[1] == thread:
        tids.add(int(p[0]))
maps = []
for line in open(samples + ".maps", errors="replace"):
    p = line.split()
    if len(p) >= 6 and lib in p[5]:
        lo, hi = (int(x, 16) for x in p[0].split("-"))
        maps.append((lo, hi, int(p[2], 16)))
bins = collections.Counter()
total = 0
for k in range(n):
    pc, tid = pairs[2 * k], pairs[2 * k + 1]
    if tid not in tids:
        continue
    total += 1
    for lo, hi, off in maps:
        if lo <= pc < hi:
            bins[(pc - lo + off) & ~63] += 1
print(f"{total} samples of {thread}; in {lib}: {sum(bins.values())}")
for off, c in bins.most_common(top):
    print(f"  0x{off:x} {c}")
