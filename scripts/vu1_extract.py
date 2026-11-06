#!/usr/bin/env python3
"""Pull what one frame's VU1 work needs out of a PCSX2 savestate, into small files for vu1play.

    python -I scripts/vu1_extract.py STATE OUT_DIR [--tadr 0xADDR]

Writes stream.bin (the VIF1 chain flattened: each DMA tag's two VIFcodes, then its data, as the VIF sees
them), data.bin and micro.bin (VU1 data and micro memory), and vifregs.bin (VIF1 registers as 18 u32s: STAT
CYCLE MODE MASK ITOPS BASE OFST TOPS ITOP TOP, R0-3, C0-3). All of it is game data: keep it out of git.
"""
import argparse
import struct
import zipfile
from pathlib import Path


def flatten(ram, tadr, limit=2000000):
    out = bytearray()
    stack = []
    addr = tadr & 0x1FFFFFF0
    for _ in range(limit):
        lo, hi = struct.unpack_from("<QQ", ram, addr)
        qwc, tag_id = lo & 0xFFFF, (lo >> 28) & 7
        ptr = (lo >> 32) & 0x1FFFFFF0
        out += ram[addr + 8:addr + 16]
        follows = addr + 16
        data = ptr if tag_id in (0, 3, 4) else follows
        out += ram[data:data + qwc * 16]
        after = follows + qwc * 16
        if tag_id in (0, 7):  # refe, end
            return bytes(out)
        if tag_id == 1:  # cnt
            addr = after
        elif tag_id == 2:  # next
            addr = ptr
        elif tag_id in (3, 4):  # ref, refs
            addr = follows
        elif tag_id == 5:  # call
            stack.append(after)
            addr = ptr
        elif tag_id == 6:  # ret
            if not stack:
                return bytes(out)
            addr = stack.pop()
    raise SystemExit("chain too long")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("state")
    ap.add_argument("out")
    ap.add_argument("--tadr", type=lambda s: int(s, 0))
    args = ap.parse_args()
    z = zipfile.ZipFile(args.state)
    ram = z.read("eeMemory.bin")
    tadr = args.tadr if args.tadr is not None else struct.unpack_from("<I", ram, 0x3284F4)[0]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    stream = flatten(ram, tadr)
    (out / "stream.bin").write_bytes(stream)
    (out / "data.bin").write_bytes(z.read("vu1Memory.bin"))
    (out / "micro.bin").write_bytes(z.read("vu1MicroMem.bin"))
    hw = z.read("eeHwRegs.bin")
    regs = [0x3C00, 0x3C40, 0x3C50, 0x3C70, 0x3C90, 0x3CA0, 0x3CB0, 0x3CC0, 0x3CD0, 0x3CE0,
            0x3D00, 0x3D10, 0x3D20, 0x3D30, 0x3D40, 0x3D50, 0x3D60, 0x3D70]
    (out / "vifregs.bin").write_bytes(b"".join(hw[r:r + 4] for r in regs))
    print(f"{out.name}: chain at {tadr:#x}, stream {len(stream) // 1024} KiB")


if __name__ == "__main__":
    main()
