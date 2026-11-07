#!/usr/bin/env python3
"""Per-thread profile from PS2PORT_SAMPLE: python3 -I scripts/sample_threads.py SAMPLES BINARY [--nm NM] [--lib DIR] [--top N] [--thread NAME]
       [--callers PATTERN]

SAMPLES holds pairs of program counter and thread id, with SAMPLES.maps and SAMPLES.threads beside it, and SAMPLES.lr (the link register,
aarch64) when the runner wrote it. BINARY is the runner with its symbols (unstripped: the one on the device may be stripped). Generated
guest functions are grouped too: EG_ for eegen's, FUN_ for PS2Recomp's. --callers PATTERN lists, for the samples whose function name
contains PATTERN, the functions their link register points into: for a leaf such as libc's memcpy, who called it.
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess


def symbols(nm, path, dynamic=False):
    args = [nm, "-n", "--defined-only"] + (["-D"] if dynamic else []) + [path]
    out = subprocess.run(args, capture_output=True, text=True).stdout
    table = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in "TtWwVvIi":
            table.append((int(parts[0], 16), parts[2]))
    return [t[0] for t in table], [t[1] for t in table]


def group_of(d):
    if "EG_" in d:
        return "eegen guest code"
    if "FUN_" in d:
        return "PS2Recomp guest code"
    if "eg_vu0" in d or "egVu1" in d or "eg_vu1" in d:
        return "translated VU"
    if "VU1Interpreter" in d or "vu1" in d.lower():
        return "VU interpreter"
    if "IopKernel" in d or "IopEmulator" in d or "IopCpu" in d or "Iop" in d or "iop" in d:
        return "IOP emulator"
    if "Rb_tree" in d:
        return "std::map walks"
    if "GSGles" in d or "GS2" in d or "GS" in d or "2GS" in d:
        return "GS"
    if "PS2Memory" in d or "Ps2Is" in d or "9PS2Memory" in d or "Ps2Fast" in d:
        return "PS2Memory"
    if "EeScheduler" in d or "PS2Runtime" in d:
        return "EE scheduler and runtime"
    if "libc" in d:
        return "libc"
    if "libmali" in d:
        return "libmali"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("binary")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--lib")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--thread")
    ap.add_argument("--callers")
    ap.add_argument("--by-ee", action="store_true", help="split the thread's samples by the EE guest thread running (SAMPLES.ee)")
    args = ap.parse_args()
    data = open(args.samples, "rb").read()
    n = len(data) // 16
    pairs = struct.unpack(f"<{n * 2}Q", data[: n * 16])
    ees = None
    if os.path.exists(args.samples + ".ee"):
        raw = open(args.samples + ".ee", "rb").read()
        if len(raw) >= n * 4:
            ees = struct.unpack(f"<{n}i", raw[: n * 4])
    lrs = None
    if os.path.exists(args.samples + ".lr"):
        raw = open(args.samples + ".lr", "rb").read()
        if len(raw) >= n * 8:
            lrs = struct.unpack(f"<{n}Q", raw[: n * 8])
    names = {}
    for line in open(args.samples + ".threads", errors="replace"):
        p = line.split(None, 1)
        if len(p) == 2:
            names[int(p[0])] = p[1].strip()
    regions = []
    main_base = None
    binary_name = None
    for line in open(args.samples + ".maps", errors="replace"):
        parts = line.split()
        if len(parts) < 6:
            continue
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        regions.append((lo, hi, int(parts[2], 16), parts[5]))
        if main_base is None and os.path.basename(parts[5]).startswith("runner"):
            main_base = lo - int(parts[2], 16)
            binary_name = parts[5]
    regions.sort()
    starts = [r[0] for r in regions]
    main_syms = symbols(args.nm, args.binary)
    libs = {}

    def resolve(pc):
        """(module, function name) for an address"""
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0 or pc >= regions[i][1]:
            return "?", "?"
        lo, hi, off, path = regions[i]
        if path == binary_name:
            j = bisect.bisect_right(main_syms[0], pc - main_base) - 1
            return "runner", (main_syms[1][j] if j >= 0 else "?")
        base = os.path.basename(path)
        name = base
        if args.lib:
            if base not in libs:
                local = os.path.join(args.lib, base)
                libs[base] = symbols(args.nm, local, True) if os.path.exists(local) else ([], [])
            addrs, syms = libs[base]
            j = bisect.bisect_right(addrs, pc - lo + off) - 1
            if j >= 0:
                name = f"{base}:~{syms[j]}"
        return base, name

    per_thread = collections.defaultdict(collections.Counter)
    per_module = collections.defaultdict(collections.Counter)
    callers = collections.defaultdict(collections.Counter)
    by_ee = collections.defaultdict(collections.Counter)
    for k in range(n):
        pc, tid = pairs[k * 2], pairs[k * 2 + 1]
        thread = names.get(tid, str(tid))
        module, name = resolve(pc)
        per_module[thread][module] += 1
        per_thread[thread][name] += 1
        if args.callers and lrs and args.callers in name:
            callers[thread][resolve(lrs[k])[1]] += 1
        if args.by_ee and ees and (not args.thread or args.thread in thread):
            by_ee[ees[k]][name] += 1
    print(n, "samples")
    for thread, counter in sorted(per_thread.items(), key=lambda e: -sum(e[1].values())):
        if args.thread and args.thread not in thread:
            continue
        total = sum(counter.values())
        print(f"--- {thread}: {total} samples ({100 * total / n:.1f}%); by module: " + " ".join(f"{k}:{100 * v / total:.0f}%" for k, v in per_module[thread].most_common(4)))
        eg = sum(v for k, v in counter.items() if k.startswith("_Z") and "EG_" in k or k.startswith("EG_"))
        fun = sum(v for k, v in counter.items() if "FUN_" in k)
        print(f"    guest code: eegen {100 * eg / total:.1f}%, PS2Recomp's {100 * fun / total:.1f}%")
        groups = collections.Counter()
        for name, v in counter.items():
            groups[group_of(name)] += v
        print("    groups: " + "; ".join(f"{k} {100 * v / total:.1f}%" for k, v in groups.most_common()))
        for name, v in counter.most_common(args.top):
            print(f"    {100 * v / total:5.1f}%  {name[:70]}")
        if args.by_ee and by_ee:
            print("    by EE guest thread (0: none, in the scheduler):")
            for ee, c in sorted(by_ee.items(), key=lambda e: -sum(e[1].values())):
                s = sum(c.values())
                tops = ", ".join(f"{name[:40]} {100 * v / s:.0f}%" for name, v in c.most_common(4))
                print(f"      EE thread {ee}: {100 * s / total:5.1f}%  ({tops})")
        if args.callers and callers[thread]:
            hits = sum(callers[thread].values())
            print(f"    called from, for '{args.callers}' ({hits} samples, {100 * hits / total:.1f}% of the thread):")
            for name, v in callers[thread].most_common(args.top):
                print(f"      {100 * v / hits:5.1f}%  {name[:70]}")


if __name__ == "__main__":
    main()
