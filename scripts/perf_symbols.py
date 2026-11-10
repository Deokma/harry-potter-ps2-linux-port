#!/usr/bin/env python3
"""Sums a perf report's raw addresses per function, for a binary perf itself cannot symbolise (the runner on /mnt/d).

  perf report -i perf.data --stdio --sort dso,symbol | python3 scripts/perf_symbols.py BINARY [TOP]

Reads `nm -C --defined-only BINARY`; lines of other objects (libc, vdso) are summed per object.
"""
import bisect
import collections
import re
import subprocess
import sys

binary = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
syms = []
for line in subprocess.run(["nm", "-C", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(None, 2)
    if len(parts) == 3 and parts[1] in "TtWw":
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
starts = [s[0] for s in syms]
name = binary.rsplit("/", 1)[-1]
total = collections.Counter()
for line in sys.stdin:
    m = re.match(r"\s*([\d.]+)%\s+(\S+)\s+\[.\]\s+(\S+)", line)
    if not m:
        continue
    pct, dso, sym = float(m.group(1)), m.group(2), m.group(3)
    if dso == name and sym.startswith("0x"):
        i = bisect.bisect_right(starts, int(sym, 16)) - 1
        total[syms[i][1][:110] if i >= 0 else sym] += pct
    else:
        total[f"[{dso}]"] += pct
for key, pct in total.most_common(top):
    print(f"{pct:6.2f}%  {key}")
