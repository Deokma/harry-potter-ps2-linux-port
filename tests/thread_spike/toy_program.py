#!/usr/bin/env python3
"""A small R5900 program for the thread spike (PHASE_4): no game data.

    python3 -I toy_program.py OUT_DIR

Writes toy.elf, toy_map.csv (function map for eegen), toy_funcs.txt and
toy_layout.h. Three threads pass a semaphore round-robin from five calls down,
checking stack canaries; a high-priority thread is woken from an interrupt
handler; a low-priority thread spins on RAM with no syscall in the loop.
"""
import struct
import sys
from pathlib import Path

ZERO, V0, V1, A0, A1 = 0, 2, 3, 4, 5
T0, T1, T2, T3 = 8, 9, 10, 11
S0, S1 = 16, 17
SP, RA = 29, 31
BASE = 0x100000
DATA = 0x200000          # data page (hi = 0x20)
LOGPTR, ERR, IRQS, SPINFLAG, DCOUNT, SPINDONE, ROUNDS, PASSES = 0, 4, 8, 12, 16, 20, 24, 28
RESULT, FRESULT = 0x20, 0x40
LOG_HI = 0x21            # log at 0x210000
SEMA_D = 4
DEPTH = 5
WAIT_SEMA, SIGNAL_SEMA, ISIGNAL_SEMA, HOOK_VU0 = 0x44, 0x42, -67, 0x7001


class Asm:
    def __init__(self):
        self.words, self.labels, self.fix, self.funcs = [], {}, [], []

    def here(self):
        return BASE + len(self.words) * 4

    def func(self, name):
        if self.funcs:
            self.funcs[-1][2] = self.here()
        self.funcs.append([name, self.here(), None])
        self.label(name)

    def label(self, name):
        self.labels[name] = self.here()

    def w(self, word):
        self.words.append(word & 0xFFFFFFFF)

    def i(self, op, rs, rt, imm):
        self.w(op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF))

    def r(self, rs, rt, rd, sa, fn):
        self.w(rs << 21 | rt << 16 | rd << 11 | sa << 6 | fn)

    def addiu(self, rt, rs, imm): self.i(9, rs, rt, imm)
    def lui(self, rt, imm): self.i(0xF, 0, rt, imm)
    def lw(self, rt, off, base): self.i(0x23, base, rt, off)
    def sw(self, rt, off, base): self.i(0x2B, base, rt, off)
    def ld(self, rt, off, base): self.i(0x37, base, rt, off)
    def sd(self, rt, off, base): self.i(0x3F, base, rt, off)
    def addu(self, rd, rs, rt): self.r(rs, rt, rd, 0, 0x21)
    def sll(self, rd, rt, sa): self.r(0, rt, rd, sa, 0)
    def nop(self): self.w(0)
    def syscall(self): self.w(0xC)
    def jr(self, rs): self.r(rs, 0, 0, 0, 8)

    def branch(self, op, rs, rt, label):
        self.fix.append((len(self.words), "b", label))
        self.i(op, rs, rt, 0)

    def beq(self, rs, rt, label): self.branch(4, rs, rt, label)
    def bne(self, rs, rt, label): self.branch(5, rs, rt, label)

    def jal(self, label):
        self.fix.append((len(self.words), "j", label))
        self.w(3 << 26)

    def j(self, label):
        self.fix.append((len(self.words), "j", label))
        self.w(2 << 26)

    def mtc1(self, rt, fs): self.w(0x11 << 26 | 4 << 21 | rt << 16 | fs << 11)
    def adds(self, fd, fs, ft): self.w(0x11 << 26 | 0x10 << 21 | ft << 16 | fs << 11 | fd << 6)
    def swc1(self, ft, off, base): self.i(0x39, base, ft, off)
    def lwc1(self, ft, off, base): self.i(0x31, base, ft, off)

    def finish(self):
        self.funcs[-1][2] = self.here()
        for at, kind, label in self.fix:
            target = self.labels[label]
            if kind == "b":
                self.words[at] |= ((target - (BASE + at * 4 + 4)) >> 2) & 0xFFFF
            else:
                self.words[at] |= (target >> 2) & 0x03FFFFFF
        return struct.pack(f"<{len(self.words)}I", *self.words)


