#!/usr/bin/env python3
"""EE work per game frame from a PC run's log (PS2PORT_STATS=1 `[eework]` lines, EE worker, docs/PHASE_9_EE.md):
    python3 -I scripts/ee_frames.py LOG FROM TO
Between the first `[eework]` line at or after FROM seconds and the last at or before TO: game frames, the EE thread's CPU ms per
game frame, its user-mode instructions (hardware counter, millions), function-table lookups and scheduler-loop entries (host stack unwinds) per game frame. PC numbers, not device ones.
"""
import re
import sys

log, lo, hi = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
pat = re.compile(r"^\[eework\] s=([\d.]+) frames=(\d+) cpu_ms=([\d.]+) lookups=(\d+) sched=(\d+)(?: instr_m=([\d.]+))?")
rows = []
with open(log, "rb") as f:
    for raw in f:
        m = pat.match(raw.decode("latin-1"))
        if m:
            rows.append(tuple(float(x) if x is not None else 0.0 for x in m.groups()))
inside = [r for r in rows if lo <= r[0] <= hi]
if len(inside) < 2:
    sys.exit(f"ee_frames: fewer than two [eework] lines in {lo}-{hi} s ({len(rows)} in all)")
a, b = inside[0], inside[-1]
frames = b[1] - a[1]
if frames <= 0:
    sys.exit("ee_frames: no game frames in the window")
print(f"window {a[0]:.0f}-{b[0]:.0f} s: game frames {frames:.0f} ({frames / (b[0] - a[0]):.1f}/s); per game frame: "
      f"EE cpu {(b[2] - a[2]) / frames:.2f} ms, lookups {(b[3] - a[3]) / frames:.0f}, scheduler entries {(b[4] - a[4]) / frames:.0f}, "
      f"user-mode instructions {(b[5] - a[5]) / frames:.2f} M")
