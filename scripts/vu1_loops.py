#!/usr/bin/env python3
"""Estimate VU1 cycles per vertex from the microcode's innermost loops.

Rebuilds the 16 KB VU1 micro memory from the ELF's overlays (the VU0 pair
comes first in .DVP.ovlytab and is skipped), finds backward branches, and
reports each innermost loop's body length in cycles (one instruction pair
per cycle, delay slot included, stalls not modelled) with its reciprocal
ops. A perspective transform issues one DIV (or EFU ERCPR) per vertex, and
the FDIV unit isn't pipelined, so reciprocals per iteration give vertices
per iteration. SQRT/RSQRT usually serve lighting, which leaves the count
ambiguous; both readings are printed.

Usage:
    python scripts/vu1_loops.py ELF [--json OUT.json]
"""

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_elf import analyze_vu, parse_elf, section_bytes

VU1_BYTES = 0x4000
BRANCHES = {0x20, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}  # B, IBEQ…IBGEZ
SPECIAL = {0x3BC: "DIV", 0x3BD: "SQRT", 0x3BE: "RSQRT", 0x7BD: "ERSQRT",
           0x7BE: "ERCPR", 0x6FC: "XGKICK", 0x37D: "SQI"}
PERSPECTIVE = ("DIV", "ERCPR")
OTHER_RECIP = ("SQRT", "RSQRT", "ERSQRT")


def vu1_image(data):
    elf = parse_elf(data)
    vu = analyze_vu(elf, data)
    secs = {s["name"]: s for s in elf["sections"]}
    vutext = section_bytes(data, secs[".vutext"])
    base = int(vu["vutext"]["addr"], 16)
    img = bytearray(VU1_BYTES)
    overlays = vu["overlays"][2:]  # first two: VU0 at 0x000 / 0x800
    for ov in overlays:
        src = int(ov["ee_addr"], 16) - base
        dst = int(ov["vu_addr"], 16)
        img[dst:dst + ov["bytes"]] = vutext[src:src + ov["bytes"]]
    return img, sum(ov["vu_instructions"] for ov in overlays)


def lower_kind(lower, upper):
    if upper & (1 << 31):  # I bit: the lower word is a float immediate
        return None
    op = lower >> 25
    if op == 0x40:
        return SPECIAL.get(lower & 0x7FF)
    return "SQ" if op == 0x01 else None


def innermost_loops(pairs):
    loops = []
    for i, (lower, upper) in enumerate(pairs):
        if upper & (1 << 31) or (lower >> 25) not in BRANCHES:
            continue
        imm = lower & 0x7FF
        imm -= 0x800 if imm & 0x400 else 0
        target = i + 1 + imm
        if target <= i:
            loops.append((target, i + 1))  # body incl. the delay slot
    return sorted(l for l in loops
                  if not any(o != l and l[0] <= o[0] and o[1] <= l[1]
                             for o in loops))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)

    img, count = vu1_image(args.elf.read_bytes())
    pairs = [struct.unpack_from("<II", img, 8 * i)
             for i in range(VU1_BYTES // 8)]
    loops = innermost_loops(pairs)
    print(f"VU1: {count} instructions, {len(loops)} innermost loops")
    print(f"{'addr':>13s} {'cycles':>6s} {'persp':>5s} {'other':>5s} "
          f"{'stores':>6s} {'cyc/vertex':>14s}")
    rows = []
    for start, end in loops:
        kinds = [lower_kind(*pairs[j]) for j in range(start, end + 1)]
        cycles = end - start + 1
        divs, ercprs = kinds.count("DIV"), kinds.count("ERCPR")
        persp = divs + ercprs
        other = sum(kinds.count(k) for k in OTHER_RECIP)
        stores = kinds.count("SQ") + kinds.count("SQI")
        # vertices per iteration: at most one per DIV / ERCPR, at least
        # max(DIV, ERCPR) if a vertex uses both
        per_vertex = None
        span = "-"
        if persp:
            per_vertex = [round(cycles / persp, 1),
                          round(cycles / max(divs, ercprs), 1)]
            span = (f"{per_vertex[0]:.1f}" if per_vertex[0] == per_vertex[1]
                    else f"{per_vertex[0]:.1f}-{per_vertex[1]:.1f}")
        rows.append({"start": hex(start * 8), "end": hex(end * 8),
                     "cycles": cycles, "perspective_recips": persp,
                     "other_recips": other, "stores": stores,
                     "cycles_per_vertex": per_vertex})
        print(f"0x{start * 8:04x}-0x{end * 8:04x} {cycles:6d} {persp:5d} "
              f"{other:5d} {stores:6d} {span:>14s}")
    if args.json:
        args.json.write_text(json.dumps({"vu1_instructions": count,
                                         "loops": rows}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
