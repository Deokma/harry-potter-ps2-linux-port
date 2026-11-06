#!/usr/bin/env python3
"""What a saved frame asks of VU1: entry points, instruction kinds, control flow.

    python -I scripts/vu1_stats.py STATE [--tadr 0xADDR] [--dump OUT.bin]

STATE is a PCSX2 savestate. Walks the frame's VIF1 chain (as vif_chain.py does) for MSCAL addresses and
the VIF state they run with, then reads VU1 micro memory from the state and traces each entry: pairs
reached, backward branches, conditional branches, E-bits, and which instructions and bits are used.
Prints numbers only. --dump writes micro memory to a file, which is game data: keep it in private/.
"""
import argparse
import collections
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vif_chain import walk  # noqa: E402

LOWER = {
    0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ", 0x11: "FCSET",
    0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR", 0x18: "FMEQ", 0x1A: "FMAND",
    0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ",
    0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ",
}
SPECIAL_LOWER = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
SPECIAL2 = {
    0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD", 0x38: "DIV", 0x39: "SQRT", 0x3A: "RSQRT",
    0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR", 0x3E: "ILWR", 0x3F: "ISWR", 0x40: "RNEXT", 0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR",
    0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP", 0x6C: "XGKICK", 0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG", 0x73: "ERLENG",
    0x74: "EATANxy", 0x75: "EATANxz", 0x76: "ESUM", 0x78: "ESQRT", 0x79: "ERSQRT", 0x7A: "ERCPR", 0x7B: "WAITP", 0x7C: "ESIN",
    0x7D: "EATAN", 0x7E: "EEXP",
}
UPPER = {0x1C: "MULq", 0x1D: "MAXi", 0x1E: "MULi", 0x1F: "MINIi", 0x20: "ADDq", 0x21: "MADDq", 0x22: "ADDi", 0x23: "MADDi",
         0x24: "SUBq", 0x25: "MSUBq", 0x26: "SUBi", 0x27: "MSUBi", 0x28: "ADD", 0x29: "MADD", 0x2A: "MUL", 0x2B: "MAX", 0x2C: "SUB",
         0x2D: "MSUB", 0x2E: "OPMSUB", 0x2F: "MINI"}
UPPER_BC = {0x00: "ADDbc", 0x04: "SUBbc", 0x08: "MADDbc", 0x0C: "MSUBbc", 0x10: "MAXbc", 0x14: "MINIbc", 0x18: "MULbc"}
SPECIAL_UPPER = {
    0x00: "ADDAbc", 0x04: "SUBAbc", 0x08: "MADDAbc", 0x0C: "MSUBAbc", 0x10: "ITOF0", 0x11: "ITOF4", 0x12: "ITOF12", 0x13: "ITOF15",
    0x14: "FTOI0", 0x15: "FTOI4", 0x16: "FTOI12", 0x17: "FTOI15", 0x18: "MULAbc", 0x1C: "MULAq", 0x1D: "ABS", 0x1E: "MULAi", 0x1F: "CLIP",
    0x20: "ADDAq", 0x21: "MADDAq", 0x22: "ADDAi", 0x23: "MADDAi", 0x24: "SUBAq", 0x25: "MSUBAq", 0x26: "SUBAi", 0x27: "MSUBAi",
    0x28: "ADDA", 0x29: "MADDA", 0x2A: "MULA", 0x2C: "SUBA", 0x2D: "MSUBA", 0x2E: "OPMULA", 0x2F: "NOP",
}


def upper_name(u):
    op = u & 0x3F
    if op >= 0x3C:
        k = (u & 3) | (u >> 4 & 0x7C)
        return SPECIAL_UPPER.get(k & ~3 if k < 0x10 or 0x18 <= k < 0x1C else k, f"U?{k:#x}")
    if op < 0x1C:
        return UPPER_BC.get(op & ~3, f"U?{op:#x}")
    return UPPER.get(op, f"U?{op:#x}")


def lower_name(l, upper):
    if upper >> 31 & 1:
        return "LOI"
    op = l >> 25
    if op == 0x40:
        f = l & 0x3F
        if f >= 0x3C:
            k = (l >> 6 & 31) << 2 | l & 3
            return SPECIAL2.get(k, f"L?{k:#x}")
        return SPECIAL_LOWER.get(f, f"L?{f:#x}")
    return LOWER.get(op, f"L?{op:#x}")


def signed11(v):
    return v - 0x800 if v & 0x400 else v


