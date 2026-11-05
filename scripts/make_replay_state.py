#!/usr/bin/env python3
"""Build a replay state for one guest call from a PCSX2 savestate.

The output is the recorder's format (PS2Recomp trial patch,
PS2PORT_SNAP_FILE): a 16-byte header ("PNSP", version 1, context size, 0),
an R5900Context, 32 MB of EE RAM and the 16 KB scratchpad. RAM, scratchpad
and VU0's vf/vi registers come from the savestate. The call's registers are
given on the command line: the savestate's own CPU registers belong to the
PS2 kernel, not to a game thread. VU0's data and micro memory are written
next to it (.vu0data, .vu0code) for MMIO models that need them.

Field offsets come from ctx_offsets.json (private/third_party/trial/harness/
ctx_offsets.cpp, identical on x86-64 and aarch64).

The output is game data: write it under private/.

Usage:
    python scripts/make_replay_state.py STATE.p2s OFFSETS.json OUT \
        --pc 0x2635d0 --sp 0x1ffebd0 --gp 0x3a7570 [--reg a0=0x...]
"""

import argparse
import json
import struct
import sys
import zipfile
from pathlib import Path

REG_NAMES = ("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 "
             "s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra").split()
# "vuMicroRegs" section: tag (32 bytes), 16 bytes, then VU0 VF[32], VI[32]
# (16 bytes each); located by vf0 == (0, 0, 0, 1.0), see PHASE_3 Part C
VF0 = struct.pack("<4f", 0.0, 0.0, 0.0, 1.0)


def vu0_registers(structures):
    tag = structures.find(b"vuMicroRegs")
    if tag < 0:
        raise SystemExit("no vuMicroRegs section in the savestate")
    at = structures.find(VF0, tag + 32, tag + 32 + 64)
    if at < 0:
        raise SystemExit("VU0 vf0 not found after vuMicroRegs")
    vf = structures[at:at + 512]
    vi = [struct.unpack_from("<I", structures, at + 512 + 16 * k)[0] & 0xFFFF
          for k in range(16)]
    return vf, vi


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("state", type=Path)
    ap.add_argument("offsets", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--pc", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--sp", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--gp", type=lambda v: int(v, 0), required=True)
    ap.add_argument("--reg", action="append", default=[],
                    help="NAME=VALUE, e.g. a0=0x1234 (64-bit, low half)")
    ap.add_argument("--count", type=lambda v: int(v, 0), default=0,
                    help="CP0 Count the replay reads. The game's clock is Count: "
                         "left at 0 it looks like a huge time jump and the game runs "
                         "its maximum catch-up ticks (PHASE_3 C7)")
    ap.add_argument("--from-block", type=lambda v: int(v, 0), default=None,
                    help="resume a waiting thread: address of the context the PS2 "
                         "kernel saved for it (its TCB's stack pointer): 32 GPRs of "
                         "16 bytes, then 32 FPU registers. Replaces --sp, --gp, ra.")
    ap.add_argument("--poke", action="append", default=[],
                    help="ADDR=VALUE, a 32-bit word of RAM to change first")
    args = ap.parse_args(argv)

    off = json.loads(args.offsets.read_text())
    z = zipfile.ZipFile(args.state)
    ram = bytearray(z.read("eeMemory.bin"))
    spr = z.read("Scratchpad.bin")
    assert len(ram) == 32 << 20 and len(spr) == 16 << 10
    vf, vi = vu0_registers(z.read("PCSX2 Internal Structures.dat"))
    for item in args.poke:
        addr, value = item.split("=")
        struct.pack_into("<I", ram, int(addr, 0), int(value, 0))

    ctx = bytearray(off["size"])
    regs = {"gp": args.gp, "sp": args.sp, "ra": 0}
    if args.from_block is not None:
        regs = {}
        ctx[off["r"] + 16:off["r"] + 512] = ram[args.from_block + 16:args.from_block + 512]
        ctx[off["f"]:off["f"] + 128] = ram[args.from_block + 512:args.from_block + 640]
    for item in args.reg:
        name, value = item.split("=")
        regs[name] = int(value, 0)
    for name, value in regs.items():
        struct.pack_into("<Q", ctx, off["r"] + 16 * REG_NAMES.index(name),
                         value & 0xFFFFFFFFFFFFFFFF)
    struct.pack_into("<I", ctx, off["pc"], args.pc)
    # cop0_count, cop0_entryhi, cop0_compare, cop0_status in R5900Context
    struct.pack_into("<I", ctx, off["cop0_status"] - 12, args.count)
    ctx[off["vu0_vf"]:off["vu0_vf"] + 512] = vf
    for k, value in enumerate(vi):
        struct.pack_into("<H", ctx, off["vi"] + 2 * k, value)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out.with_name(args.out.name + ".in"), "wb") as f:
        f.write(struct.pack("<4I", 0x50534E50, 1, off["size"], 0))
        f.write(ctx)
        f.write(ram)
        f.write(spr)
    args.out.with_name(args.out.name + ".vu0data").write_bytes(z.read("vu0Memory.bin"))
    args.out.with_name(args.out.name + ".vu0code").write_bytes(z.read("vu0MicroMem.bin"))
    print(f"{args.out}.in: pc {args.pc:#x} sp {args.sp:#x} gp {args.gp:#x}, "
          f"VU0 vf1 {struct.unpack_from('<4f', vf, 16)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
