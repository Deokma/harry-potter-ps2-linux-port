#!/usr/bin/env python3
"""Names for the host call stacks a run printed with PS2PORT_STACKS (runner/display.cpp): '[stack] map' gives the runner's load
address, each '[stack] tid N: ADDR ...' line one thread's stack, innermost first.

  stack_symbols.py LOG BINARY [--nm NM] [--depth N]     BINARY: the runner with its symbols (unstripped)
"""
import bisect
import subprocess
import sys


def main():
    args = sys.argv[1:]
    nm = "nm"
    depth = 14
    if "--nm" in args:
        i = args.index("--nm")
        nm = args[i + 1]
        del args[i:i + 2]
    if "--depth" in args:
        i = args.index("--depth")
        depth = int(args[i + 1])
        del args[i:i + 2]
    log, binary = args[0], args[1]
    out = subprocess.run([nm, "-n", "-C", "--defined-only", binary], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    base = None
    libc = None  # (load address, path): its functions named from its dynamic symbols
    lib_addrs, lib_names = [], []
    for line in open(log, errors="replace"):
        if line.startswith("[stack] libc ") and libc is None:
            f = line.split()
            libc = (int(f[2].split("-")[0], 16), f[-1])
            dyn = subprocess.run([nm, "-D", "-n", "--defined-only", libc[1]], capture_output=True, text=True).stdout
            for row in dyn.splitlines():
                p = row.split(" ", 2)
                if len(p) == 3 and p[1] in "tTwWi":
                    lib_addrs.append(int(p[0], 16))
                    lib_names.append("libc:" + p[2])
    for line in open(log, errors="replace"):
        if line.startswith("[stack] map "):
            base = int(line.split()[2].split("-")[0], 16)
        elif line.startswith("[stack] tid ") and base is not None:
            head, _, rest = line.partition(":")
            print(head)
            words = [w for w in rest.split() if w.startswith("0x")]  # two threads' lines can interleave: keep addresses only
            for word in words[1:depth + 1]:  # the first frame is the signal handler
                a = int(word, 16) - base
                i = bisect.bisect_right(addrs, a) - 1
                name = names[i] if 0 <= i < len(names) and a - addrs[i] < 1 << 20 else "?"
                if name == "?" and libc and lib_addrs:
                    la = int(word, 16) - libc[0]
                    j = bisect.bisect_right(lib_addrs, la) - 1
                    if 0 <= j and la - lib_addrs[j] < 1 << 16:
                        name = lib_names[j]
                print(f"    {name[:150]}")


main()
