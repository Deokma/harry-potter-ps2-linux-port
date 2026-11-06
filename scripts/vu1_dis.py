#!/usr/bin/env python3
"""Lists VU1 micro memory: python -I scripts/vu1_dis.py MICRO.bin FROM [TO]  (pair indices). Output is game code: don't commit it."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vu1_stats import lower_name, upper_name, signed11  # noqa: E402


def dest(d):
    return "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if d & b)


def upper_text(u):
    d, ft, fs, fd = u >> 21 & 15, u >> 16 & 31, u >> 11 & 31, u >> 6 & 31
    name = upper_name(u)
    bits = "".join(c for c, b in (("I", 31), ("E", 30), ("M", 29), ("D", 28), ("T", 27)) if u >> b & 1)
    op = u & 0x3F
    if name == "NOP":
        text = "nop"
    elif op < 0x3C:
        text = f"{name}.{dest(d)} vf{fd}, vf{fs}, vf{ft}" + (f".{'xyzw'[op & 3]}" if op < 0x1C else "")
    else:
        k = (u & 3) | (u >> 4 & 0x7C)
        text = f"{name}.{dest(d)} vf{ft}|acc, vf{fs}, vf{ft}" + (f".{'xyzw'[k & 3]}" if k < 0x10 or 0x18 <= k < 0x1C else "")
    return (f"[{bits}] " if bits else "") + text


def lower_text(l, u, pc):
    if u >> 31 & 1:
        return f"LOI {struct.unpack('<f', struct.pack('<I', l))[0]:g} ({l:#010x})"
    if l in (0, 0x8000033C):
        return "nop"
    d, vft, vfs, vit, vis, vid = l >> 21 & 15, l >> 16 & 31, l >> 11 & 31, l >> 16 & 15, l >> 11 & 15, l >> 6 & 15
    name = lower_name(l, u)
    imm = signed11(l & 0x7FF)
    if name in ("B", "BAL", "IBEQ", "IBNE", "IBLTZ", "IBGTZ", "IBLEZ", "IBGEZ"):
        return f"{name} vi{vit}, vi{vis} -> {(pc + 1 + imm) & 0x7FF:#x}"
    if name in ("LQ", "SQ", "ILW", "ISW"):
        return f"{name}.{dest(d)} vf{vft}/vi{vit}, vf{vfs}/vi{vis} + {imm}"
    if name in ("IADDIU", "ISUBIU"):
        return f"{name} vi{vit}, vi{vis}, {(l >> 21 & 15) << 11 | l & 0x7FF:#x}"
    if name in ("IADD", "ISUB", "IAND", "IOR"):
        return f"{name} vi{vid}, vi{vis}, vi{vit}"
    if name in ("FCSET", "FCAND", "FCEQ", "FCOR"):
        return f"{name} {l & 0xFFFFFF:#x}"
    if name in ("JR", "JALR", "XGKICK", "XTOP", "XITOP", "FMEQ", "FCGET", "IADDI", "MTIR", "MFIR"):
        return f"{name} vi{vit}, vi{vis}, vf{vfs}, .{dest(d)}"
    return f"{name}.{dest(d)} vf{vft}, vf{vfs} (fsf {l >> 21 & 3}, ftf {l >> 23 & 3}) vi{vit}, vi{vis}"


def main():
    code = Path(sys.argv[1]).read_bytes()
    start = int(sys.argv[2], 0)
    stop = int(sys.argv[3], 0) if len(sys.argv) > 3 else start + 16
    for pc in range(start, stop):
        l, u = struct.unpack_from("<II", code, pc * 8)
        print(f"{pc:#05x}  {upper_text(u):36s} | {lower_text(l, u, pc)}")


if __name__ == "__main__":
    main()
