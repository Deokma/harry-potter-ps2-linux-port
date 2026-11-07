#!/usr/bin/env python3
"""Per-core load from a device_panel.sh freq.txt: python3 -I scripts/core_load.py FREQ_TXT [FROM_S TO_S]

Each line: seconds, CPU kHz, cooling state, CPU zone temperature (milli-C), then the cpu0-3 lines of /proc/stat joined by '|'.
Prints, between FROM_S and TO_S, each core's busy share (everything but idle and iowait), the clock and temperature range.
"""
import sys


def parse(line):
    head, _, stat = line.partition("cpu")
    parts = head.split()
    if len(parts) < 4:
        return None
    cores = {}
    for entry in ("cpu" + stat).split("|"):
        f = entry.split()
        if len(f) >= 8 and f[0].startswith("cpu"):
            v = [int(x) for x in f[1:]]
            idle = v[3] + v[4]
            cores[f[0]] = (sum(v), idle)
    return int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]), cores


def main():
    rows = [r for r in (parse(l) for l in open(sys.argv[1])) if r]
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    sel = [r for r in rows if lo <= r[0] <= hi]
    if len(sel) < 2:
        print("not enough samples")
        return
    a, b = sel[0], sel[-1]
    print(f"{a[0]}-{b[0]} s: clock {min(r[1] for r in sel)}-{max(r[1] for r in sel)} kHz, cooling state max {max(r[2] for r in sel)}, "
          f"CPU zone {min(r[3] for r in sel) / 1000:.1f}-{max(r[3] for r in sel) / 1000:.1f} C")
    for core in sorted(b[4]):
        if core not in a[4]:
            continue
        total = b[4][core][0] - a[4][core][0]
        idle = b[4][core][1] - a[4][core][1]
        print(f"  {core}: {100 * (total - idle) / total if total else 0:.0f} % busy")


if __name__ == "__main__":
    main()
