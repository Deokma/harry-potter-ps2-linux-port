#!/usr/bin/env python3
"""Averages the [iopc] lines (PS2PORT_IOP_COUNTERS=1) of a runner log over a stretch of host time.

usage: python3 -I scripts/iop_counters.py LOG [FROM_S] [TO_S]
Prints the mean of every "name N" pair in the summary, mixer and hand-off lines, and the mean instructions a second by
module, thread, hot 256-byte stretch and import, over the reports whose t= falls in [FROM_S, TO_S].
"""
import re
import sys
from collections import defaultdict


def main():
    log = sys.argv[1]
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    num = re.compile(r"([A-Za-z][A-Za-z /\-]*?)\s+([0-9]+(?:\.[0-9]+)?)")
    pairs = re.compile(r"(\S+) ([0-9]+)")
    t = None
    scalars = defaultdict(list)
    tables = defaultdict(lambda: defaultdict(float))
    reports = 0
    with open(log, "rb") as f:
        for raw in f:
            if not raw.startswith(b"[iopc]"):
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            m = re.match(r"\[iopc\] t=([0-9.]+) s: (.*)", line)
            if m:
                t = float(m.group(1))
                if lo <= t <= hi:
                    reports += 1
                    for k, v in num.findall(m.group(2)):
                        scalars["main " + k.strip()].append(float(v))
                    idle = re.search(r"idle ([0-9.]+) %", line)
                    if idle:
                        scalars["main idle %"].append(float(idle.group(1)))
                continue
            if t is None or not (lo <= t <= hi):
                continue
            m = re.match(r"\[iopc\] (modules|hot 256B|threads) instr/s:(.*)", line)
            if m:
                for k, v in pairs.findall(m.group(2)):
                    tables[m.group(1)][k] += float(v)
                continue
            m = re.match(r"\[iopc\] imports/s ([0-9]+):(.*)", line)
            if m:
                scalars["imports total"].append(float(m.group(1)))
                for k, v in pairs.findall(m.group(2)):
                    tables["imports"][k] += float(v)
                continue
            m = re.match(r"\[iopc\] (mixer|hand-off)/s: (.*)", line)
            if m:
                for k, v in num.findall(m.group(2)):
                    scalars[m.group(1) + " " + k.strip()].append(float(v))
    print(f"{reports} reports, t in [{lo:g}, {hi:g}] s")
    for k, vs in scalars.items():
        print(f"  {k}: {sum(vs) / len(vs):.6g}")
    for name, tab in tables.items():
        top = sorted(tab.items(), key=lambda kv: -kv[1])[:10]
        print(f"  {name}: " + ", ".join(f"{k} {v / max(reports, 1):.0f}" for k, v in top))


if __name__ == "__main__":
    main()
