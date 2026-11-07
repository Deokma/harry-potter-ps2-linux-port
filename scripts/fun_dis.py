#!/usr/bin/env python3
"""MIPS listing of a game function, from the comments in PS2Recomp's generated code:
    python -I scripts/fun_dis.py ADDR [ADDR...] [--from A] [--to B]

Each ADDR may be anywhere in a function. Reads private/third_party/trial/out-b. The output is game code: don't commit it.
"""
import bisect
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent / "private/third_party/trial/out-b"
files = {int(p.name[4:12], 16): p for p in root.glob("FUN_*.cpp")}
starts = sorted(files)
line = re.compile(r"//\s*0x([0-9a-f]+):\s*0x[0-9a-f]+\s+(.*)")

args = sys.argv[1:]
low, high = 0, 1 << 32
if "--from" in args:
    i = args.index("--from")
    low = int(args[i + 1], 16)
    del args[i:i + 2]
if "--to" in args:
    i = args.index("--to")
    high = int(args[i + 1], 16)
    del args[i:i + 2]
for arg in args:
    a = int(arg, 16)
    s = starts[bisect.bisect_right(starts, a) - 1]
    print(f"--- FUN_{s:08x}")
    for text in files[s].read_text(errors="replace").splitlines():
        m = line.search(text)
        if m and low <= int(m.group(1), 16) <= high:
            print(f"{m.group(1)}  {re.sub(r' +', ' ', m.group(2))}"[:80])
