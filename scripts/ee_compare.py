#!/usr/bin/env python3
"""Same-work check between PC runs (EE worker): per game frame, GIF packets and kicks from `[stats]`, beside `[eework]`, in a window.
    python3 -I scripts/ee_compare.py FROM TO LOG...
Also the time the game reached play (mode 5, `[watch]` of 0x38c194). If the per-frame GIF numbers match, the runs drew the same.
"""
import re
import sys

lo, hi = float(sys.argv[1]), float(sys.argv[2])
ew = re.compile(r"^\[eework\] s=([\d.]+) frames=(\d+) cpu_ms=([\d.]+) lookups=(\d+) sched=(\d+)(?: instr_m=([\d.]+))?")
st = re.compile(r"^\[stats\] .*?gifpkts=(\d+) gifbytes=(\d+) kicks=(\d+)")
for log in sys.argv[3:]:
    rows, last, mode5 = [], None, None
    with open(log, "rb") as f:
        for raw in f:
            line = raw.decode("latin-1")
            m = ew.match(line)
            if m:
                last = [float(x) if x is not None else 0.0 for x in m.groups()]
                continue
            m = st.match(line)
            if m and last:
                rows.append(last + [float(x) for x in m.groups()])
                last = None
            if mode5 is None and line.startswith("[watch]"):
                p = line.split()
                if len(p) >= 4 and p[3] == "5":
                    mode5 = p[1]
    inside = [r for r in rows if lo <= r[0] <= hi]
    if len(inside) < 2:
        print(f"{log}: too few lines")
        continue
    a, b = inside[0], inside[-1]
    fr = b[1] - a[1]
    print(f"{log.split('/')[-1]}: mode 5 at {mode5} s; frames {fr:.0f}; per frame: cpu {(b[2] - a[2]) / fr:.2f} ms, lookups {(b[3] - a[3]) / fr:.0f}, "
          f"sched {(b[4] - a[4]) / fr:.0f}, instr {(b[5] - a[5]) / fr:.2f} M, gif packets {(b[6] - a[6]) / fr:.0f}, gif KB {(b[7] - a[7]) / fr / 1024:.0f}, kicks {(b[8] - a[8]) / fr:.0f}")