def build():
    a = Asm()

    # deep(a0 = thread 0..2, a1 = depth): at the bottom, wait for our semaphore and
    # signal the next thread's, so the switch happens with five eegen frames live
    a.func("deep")
    a.addiu(SP, SP, -0x30)
    a.sd(RA, 0x20, SP)
    a.sd(S0, 0x10, SP)
    a.sd(S1, 0, SP)
    a.addu(S0, A0, ZERO)
    a.addu(S1, A1, ZERO)
    a.sll(T0, A0, 4)             # canary on the guest stack
    a.addu(T0, T0, A1)
    a.bne(A1, ZERO, "recurse")
    a.sw(T0, 0x28, SP)           # delay slot
    a.addiu(A0, S0, 1)
    a.addiu(V1, ZERO, WAIT_SEMA)
    a.syscall()
    a.addiu(T0, S0, 1)
    a.addiu(T1, ZERO, 3)
    a.bne(T0, T1, "signal")
    a.nop()
    a.addiu(T0, ZERO, 0)
    a.label("signal")
    a.addiu(A0, T0, 1)
    a.addiu(V1, ZERO, SIGNAL_SEMA)
    a.syscall()
    a.j("check")
    a.nop()
    a.label("recurse")
    a.addu(A0, S0, ZERO)
    a.jal("deep")
    a.addiu(A1, S1, -1)          # delay slot
    a.label("check")
    a.sll(T0, S0, 4)
    a.addu(T0, T0, S1)
    a.lw(T1, 0x28, SP)
    a.beq(T0, T1, "fine")
    a.nop()
    a.lui(T2, DATA >> 16)
    a.lw(T3, ERR, T2)
    a.addiu(T3, T3, 1)
    a.sw(T3, ERR, T2)
    a.label("fine")
    a.ld(RA, 0x20, SP)
    a.ld(S0, 0x10, SP)
    a.ld(S1, 0, SP)
    a.jr(RA)
    a.addiu(SP, SP, 0x30)

    # thread_entry(a0 = 0..2): ROUNDS passes, logging the order, summing floats
    a.func("thread_entry")
    a.addiu(SP, SP, -0x40)
    a.sd(RA, 0x30, SP)
    a.sd(S0, 0x20, SP)
    a.sd(S1, 0x10, SP)
    a.addu(S0, A0, ZERO)
    a.addiu(S1, ZERO, 0)
    a.lui(T0, 0x3FC0)            # 1.5f
    a.mtc1(T0, 21)
    a.mtc1(ZERO, 20)
    a.label("round")
    a.addu(A0, S0, ZERO)
    a.jal("deep")
    a.addiu(A1, ZERO, DEPTH)     # delay slot
    a.lui(T0, DATA >> 16)
    a.lw(T1, LOGPTR, T0)
    a.sll(T2, T1, 2)
    a.lui(T3, LOG_HI)
    a.addu(T3, T3, T2)
    a.sw(S0, 0, T3)
    a.addiu(T1, T1, 1)
    a.sw(T1, LOGPTR, T0)
    a.adds(20, 20, 21)
    a.addiu(S1, S1, 1)
    a.lw(T1, ROUNDS, T0)
    a.bne(S1, T1, "round")
    a.nop()
    a.sll(T2, S0, 2)
    a.lui(T0, DATA >> 16)
    a.addu(T0, T0, T2)
    a.sw(S1, RESULT, T0)
    a.swc1(20, FRESULT, T0)
    a.ld(RA, 0x30, SP)
    a.ld(S0, 0x20, SP)
    a.ld(S1, 0x10, SP)
    a.jr(RA)
    a.addiu(SP, SP, 0x40)

    # irq_handler: what a vblank handler does, counting and waking a thread
    a.func("irq_handler")
    a.lui(T0, DATA >> 16)
    a.lw(T1, IRQS, T0)
    a.addiu(T1, T1, 1)
    a.sw(T1, IRQS, T0)
    a.lw(T1, LOGPTR, T0)         # only after every pass: the spinner must really spin
    a.lw(T2, PASSES, T0)
    a.bne(T1, T2, "wake")
    a.nop()
    a.addiu(T3, ZERO, 1)
    a.sw(T3, SPINFLAG, T0)
    a.label("wake")
    a.addiu(A0, ZERO, SEMA_D)
    a.addiu(V1, ZERO, ISIGNAL_SEMA)
    a.syscall()
    a.jr(RA)
    a.nop()

    # thread_d: high priority, woken only from the interrupt handler
    a.func("thread_d")
    a.label("d_loop")
    a.addiu(A0, ZERO, SEMA_D)
    a.addiu(V1, ZERO, WAIT_SEMA)
    a.syscall()
    a.lui(T0, DATA >> 16)
    a.lw(T1, DCOUNT, T0)
    a.addiu(T1, T1, 1)
    a.sw(T1, DCOUNT, T0)
    a.addiu(V1, ZERO, HOOK_VU0)  # native: bump a VU0 register, shared across threads
    a.syscall()
    a.j("d_loop")
    a.nop()

    # thread_s: low priority, spins on RAM with no syscall: only an asynchronous
    # interrupt can end this loop
    a.func("thread_s")
    a.lui(T0, DATA >> 16)
    a.label("spin")
    a.lw(T1, SPINFLAG, T0)
    a.beq(T1, ZERO, "spin")
    a.nop()
    a.addiu(T1, ZERO, 1)
    a.sw(T1, SPINDONE, T0)
    a.jr(RA)
    a.nop()

    return a.finish(), a.funcs


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text, funcs = build()
    ehdr = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9) + struct.pack(
        "<HHIIIIIHHHHHH", 2, 8, 1, BASE, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    phdr = struct.pack("<8I", 1, 0x1000, BASE, BASE, len(text), len(text), 5, 0x1000)
    (out / "toy.elf").write_bytes(ehdr + phdr + bytes(0x1000 - 52 - 32) + text)
    with open(out / "toy_map.csv", "w", newline="\n") as f:
        f.write("Name,Start,End,Size\n")
        for name, start, end in funcs:
            f.write(f"{name},{start:#010x},{end:#010x},{end - start}\n")
    with open(out / "toy_funcs.txt", "w", newline="\n") as f:
        for _, start, _ in funcs:
            f.write(f"{start:#x}\n")
    with open(out / "toy_layout.h", "w", newline="\n") as f:
        for name, start, _ in funcs:
            f.write(f"constexpr uint32_t TOY_{name.upper()} = {start:#x}u;\n")
        for name, off in (("LOGPTR", LOGPTR), ("ERR", ERR), ("IRQS", IRQS), ("SPINFLAG", SPINFLAG),
                          ("DCOUNT", DCOUNT), ("SPINDONE", SPINDONE), ("ROUNDS", ROUNDS), ("PASSES", PASSES),
                          ("RESULT", RESULT), ("FRESULT", FRESULT)):
            f.write(f"constexpr uint32_t TOY_{name} = {DATA + off:#x}u;\n")
        f.write(f"constexpr uint32_t TOY_LOG = {LOG_HI << 16:#x}u;\n")
        f.write(f"constexpr int TOY_SEMA_D = {SEMA_D};\n")
        f.write(f"constexpr int TOY_HOOK_VU0 = {HOOK_VU0};\n")
    print(f"{len(text)} bytes of code, {len(funcs)} functions")


if __name__ == "__main__":
    main()
