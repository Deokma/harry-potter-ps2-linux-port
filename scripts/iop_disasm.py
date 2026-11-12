#!/usr/bin/env python3
"""A small R3000 disassembler for IRX modules (no MIPS objdump in WSL), to read hot spots the [iopc] lines name.

usage: python3 -I scripts/iop_disasm.py MODULE.IRX FROM_HEX TO_HEX
Offsets are module-relative (as in [iopc] "LIBSD.IRX;1+3800"): the first PT_LOAD segment's vaddr 0.
"""
import struct
import sys

R = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 12: "syscall", 13: "break",
           16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo", 24: "mult", 25: "multu", 26: "div", 27: "divu", 32: "add",
           33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt", 43: "sltu"}
OPS = {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi", 13: "ori", 14: "xori", 15: "lui", 32: "lb", 33: "lh",
       34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 46: "swr"}


def dis(pc, w):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if w == 0:
        return "nop"
    if op == 0:
        n = SPECIAL.get(fn, f"special{fn}")
        if fn in (0, 2, 3):
            return f"{n} {R[rd]}, {R[rt]}, {sa}"
        if fn == 8:
            return f"jr {R[rs]}"
        if fn == 9:
            return f"jalr {R[rd]}, {R[rs]}"
        if fn in (16, 18):
            return f"{n} {R[rd]}"
        if fn in (24, 25, 26, 27):
            return f"{n} {R[rs]}, {R[rt]}"
        return f"{n} {R[rd]}, {R[rs]}, {R[rt]}"
    if op == 1:
        n = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get(rt, "regimm")
        return f"{n} {R[rs]}, 0x{pc + 4 + simm * 4:x}"
    if op in (2, 3):
        return f"{'j' if op == 2 else 'jal'} 0x{(w & 0x3FFFFFF) << 2:x}"
    if op in (4, 5):
        return f"{'beq' if op == 4 else 'bne'} {R[rs]}, {R[rt]}, 0x{pc + 4 + simm * 4:x}"
    if op in (6, 7):
        return f"{'blez' if op == 6 else 'bgtz'} {R[rs]}, 0x{pc + 4 + simm * 4:x}"
    if op == 15:
        return f"lui {R[rt]}, 0x{imm:x}"
    if op in (12, 13, 14):
        return f"{OPS[op]} {R[rt]}, {R[rs]}, 0x{imm:x}"
    if op in OPS and op >= 32:
        return f"{OPS[op]} {R[rt]}, {simm}({R[rs]})"
    if op in OPS:
        return f"{OPS[op]} {R[rt]}, {R[rs]}, {simm}"
    if op == 16:
        return f"cop0 {['mfc0', '', '', '', 'mtc0'][rs] if rs in (0, 4) else rs} {R[rt]}, ${rd}" if rs in (0, 4) else f"cop0 0x{w:08x}"
    return f".word 0x{w:08x}"


def main():
    data = open(sys.argv[1], "rb").read()
    lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
    phoff, = struct.unpack_from("<I", data, 28)
    phnum, = struct.unpack_from("<H", data, 44)
    phentsize, = struct.unpack_from("<H", data, 42)
    seg = None
    for i in range(phnum):
        ptype, off, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if ptype == 1:
            seg = (off, vaddr, filesz)
            break
    off, vaddr, filesz = seg
    for pc in range(lo & ~3, hi, 4):
        if pc - vaddr < 0 or pc - vaddr + 4 > filesz:
            continue
        w, = struct.unpack_from("<I", data, off + pc - vaddr)
        print(f"{pc:6x}: {w:08x}  {dis(pc, w)}")


if __name__ == "__main__":
    main()