def trace(code, entry):
    """Every pair reachable from entry. Follows branches both ways, and JR / JALR through a register holding a
    known constant (the stubs at the entry points, BAL's link). An unknown jump is counted and ends the path."""
    seen = {}
    visited = set()
    work = [(entry, {0: 0})]
    info = collections.Counter()
    backward = set()
    while work and len(visited) < 200000:
        pc, vi = work.pop()
        vi = dict(vi)
        delayed = None
        while True:
            key = (pc, tuple(sorted(vi.items())))
            if key in visited or pc * 8 + 8 > len(code):
                break
            visited.add(key)
            l, u = struct.unpack_from("<II", code, pc * 8)
            name = lower_name(l, u)
            seen[pc] = (name, upper_name(u), u)
            vit, vis = l >> 16 & 15, l >> 11 & 15
            next_pc = pc + 1
            if delayed is not None:
                next_pc, delayed = delayed, None
            if name in ("IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ", "B", "BAL"):
                target = pc + 1 + signed11(l & 0x7FF)
                if target <= pc and (pc, target) not in backward:
                    backward.add((pc, target))
                    info["backward"] += 1
                if name in ("B", "BAL"):
                    if name == "BAL" and vit:
                        vi[vit] = pc + 2
                    delayed = target
                else:
                    info["conditional"] += 1
                    work.append((target, dict(vi)))
            elif name in ("JR", "JALR"):
                if name == "JALR" and vit:
                    vi[vit] = pc + 2
                if vis in vi:
                    delayed = vi[vis]
                else:
                    info["jr_unknown"] += 1
                    pc = next_pc
                    l2, u2 = struct.unpack_from("<II", code, pc * 8)
                    seen[pc] = (lower_name(l2, u2), upper_name(u2), u2)
                    break
            elif name == "IADDIU" and vit:
                imm = (l >> 21 & 0xF) << 11 | l & 0x7FF
                if vis in vi:
                    vi[vit] = vi[vis] + imm
                else:
                    vi.pop(vit, None)
            elif name == "ISUBIU" and vit:
                imm = (l >> 21 & 0xF) << 11 | l & 0x7FF
                if vis in vi:
                    vi[vit] = vi[vis] - imm
                else:
                    vi.pop(vit, None)
            elif name in ("IADD", "ISUB", "IAND", "IOR"):
                vi.pop(l >> 6 & 15, None)
            elif name in ("IADDI", "ILW", "ILWR", "MTIR", "XTOP", "XITOP", "FCAND", "FCOR", "FCEQ", "FCGET", "FMAND", "FMOR", "FMEQ", "FSAND", "FSOR", "FSEQ", "RGET"):
                if vit:
                    vi.pop(vit, None)
            elif name in ("LQI", "LQD"):
                vi.pop(vis, None)
            elif name in ("SQI", "SQD"):
                vi.pop(vit, None)
            if u >> 30 & 1:
                # the pair after an E-bit runs too
                info["ebit"] += 1
                l2, u2 = struct.unpack_from("<II", code, (pc + 1) * 8)
                seen[pc + 1] = (lower_name(l2, u2), upper_name(u2), u2)
                break
            pc = next_pc
    return seen, info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("state")
    ap.add_argument("--tadr", type=lambda s: int(s, 0))
    ap.add_argument("--dump")
    args = ap.parse_args()
    z = zipfile.ZipFile(args.state)
    code = z.read("vu1MicroMem.bin")
    if args.dump:
        Path(args.dump).write_bytes(code)
    ram = z.read("eeMemory.bin")
    tadr = args.tadr if args.tadr is not None else struct.unpack_from("<I", ram, 0x3284F4)[0]
    vif, tags, qwords, how = walk(ram, tadr)
    calls = vif.mscal
    print("tags", sum(tags.values()), "qwords", qwords, how, "MSCAL", sum(calls.values()), "entries", len(calls), "MPG bytes", vif.mpg_bytes, "unpack vectors", vif.unpack_vectors, "VIFcodes", dict(vif.ops.most_common(12)))
    all_lower, all_upper = collections.Counter(), collections.Counter()
    bits = collections.Counter()
    for entry, n in sorted(calls.items(), key=lambda e: -e[1]):
        seen, info = trace(code, entry)
        print(f"  entry {entry:#06x} calls {n:5d}  reach {len(seen):5d} pairs  back {info['backward']:3d} cond {info['conditional']:3d} jr? {info['jr_unknown']:2d} ebit {info['ebit']:2d}")
        for name, up, u in seen.values():
            all_lower[name] += 1
            all_upper[up] += 1
            for b, label in ((28, "D"), (27, "T"), (29, "M"), (31, "I")):
                if u >> b & 1:
                    bits[label] += 1
    print("lower:", " ".join(f"{k}:{v}" for k, v in all_lower.most_common()))
    print("upper:", " ".join(f"{k}:{v}" for k, v in all_upper.most_common()))
    print("bits:", dict(bits))


if __name__ == "__main__":
    main()
