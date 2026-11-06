#!/usr/bin/env python3
"""Where VU1 time goes, from vu1play's pair counts: python -I scripts/vu1_hot.py MICRO.bin COUNTS.bin [--list N]

COUNTS.bin comes from a vu1gen --count build run with VU1PLAY_COUNTS=FILE and one pass (counts add up over passes). Prints pairs
executed, the runs of hot pairs and the mix of operations weighted by how often they run. --list N lists the N hottest runs.
"""
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vu1_stats import lower_name, upper_name  # noqa: E402
from vu1_dis import lower_text, upper_text  # noqa: E402


def main():
    code = Path(sys.argv[1]).read_bytes()
    counts = struct.unpack("<2048Q", Path(sys.argv[2]).read_bytes())
    total = sum(counts)
    print("pairs executed", total)
    runs = []
    pc = 0
    while pc < 2048:
        if counts[pc] == 0:
            pc += 1
            continue
        start = pc
        total_here = 0
        while pc < 2048 and counts[pc]:
            total_here += counts[pc]
            pc += 1
        runs.append((total_here, start, pc))
    runs.sort(reverse=True)
    for here, start, stop in runs[:12]:
        peak = max(counts[start:stop])
        print(f"  {start:#05x}-{stop:#05x}: {100 * here / total:5.1f}%  {stop - start:4d} pairs, hottest runs {peak} times")
    lower = collections.Counter()
    upper = collections.Counter()
    for pc in range(2048):
        if counts[pc]:
            l, u = struct.unpack_from("<II", code, pc * 8)
            lower[lower_name(l, u) if l not in (0, 0x8000033C) else "nop"] += counts[pc]
            upper[upper_name(u)] += counts[pc]
    print("upper:", " ".join(f"{k}:{100 * v / total:.1f}" for k, v in upper.most_common(14)))
    print("lower:", " ".join(f"{k}:{100 * v / total:.1f}" for k, v in lower.most_common(14)))
    if len(sys.argv) > 4:
        for here, start, stop in runs[:int(sys.argv[4])]:
            print(f"--- {start:#05x}-{stop:#05x}")
            for pc in range(start, stop):
                l, u = struct.unpack_from("<II", code, pc * 8)
                print(f"{pc:#05x} {counts[pc]:7d}  {upper_text(u):34s} | {lower_text(l, u, pc)}"[:112])


if __name__ == "__main__":
    main()
