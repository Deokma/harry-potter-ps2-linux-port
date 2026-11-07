#!/usr/bin/env python3
"""Per-thread profile from PS2PORT_SAMPLE: python3 -I scripts/sample_threads.py SAMPLES BINARY [--nm NM] [--lib DIR] [--top N] [--thread NAME]

SAMPLES holds pairs of program counter and thread id, with SAMPLES.maps and SAMPLES.threads beside it. BINARY is the runner with its symbols
(unstripped: the one on the device may be stripped). Generated guest functions are grouped too: EG_ for eegen's, FUN_ for PS2Recomp's.
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("binary")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--lib")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--thread")
    args = ap.parse_args()
    data = open(args.samples, "rb").read()
    n = len(data) // 16
    pairs = struct.unpack(f"<{n * 2}Q", data[: n * 16])
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
    per_thread = collections.defaultdict(collections.Counter)
    per_module = collections.defaultdict(collections.Counter)
    for k in range(n):
        pc, tid = pairs[k * 2], pairs[k * 2 + 1]
        thread = names.get(tid, str(tid))
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0 or pc >= regions[i][1]:
            per_module[thread]["?"] += 1
            per_thread[thread]["?"] += 1
            continue
        lo, hi, off, path = regions[i]
        if path == binary_name:
            a = pc - main_base
            j = bisect.bisect_right(main_syms[0], a) - 1
            name = main_syms[1][j] if j >= 0 else "?"
            per_module[thread]["runner"] += 1
        else:
            base = os.path.basename(path)
            per_module[thread][base] += 1
            name = base
            if args.lib:
                if base not in libs:
                    local = os.path.join(args.lib, base)
                    libs[base] = symbols(args.nm, local, True) if os.path.exists(local) else ([], [])
                addrs, syms = libs[base]
                a = pc - lo + off
                j = bisect.bisect_right(addrs, a) - 1
                if j >= 0:
                    name = f"{base}:~{syms[j]}"
        per_thread[thread][name] += 1
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
            d = name
            if "EG_" in d:
                g = "eegen guest code"
            elif "FUN_" in d:
                g = "PS2Recomp guest code"
            elif "eg_vu0" in d or "egVu1" in d or "eg_vu1" in d:
                g = "translated VU"
            elif "VU1Interpreter" in d or "vu1" in d.lower():
                g = "VU interpreter"
            elif "IopKernel" in d or "IopEmulator" in d or "IopCpu" in d or "Iop" in d or "iop" in d:
                g = "IOP emulator"
            elif "Rb_tree" in d:
                g = "std::map walks"
            elif "GSGles" in d or "GS2" in d or "GS" in d or "2GS" in d:
                g = "GS"
            elif "PS2Memory" in d or "Ps2Is" in d or "9PS2Memory" in d:
                g = "PS2Memory"
            elif "EeScheduler" in d or "PS2Runtime" in d:
                g = "EE scheduler and runtime"
            elif "libc" in d:
                g = "libc"
            elif "libmali" in d:
                g = "libmali"
            else:
                g = "other"
            groups[g] += v
        print("    groups: " + "; ".join(f"{k} {100 * v / total:.1f}%" for k, v in groups.most_common()))
        for name, v in counter.most_common(args.top):
            print(f"    {100 * v / total:5.1f}%  {name[:70]}")


if __name__ == "__main__":
    main()
