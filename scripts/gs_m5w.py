#!/usr/bin/env python3
"""GL work per game frame from a runner log with PS2PORT_M5_STATS=1 ([m5] game frames/s, [m5w] runs, vertices, passes per s).

    python3 -I scripts/gs_m5w.py LOG [FROM_SECONDS] [TO_SECONDS]

Sums the 5-s windows with FROM <= t < TO (default 240 to the end) and prints runs, draw calls (passes), vertices and execute ms
per game frame.
"""
import re
import sys


def main():
    log = sys.argv[1]
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 240.0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    frames = {}
    work = {}
    for line in open(log, "rb"):
        line = line.decode("latin-1")
        m = re.match(r"\[m5\] t=(\d+) .*game frames/s ([\d.]+)", line)
        if m:
            frames[int(m.group(1))] = float(m.group(2))
        m = re.match(r"\[m5w\] t=(\d+) .*runs ([\d.]+) vertices ([\d.]+) passes ([\d.]+).*execute ([\d.]+) ms", line)
        if m:
            work[int(m.group(1))] = [float(m.group(k)) for k in range(2, 6)]
    ts = [t for t in work if lo <= t < hi and t in frames]
    f = sum(frames[t] for t in ts)
    if not ts or f == 0:
        print("no windows")
        return
    s = [sum(work[t][k] for t in ts) for k in range(4)]
    print("%d windows (t %d-%d), %.1f game frames/s; per game frame: runs %.0f, draw calls %.0f, vertices %.0f, execute %.1f ms" %
          (len(ts), min(ts), max(ts), f / len(ts), s[0] / f, s[2] / f, s[1] / f, s[3] / f))


if __name__ == "__main__":
    main()
