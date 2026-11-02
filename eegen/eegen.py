#!/usr/bin/env python3
"""eegen: the port's own R5900 code generator (Phase 3, milestone 1).

    python3 -I eegen/eegen.py ELF FUNCTION_MAP OUT_DIR --functions FILE
        [--ps2recomp-table REGISTER_FUNCTIONS_CPP] [--native FILE] [--skip ADDR,...]
        [--hardware-semantics]

Translates whole guest functions (ranges from FUNCTION_MAP, the CSV that
scripts/export_function_map.py writes) into C++ with the same interface as
PS2Recomp's generated functions, void f(uint8_t *rdram, R5900Context *ctx,
PS2Runtime *runtime), so the two can be mixed function by function: a
function eegen translates is registered at its start address, and every
other entry into it (resuming after an unwind) still goes to PS2Recomp's
resumable version.

What it does differently from PS2Recomp's generator:
  - guest GPRs (low 64 bits) and FPU registers live in C++ locals inside a
    function; the context is written back only before calls, syscalls and
    returns (registers that may be dirty there) and read again after them
    (registers live there), from a dataflow pass over the function;
  - no per-instruction pc stores, no loop checkpoints (as the quick variant
    v4 of the trial; preemption needs its own design later);
  - calls go straight to the callee's C++ function (no table lookup); jal to
    an address that is not a known function start, and jalr, go through the
    runtime's dispatchGuestBranch as before;
  - guest memory: one compare for RDRAM below 32 MB, everything else out of
    line (eegen_rt.h);
  - FPU arithmetic with PS2Recomp's float model inline (eegen_rt.h).

FILE lists function start addresses (first column, hex), e.g. the .hot list
of scripts/sample_profile.py. --native lists functions the runtime replaces
with native code (PS2Recomp's stubs: memcpy, pad, DMA, SIF RPC, ...); those
are never translated, calls go to the runtime's version. A function that uses an instruction eegen does
not handle yet is left out (reported) and keeps PS2Recomp's code.

By default the semantics follow PS2Recomp's output exactly where it differs
from the hardware (so the result can be checked byte for byte against the
PS2Recomp reference): sqrt.s reads fs, cvt.w.s rounds to nearest, lq / sq do
not clear the low address bits, blez / bgtz / bltz / bgez compare the low 32
bits. --hardware-semantics switches to what the R5900 does.

Writes OUT_DIR/EG_xxxxxxxx.cpp per function, OUT_DIR/eg_table.cpp (the
eg_functions[] list) and OUT_DIR/eegen_report.txt. The output is derived from
the game: keep it under private/.
"""
import argparse
import collections
import csv
import re
import struct
from pathlib import Path


class Unsupported(Exception):
    pass


# --------------------------------------------------------------------------
# input
# --------------------------------------------------------------------------

def load_elf(path):
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit("not a 32-bit little-endian ELF")
    e_phoff, e_shoff = struct.unpack_from("<II", data, 28)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", data, 42)
    segments = []
    for i in range(e_phnum):
        p_type, off, vaddr, _, filesz = struct.unpack_from("<5I", data, e_phoff + i * e_phentsize)
        if p_type == 1:
            segments.append((vaddr, data[off:off + filesz]))
    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from("<10I", data, e_shoff + i * e_shentsize)
        sections.append({"type": sh[1], "flags": sh[2], "addr": sh[3], "offset": sh[4], "size": sh[5]})
    return data, segments, sections


class Memory:
    def __init__(self, segments):
        self.segments = segments

    def word(self, addr):
        for base, blob in self.segments:
            if base <= addr < base + len(blob) - 3:
                return struct.unpack_from("<I", blob, addr - base)[0]
        raise Unsupported(f"no code at 0x{addr:08x}")


def jump_table_targets(data, sections, lo, hi):
    """Runs of >= 3 words in non-executable data that point into code
    (the same rule as scripts/analyze_elf.py)."""
    targets = set()
    for s in sections:
        if s["type"] != 1 or s["flags"] & 4 or s["size"] < 12:
            continue
        words = struct.unpack_from(f"<{s['size'] // 4}I", data, s["offset"])
        run = []
        for w in words + (0,):
            if lo <= w < hi and w % 4 == 0:
                run.append(w)
            else:
                if len(run) >= 3:
                    targets.update(run)
                run = []
    return targets


# --------------------------------------------------------------------------
# instruction semantics
# --------------------------------------------------------------------------

def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def u32(n):
    return "0u" if n == 0 else f"((uint32_t)r{n})"


def s32(n):
    return "0" if n == 0 else f"((int32_t)r{n})"


def u64(n):
    return "0ull" if n == 0 else f"r{n}"


def s64(n):
    return "0ll" if n == 0 else f"((int64_t)r{n})"


def gpr(*ns):
    return {f"r{n}" for n in ns if n}


def fpr(*ns):
    return {f"f{n}" for n in ns}


class Ins:
    """One decoded instruction: kind, C code for plain instructions, the
    registers it reads (uses) and writes (defs)."""

    def __init__(self, addr, word):
        self.addr, self.word = addr, word
        self.kind = "op"     # op nop branch likely jump jal jalr jr syscall break
        self.mn = "?"
        self.code = ""
        self.uses, self.defs = set(), set()
        self.cond = None     # branch condition (C expression)
        self.target = None   # branch / jump target
        self.rs = self.rd = 0


def translate(addr, w, hw):
    """Decode one instruction (hw: hardware semantics instead of PS2Recomp's)."""
    i = Ins(addr, w)
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = sext16(imm)
    ea = f"0x{simm & 0xFFFFFFFF:x}u" if rs == 0 else f"({u32(rs)} + 0x{simm & 0xFFFFFFFF:x}u)"
    branch_target = (addr + 4 + (simm << 2)) & 0xFFFFFFFF

    def op_(mn, code, uses=(), defs=()):
        i.mn, i.code = mn, code
        i.uses, i.defs = set(uses), set(defs)
        return i

    def set_s32(n, e):
        return f"r{n} = (uint64_t)(int64_t)(int32_t)({e});" if n else ""

    def set_ze32(n, e):
        return f"r{n} = (uint64_t)(uint32_t)({e});" if n else ""

    def set_u64(n, e):
        return f"r{n} = (uint64_t)({e});" if n else ""

    def load(mn, setter, expr):
        code = setter(rt, expr) if rt else f"(void)({expr});"
        return op_(mn, code, gpr(rs), gpr(rt))

    def branch(mn, cond, uses, likely=False):
        i.mn, i.kind, i.cond, i.target = mn, "likely" if likely else "branch", cond, branch_target
        i.uses = set(uses)
        if cond == "true":
            i.kind = "jump"
        return i

    if op == 0:
        if w == 0:
            i.mn, i.kind = "nop", "nop"
            return i
        if fn == 0x00:
            return op_("sll", set_s32(rd, f"{u32(rt)} << {sa}"), gpr(rt), gpr(rd))
        if fn == 0x02:
            return op_("srl", set_s32(rd, f"{u32(rt)} >> {sa}"), gpr(rt), gpr(rd))
        if fn == 0x03:
            return op_("sra", set_s32(rd, f"{s32(rt)} >> {sa}"), gpr(rt), gpr(rd))
        if fn == 0x04:
            return op_("sllv", set_s32(rd, f"{u32(rt)} << ({u32(rs)} & 31u)"), gpr(rs, rt), gpr(rd))
        if fn == 0x06:
            return op_("srlv", set_s32(rd, f"{u32(rt)} >> ({u32(rs)} & 31u)"), gpr(rs, rt), gpr(rd))
        if fn == 0x07:
            return op_("srav", set_s32(rd, f"{s32(rt)} >> ({u32(rs)} & 31u)"), gpr(rs, rt), gpr(rd))
        if fn == 0x08:
            i.mn, i.kind, i.rs, i.uses = "jr", "jr", rs, gpr(rs)
            return i
        if fn == 0x09:
            i.mn, i.kind, i.rs, i.rd, i.uses, i.defs = "jalr", "jalr", rs, rd, gpr(rs), gpr(rd)
            return i
        if fn in (0x0A, 0x0B):
            mn = "movz" if fn == 0x0A else "movn"
            if rd == 0:
                return op_(mn, "")
            test = "==" if fn == 0x0A else "!="
            hi = f"eg_get_hi(ctx, {rs})" if rs else "0ull"
            code = f"if ({u64(rt)} {test} 0) {{ r{rd} = {u64(rs)}; eg_put_hi(ctx, {rd}, {hi}); }}"
            return op_(mn, code, gpr(rs, rt, rd), gpr(rd))
        if fn == 0x0C:
            i.mn, i.kind, i.target = "syscall", "syscall", (w >> 6) & 0xFFFFF
            return i
        if fn == 0x0D:
            i.mn, i.kind = "break", "break"
            return i
        if fn == 0x0F:
            return op_("sync", "")
        if fn == 0x10:
            return op_("mfhi", set_u64(rd, "ctx->hi"), (), gpr(rd))
        if fn == 0x11:
            return op_("mthi", f"ctx->hi = {u64(rs)};", gpr(rs))
        if fn == 0x12:
            return op_("mflo", set_u64(rd, "ctx->lo"), (), gpr(rd))
        if fn == 0x13:
            return op_("mtlo", f"ctx->lo = {u64(rs)};", gpr(rs))
        if fn == 0x14:
            return op_("dsllv", set_u64(rd, f"{u64(rt)} << ({u32(rs)} & 63u)"), gpr(rs, rt), gpr(rd))
        if fn == 0x16:
            return op_("dsrlv", set_u64(rd, f"{u64(rt)} >> ({u32(rs)} & 63u)"), gpr(rs, rt), gpr(rd))
        if fn == 0x17:
            return op_("dsrav", set_u64(rd, f"{s64(rt)} >> ({u32(rs)} & 63u)"), gpr(rs, rt), gpr(rd))
        if fn == 0x18:
            code = (f"{{ int64_t m_ = (int64_t){s32(rs)} * (int64_t){s32(rt)}; "
                    f"ctx->lo = (uint64_t)(int64_t)(int32_t)m_; ctx->hi = (uint64_t)(int64_t)(int32_t)(m_ >> 32); "
                    f"{set_s32(rd, 'm_')} }}")
            return op_("mult", code, gpr(rs, rt), gpr(rd))
        if fn == 0x19:
            code = (f"{{ uint64_t m_ = (uint64_t){u32(rs)} * (uint64_t){u32(rt)}; "
                    f"ctx->lo = (uint64_t)(int64_t)(int32_t)m_; ctx->hi = (uint64_t)(int64_t)(int32_t)(m_ >> 32); "
                    f"{set_s32(rd, 'm_')} }}")
            return op_("multu", code, gpr(rs, rt), gpr(rd))
        if fn == 0x1A:
            code = (f"{{ int32_t d_ = {s32(rt)}; int32_t n_ = {s32(rs)}; if (d_ != 0) {{ "
                    "if (d_ == -1 && n_ == INT32_MIN) { ctx->lo = (uint64_t)(int64_t)INT32_MIN; ctx->hi = 0; } "
                    "else { ctx->lo = (uint64_t)(int64_t)(n_ / d_); ctx->hi = (uint64_t)(int64_t)(n_ % d_); } } "
                    "else { ctx->lo = (n_ < 0) ? 1ull : 0xFFFFFFFFFFFFFFFFull; ctx->hi = (uint64_t)(int64_t)n_; } }")
            return op_("div", code, gpr(rs, rt))
        if fn == 0x1B:
            code = (f"{{ uint32_t d_ = {u32(rt)}; uint32_t n_ = {u32(rs)}; if (d_ != 0) {{ "
                    "ctx->lo = (uint64_t)(int64_t)(int32_t)(n_ / d_); ctx->hi = (uint64_t)(int64_t)(int32_t)(n_ % d_); } "
                    "else { ctx->lo = 0xFFFFFFFFFFFFFFFFull; ctx->hi = (uint64_t)(int64_t)(int32_t)n_; } }")
            return op_("divu", code, gpr(rs, rt))
        if fn == 0x21:
            return op_("addu", set_s32(rd, f"{u32(rs)} + {u32(rt)}"), gpr(rs, rt), gpr(rd))
        if fn == 0x23:
            return op_("subu", set_s32(rd, f"{u32(rs)} - {u32(rt)}"), gpr(rs, rt), gpr(rd))
        logic = {0x24: ("and", "&"), 0x25: ("or", "|"), 0x26: ("xor", "^")}
        if fn in logic:
            mn, o = logic[fn]
            return op_(mn, set_u64(rd, f"{u64(rs)} {o} {u64(rt)}"), gpr(rs, rt), gpr(rd))
        if fn == 0x27:
            return op_("nor", set_u64(rd, f"~({u64(rs)} | {u64(rt)})"), gpr(rs, rt), gpr(rd))
        if fn == 0x2A:
            return op_("slt", set_u64(rd, f"({s64(rs)} < {s64(rt)}) ? 1 : 0"), gpr(rs, rt), gpr(rd))
        if fn == 0x2B:
            return op_("sltu", set_u64(rd, f"({u64(rs)} < {u64(rt)}) ? 1 : 0"), gpr(rs, rt), gpr(rd))
        if fn == 0x2D:
            return op_("daddu", set_u64(rd, f"{u64(rs)} + {u64(rt)}"), gpr(rs, rt), gpr(rd))
        if fn == 0x2F:
            return op_("dsubu", set_u64(rd, f"{u64(rs)} - {u64(rt)}"), gpr(rs, rt), gpr(rd))
        shifts = {0x38: ("dsll", "<<", 0, u64), 0x3A: ("dsrl", ">>", 0, u64), 0x3B: ("dsra", ">>", 0, s64),
                  0x3C: ("dsll32", "<<", 32, u64), 0x3E: ("dsrl32", ">>", 32, u64), 0x3F: ("dsra32", ">>", 32, s64)}
        if fn in shifts:
            mn, o, base, get = shifts[fn]
            return op_(mn, set_u64(rd, f"{get(rt)} {o} {base + sa}"), gpr(rt), gpr(rd))
        raise Unsupported(f"special 0x{fn:02x}")

    if op == 1:
        # PS2Recomp compares the low 32 bits; the hardware the whole register
        val = s64(rs) if hw else s32(rs)
        regimm = {0: ("bltz", "<", False), 1: ("bgez", ">=", False), 2: ("bltzl", "<", True), 3: ("bgezl", ">=", True)}
        if rt in regimm:
            mn, o, likely = regimm[rt]
            return branch(mn, f"{val} {o} 0", gpr(rs), likely)
        raise Unsupported(f"regimm 0x{rt:02x}")

    if op in (2, 3):
        i.mn, i.kind = ("j", "jump") if op == 2 else ("jal", "jal")
        i.target = ((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return i

    if op in (4, 5, 0x14, 0x15):
        mn = {4: "beq", 5: "bne", 0x14: "beql", 0x15: "bnel"}[op]
        o = "==" if op in (4, 0x14) else "!="
        cond = "true" if (op == 4 and rs == rt) else f"{u64(rs)} {o} {u64(rt)}"
        if op == 0x14 and rs == rt:
            cond = "1"  # beql x, x: always taken, delay slot always run
        return branch(mn, cond, gpr(rs, rt), op >= 0x14)
    if op in (6, 7, 0x16, 0x17):
        mn = {6: "blez", 7: "bgtz", 0x16: "blezl", 0x17: "bgtzl"}[op]
        o = "<=" if op in (6, 0x16) else ">"
        val = s64(rs) if hw else s32(rs)
        return branch(mn, f"{val} {o} 0", gpr(rs), op >= 0x16)

    if op == 9:
        return op_("addiu", set_s32(rt, f"{u32(rs)} + 0x{simm & 0xFFFFFFFF:x}u"), gpr(rs), gpr(rt))
    if op == 0x0A:
        return op_("slti", set_u64(rt, f"({s64(rs)} < (int64_t){simm}) ? 1 : 0"), gpr(rs), gpr(rt))
    if op == 0x0B:
        return op_("sltiu", set_u64(rt, f"({u64(rs)} < (uint64_t)(int64_t){simm}) ? 1 : 0"), gpr(rs), gpr(rt))
    if op in (0x0C, 0x0D, 0x0E):
        mn, o = {0x0C: ("andi", "&"), 0x0D: ("ori", "|"), 0x0E: ("xori", "^")}[op]
        return op_(mn, set_u64(rt, f"{u64(rs)} {o} 0x{imm:x}ull"), gpr(rs), gpr(rt))
    if op == 0x0F:
        return op_("lui", set_s32(rt, f"0x{(imm << 16) & 0xFFFFFFFF:x}u"), (), gpr(rt))
    if op == 0x19:
        return op_("daddiu", set_u64(rt, f"{s64(rs)} + (int64_t){simm}"), gpr(rs), gpr(rt))

    if op == 0x10:
        if w == 0x42000038:
            return op_("ei", "ctx->cop0_status |= 0x10000;")
        if w == 0x42000039:
            return op_("di", "ctx->cop0_status &= ~0x10000;")
        if rs == 0 and rd == 9:
            return op_("mfc0", set_s32(rt, "(int32_t)ctx->cop0_count"), (), gpr(rt))
        raise Unsupported("cop0")

    if op == 0x11:
        ft, fs, fd = rt, rd, sa
        if rs == 0:
            return op_("mfc1", set_s32(rt, f"eg_bits(f{fs})"), fpr(fs), gpr(rt))
        if rs == 4:
            return op_("mtc1", f"f{fs} = eg_fbits({u32(rt)});", gpr(rt), fpr(fs))
        if rs == 6 and fs == 31:
            return op_("ctc1", f"ctx->fcr31 = {u32(rt)} & 0x0183FFFFu;", gpr(rt))
        if rs == 8:
            bc = {0: ("bc1f", False, False), 1: ("bc1t", True, False), 2: ("bc1fl", False, True), 3: ("bc1tl", True, True)}
            if rt not in bc:
                raise Unsupported("bc1")
            mn, t, likely = bc[rt]
            return branch(mn, "(ctx->fcr31 & 0x800000u) != 0" if t else "(ctx->fcr31 & 0x800000u) == 0", (), likely)
        if rs == 16:
            arith = {0: ("add.s", "eg_fadd"), 1: ("sub.s", "eg_fsub"), 2: ("mul.s", "eg_fmul")}
            if fn in arith:
                mn, f = arith[fn]
                return op_(mn, f"f{fd} = {f}(f{fs}, f{ft});", fpr(fs, ft), fpr(fd))
            if fn == 3:
                return op_("div.s", f"if (f{ft} == 0.0f) ctx->fcr31 |= 0x100000u; f{fd} = ps2_fdiv(f{fs}, f{ft});",
                           fpr(fs, ft), fpr(fd))
            if fn == 4:
                src = ft if hw else fs  # PS2Recomp reads fs
                return op_("sqrt.s", f"f{fd} = ps2_fsqrt(f{src});", fpr(src), fpr(fd))
            if fn == 5:
                return op_("abs.s", f"f{fd} = fabsf(f{fs});", fpr(fs), fpr(fd))
            if fn == 6:
                return op_("mov.s", f"f{fd} = f{fs};", fpr(fs), fpr(fd))
            if fn == 7:
                return op_("neg.s", f"f{fd} = -f{fs};", fpr(fs), fpr(fd))
            if fn == 0x16:
                return op_("rsqrt.s", f"f{fd} = ps2_frsqrt(f{fs}, f{ft});", fpr(fs, ft), fpr(fd))
            acc = {0x18: ("adda.s", "eg_fadd"), 0x19: ("suba.s", "eg_fsub"), 0x1A: ("mula.s", "eg_fmul")}
            if fn in acc:
                mn, f = acc[fn]
                return op_(mn, f"ctx->f_acc = {f}(f{fs}, f{ft});", fpr(fs, ft))
            if fn == 0x24:
                cvt = f"eg_cvt_w_s(f{fs})" if hw else f"(uint32_t)(int32_t)nearbyintf(f{fs})"
                return op_("cvt.w.s", f"f{fd} = eg_fbits({cvt});", fpr(fs), fpr(fd))
            if fn == 0x30:
                return op_("c.f.s", "ctx->fcr31 &= ~0x800000u;")
            cmp = {0x32: ("c.eq.s", "=="), 0x34: ("c.lt.s", "<"), 0x36: ("c.le.s", "<=")}
            if fn in cmp:
                mn, o = cmp[fn]
                return op_(mn, f"ctx->fcr31 = (f{fs} {o} f{ft}) ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);",
                           fpr(fs, ft))
            raise Unsupported(f"cop1.s 0x{fn:02x}")
        if rs == 20 and fn == 0x20:
            return op_("cvt.s.w", f"f{fd} = (float)(int32_t)eg_bits(f{fs});", fpr(fs), fpr(fd))
        raise Unsupported(f"cop1 fmt {rs}")

    if op == 0x1C:
        if fn == 0:
            code = (f"{{ uint64_t a_ = Ps2HiLoToU64(ctx->hi, ctx->lo); int64_t p_ = (int64_t){s32(rs)} * (int64_t){s32(rt)}; "
                    "int64_t m_ = a_ + p_; ctx->lo = Ps2SignExt32ToU64((uint32_t)m_); "
                    f"ctx->hi = Ps2SignExt32ToU64((uint32_t)(m_ >> 32)); {set_s32(rd, 'm_')} }}")
            return op_("madd", code, gpr(rs, rt), gpr(rd))
        raise Unsupported(f"mmi 0x{fn:02x}")

    lq_ea = f"({ea} & ~15u)" if hw else ea  # PS2Recomp keeps the low bits
    if op == 0x1E:
        call = f"eg_lq(rdram, ctx, runtime, {lq_ea}, {rt})"
        return op_("lq", f"r{rt} = {call};" if rt else f"(void){call};", gpr(rs), gpr(rt))
    if op == 0x1F:
        hi = f"eg_get_hi(ctx, {rt})" if rt else "0ull"
        return op_("sq", f"eg_sq(rdram, ctx, runtime, {lq_ea}, {u64(rt)}, {hi});", gpr(rs, rt))

    loads = {0x20: ("lb", set_s32, "(int8_t)EG_LD(uint8_t, {})"), 0x21: ("lh", set_s32, "(int16_t)EG_LD(uint16_t, {})"),
             0x23: ("lw", set_s32, "EG_LD(uint32_t, {})"), 0x24: ("lbu", set_ze32, "EG_LD(uint8_t, {})"),
             0x25: ("lhu", set_ze32, "EG_LD(uint16_t, {})"), 0x27: ("lwu", set_ze32, "EG_LD(uint32_t, {})"),
             0x37: ("ld", set_u64, "EG_LD(uint64_t, {})")}
    if op in loads:
        mn, setter, expr = loads[op]
        return load(mn, setter, expr.format(ea))
    stores = {0x28: ("sb", "uint8_t", u32), 0x29: ("sh", "uint16_t", u32), 0x2B: ("sw", "uint32_t", u32),
              0x3F: ("sd", "uint64_t", u64)}
    if op in stores:
        mn, t, get = stores[op]
        return op_(mn, f"EG_ST({t}, {ea}, {get(rt)});", gpr(rs, rt))

    head = f"uint32_t a_ = {ea}; "
    if op == 0x22:
        code = (f"{{ {head}uint32_t o_ = a_ & 3u; uint32_t m_ = EG_LD(uint32_t, a_ & ~3u); uint32_t s_ = (3u - o_) << 3; "
                f"uint32_t k_ = s_ == 0 ? 0u : ((1u << s_) - 1u); {set_s32(rt, f'({u32(rt)} & k_) | (m_ << s_)')} }}")
        return op_("lwl", code, gpr(rs, rt), gpr(rt))
    if op == 0x26:
        code = (f"{{ {head}uint32_t o_ = a_ & 3u; uint32_t m_ = EG_LD(uint32_t, a_ & ~3u); uint32_t s_ = o_ << 3; "
                f"uint32_t k_ = o_ == 0 ? 0u : (0xFFFFFFFFu << ((4u - o_) << 3)); uint32_t v_ = ({u32(rt)} & k_) | (m_ >> s_); "
                f"uint64_t w_ = ({u64(rt)} & 0xFFFFFFFF00000000ull) | (uint64_t)v_; if (o_ == 0) w_ = (uint64_t)(int64_t)(int32_t)v_; "
                f"{set_u64(rt, 'w_')} }}")
        return op_("lwr", code, gpr(rs, rt), gpr(rt))
    if op == 0x1A:
        code = (f"{{ {head}uint32_t o_ = a_ & 7u; uint64_t m_ = EG_LD(uint64_t, a_ & ~7u); uint32_t s_ = (7u - o_) << 3; "
                f"uint64_t k_ = s_ == 0 ? 0ull : ((1ull << s_) - 1ull); {set_u64(rt, f'({u64(rt)} & k_) | (m_ << s_)')} }}")
        return op_("ldl", code, gpr(rs, rt), gpr(rt))
    if op == 0x1B:
        code = (f"{{ {head}uint32_t o_ = a_ & 7u; uint64_t m_ = EG_LD(uint64_t, a_ & ~7u); uint32_t s_ = o_ << 3; "
                f"uint64_t k_ = o_ == 0 ? 0ull : (0xFFFFFFFFFFFFFFFFull << ((8u - o_) << 3)); "
                f"{set_u64(rt, f'({u64(rt)} & k_) | (m_ >> s_)')} }}")
        return op_("ldr", code, gpr(rs, rt), gpr(rt))
    if op in (0x2A, 0x2E):
        left = op == 0x2A
        sh = "(3u - o_) << 3" if left else "o_ << 3"
        mask = "0xFFFFFFFFu >> s_" if left else "0xFFFFFFFFu << s_"
        val = "(v_ >> s_)" if left else "(v_ << s_)"
        code = (f"{{ {head}uint32_t o_ = a_ & 3u; uint32_t s_ = {sh}; uint32_t k_ = {mask}; "
                f"uint32_t m_ = EG_LD(uint32_t, a_ & ~3u); uint32_t v_ = {u32(rt)}; "
                f"EG_ST(uint32_t, a_ & ~3u, (m_ & ~k_) | ({val} & k_)); }}")
        return op_("swl" if left else "swr", code, gpr(rs, rt))
    if op in (0x2C, 0x2D):
        left = op == 0x2C
        sh = "(7u - o_) << 3" if left else "o_ << 3"
        mask = "0xFFFFFFFFFFFFFFFFull >> s_" if left else "0xFFFFFFFFFFFFFFFFull << s_"
        val = "(v_ >> s_)" if left else "(v_ << s_)"
        code = (f"{{ {head}uint32_t o_ = a_ & 7u; uint32_t s_ = {sh}; uint64_t k_ = {mask}; "
                f"uint64_t m_ = EG_LD(uint64_t, a_ & ~7u); uint64_t v_ = {u64(rt)}; "
                f"EG_ST(uint64_t, a_ & ~7u, (m_ & ~k_) | ({val} & k_)); }}")
        return op_("sdl" if left else "sdr", code, gpr(rs, rt))

    if op == 0x31:
        return op_("lwc1", f"f{rt} = EG_LD(float, {ea});", gpr(rs), fpr(rt))
    if op == 0x39:
        return op_("swc1", f"EG_ST(float, {ea}, f{rt});", gpr(rs) | fpr(rt))
    if op == 0x33:
        return op_("pref", "", gpr(rs))
    if op == 0x36:
        return op_("lqc2", f"ctx->vu0_vf[{rt}] = eg_lqc2(rdram, ctx, runtime, {lq_ea});", gpr(rs))
    if op == 0x3E:
        return op_("sqc2", f"eg_sqc2(rdram, ctx, runtime, {lq_ea}, ctx->vu0_vf[{rt}]);", gpr(rs))
    raise Unsupported(f"opcode 0x{op:02x}")


def mix_class(mn):
    """Instruction class for the counting build's mix (as make_harness.py)."""
    if mn == "nop":
        return 9
    if mn in ("lq", "sq", "lqc2", "sqc2"):
        return 2
    if mn.endswith(".s") or mn.endswith(".w") or mn in ("mfc1", "mtc1", "cfc1", "ctc1", "lwc1", "swc1",
                                                        "bc1f", "bc1t", "bc1fl", "bc1tl"):
        return 4
    if mn[0] in "bj" or mn in ("jr", "jalr"):
        return 3
    if mn in ("lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lwl", "lwr", "ldl", "ldr",
              "sb", "sh", "sw", "sd", "swl", "swr", "sdl", "sdr"):
        return 1
    if mn.startswith(("mult", "div", "madd", "msub")):
        return 7
    if mn in ("syscall", "break", "sync", "pref", "ei", "di") or mn.startswith("mfc0"):
        return 8
    return 0


# --------------------------------------------------------------------------
# one function
# --------------------------------------------------------------------------

class Node:
    """A step of the emitted function. kinds: op, cjump, jump, switch (local
    control flow), sync (call / syscall / break: the context is written back
    before and read after), exit (leaves the function)."""

    def __init__(self, kind, code="", uses=(), defs=(), targets=(), fall=True, **extra):
        self.kind, self.code = kind, code
        self.uses, self.defs = set(uses), set(defs)
        self.targets, self.fall = list(targets), fall
        self.extra = extra
        self.label = None


class Program:
    def __init__(self, args):
        self.data, segments, self.sections = load_elf(args.elf)
        self.mem = Memory(segments)
        self.funcs = []
        with open(args.function_map, newline="") as f:
            for row in csv.DictReader(f):
                self.funcs.append((int(row["Start"], 16), int(row["End"], 16)))
        self.starts = {s for s, _ in self.funcs}
        lo = min(s for s, _ in self.funcs)
        hi = max(e for _, e in self.funcs)
        self.tables = jump_table_targets(self.data, self.sections, lo, hi)
        self.ps2recomp = {}
        if args.ps2recomp_table:
            pat = re.compile(r"= (FUN_([0-9a-f]{8})_0x[0-9a-f]+); // 0x([0-9a-f]+)")
            for m in pat.finditer(Path(args.ps2recomp_table).read_text()):
                if int(m.group(2), 16) == int(m.group(3), 16):
                    self.ps2recomp[int(m.group(3), 16)] = m.group(1)
        self.skip = {int(x, 16) for x in args.skip.split(",")} if args.skip else set()
        self.native = set()
        if args.native:
            for line in Path(args.native).read_text().splitlines():
                if line.strip():
                    self.native.add(int(line.split()[0], 16))
        self.hw = args.hardware_semantics
        self.fp_groups = not args.no_float_groups
        self.ours = set()

    def callee(self, t):
        """C++ function to call for guest address t, or None (dispatch)."""
        if t in self.ours:
            return f"EG_{t:08x}"
        return self.ps2recomp.get(t)


def build_nodes(prog, start, end):
    ins = {}
    for a in range(start, end, 4):
        try:
            ins[a] = translate(a, prog.mem.word(a), prog.hw)
        except Unsupported as e:
            ins[a] = e
    cases = sorted(t for t in prog.tables if start <= t < end)

    def get(a):
        i = ins.get(a)
        if isinstance(i, Exception):
            raise Unsupported(f"{i} at 0x{a:08x}")
        if i is None:
            raise Unsupported(f"falls out of the function at 0x{a:08x}")
        return i

    # reachable instruction starts (a branch and its delay slot form a pair)
    starts, work, delay_targets = set(), [start], set()
    pair = lambda i: i.kind in ("branch", "likely", "jump", "jal", "jalr", "jr")
    while work:
        a = work.pop()
        if a in starts or not (start <= a < end):
            continue
        i = get(a)
        starts.add(a)
        if pair(i):
            d = get(a + 4)
            if pair(d) or d.kind in ("syscall",):
                raise Unsupported(f"{d.mn} in a delay slot at 0x{a + 4:08x}")
        nxt = []
        if i.kind in ("branch", "likely"):
            nxt = [i.target, a + 8]
        elif i.kind == "jump":
            nxt = [i.target]
        elif i.kind in ("jal", "jalr"):
            nxt = [a + 8]
        elif i.kind == "jr":
            nxt = cases if i.rs != 31 else []
        else:
            nxt = [a + 4]
        work.extend(t for t in nxt if start <= t < end)
    for a in list(starts):
        if pair(ins[a]) and a + 4 in starts:
            delay_targets.add(a + 4)
    starts -= delay_targets

    nodes, out_of_line = [], []
    labels = {}

    def count(i):
        return f"HARNESS_COUNT_K({mix_class(i.mn)}); "

    def plain(i, into, delay=False):
        """Nodes for a non-control instruction (also a delay slot). PS2Recomp
        emits nothing for a nop in a delay slot, so the counting build does
        not count those either (the totals stay comparable)."""
        if delay and i.kind == "nop":
            return
        if i.kind == "syscall":
            into.append(Node("sync", count(i), call="syscall", addr=i.addr, code_no=i.target))
        elif i.kind == "break":
            into.append(Node("sync", count(i), call="break", addr=i.addr))
        else:
            into.append(Node("op", count(i) + i.code, i.uses, i.defs, ins=i, cnt=count(i)))

    def exit_to(t, into):
        into.append(Node("exit", fall=False, how="jump", target=t))

    def goto(t, into):
        """Local jump, or a node leaving the function."""
        if start <= t < end and (t in starts or t in delay_targets):
            into.append(Node("jump", targets=[t], fall=False))
        else:
            exit_to(t, into)

    def cjump(cond, t, into, negate=False, uses=()):
        c = f"!({cond})" if negate else cond
        if start <= t < end and (t in starts or t in delay_targets):
            into.append(Node("cjump", c, uses, targets=[t]))
        else:
            lab = f"X_{t:08x}_{len(out_of_line)}"
            into.append(Node("cjump", c, uses, targets=[lab]))
            n = Node("exit", fall=False, how="jump", target=t)
            n.label = lab
            out_of_line.append(n)

    for a in sorted(starts):
        i = ins[a]
        first = len(nodes)
        if i.kind in ("branch", "likely", "jump", "jal", "jalr", "jr"):
            d = ins[a + 4]
            ret = a + 8
            if i.kind == "branch":
                nodes.append(Node("op", count(i) + f"c_{a:x} = {i.cond};", i.uses, temp=f"bool c_{a:x}"))
                plain(d, nodes, delay=True)
                cjump(f"c_{a:x}", i.target, nodes)
                if not (start <= ret < end):
                    exit_to(ret, nodes)
            elif i.kind == "likely":
                if i.cond == "1":
                    nodes.append(Node("op", count(i)))
                else:
                    nodes.append(Node("op", count(i)))
                    cjump(i.cond, ret, nodes, negate=True, uses=i.uses)
                plain(d, nodes, delay=True)
                goto(i.target, nodes)
            elif i.kind == "jump":
                nodes.append(Node("op", count(i)))
                plain(d, nodes, delay=True)
                goto(i.target, nodes)
            elif i.kind == "jal":
                nodes.append(Node("op", count(i) + f"r31 = 0x{ret:x}ull;", (), {"r31"}))
                plain(d, nodes, delay=True)
                if i.target not in prog.skip:
                    nodes.append(Node("sync", call="jal", target=i.target, ret=ret, addr=a))
                if not (start <= ret < end):
                    exit_to(ret, nodes)
            elif i.kind == "jalr":
                code = count(i) + f"t_{a:x} = {u32(i.rs)};" + (f" r{i.rd} = 0x{ret:x}ull;" if i.rd else "")
                nodes.append(Node("op", code, i.uses, i.defs, temp=f"uint32_t t_{a:x}"))
                plain(d, nodes, delay=True)
                nodes.append(Node("sync", call="jalr", target=f"t_{a:x}", ret=ret, addr=a))
                if not (start <= ret < end):
                    exit_to(ret, nodes)
            else:  # jr
                nodes.append(Node("op", count(i) + f"t_{a:x} = {u32(i.rs)};", i.uses, temp=f"uint32_t t_{a:x}"))
                plain(d, nodes, delay=True)
                if i.rs == 31 or not cases:
                    nodes.append(Node("exit", fall=False, how="return", target=f"t_{a:x}"))
                else:
                    lab = f"D_{a:x}"
                    nodes.append(Node("switch", targets=cases + [lab], fall=False, value=f"t_{a:x}", default=lab))
                    n = Node("exit", fall=False, how="return", target=f"t_{a:x}")
                    n.label = lab
                    out_of_line.append(n)
        else:
            plain(i, nodes)
            if not (start <= a + 4 < end) or (a + 4 not in starts and a + 4 not in delay_targets):
                goto(a + 4, nodes)
        labels[a] = first
        nodes[first].label = f"L_{a:x}"
    # entries into delay slots: the instruction, then on after its branch
    for a in sorted(delay_targets):
        first = len(nodes)
        plain(ins[a], nodes)
        goto(a + 4, nodes)
        nodes[first].label = f"L_{a:x}"
    for n in out_of_line:
        nodes.append(n)
    index = {}
    for k, n in enumerate(nodes):
        if n.label:
            index[n.label] = k
    for k, n in enumerate(nodes):
        n.succ = []
        if n.fall and k + 1 < len(nodes):
            n.succ.append(k + 1)
        for t in n.targets:
            n.succ.append(index[t if isinstance(t, str) else f"L_{t:x}"])
    return nodes


def dataflow(nodes):
    """dirty: registers that may differ from the context (forward, union);
    live: registers whose local value is needed (backward)."""
    preds = collections.defaultdict(list)
    for k, n in enumerate(nodes):
        for s in n.succ:
            preds[s].append(k)
    din = [set() for _ in nodes]
    dout = [set() for _ in nodes]
    changed = True
    while changed:
        changed = False
        for k, n in enumerate(nodes):
            d = set()
            for p in preds[k]:
                d |= dout[p]
            o = set() if n.kind == "sync" else d | n.defs
            if d != din[k] or o != dout[k]:
                din[k], dout[k] = d, o
                changed = True
    lin = [set() for _ in nodes]
    lout = [set() for _ in nodes]
    changed = True
    while changed:
        changed = False
        for k in range(len(nodes) - 1, -1, -1):
            n = nodes[k]
            o = set()
            for s in n.succ:
                o |= lin[s]
            if n.kind in ("sync", "exit"):
                i = n.uses | din[k]
            else:
                i = n.uses | (o - n.defs)
            if i != lin[k] or o != lout[k]:
                lin[k], lout[k] = i, o
                changed = True
    return din, lin, lout


def reg_sort(keys):
    return sorted(keys, key=lambda r: (r[0], int(r[1:])))


def spill(keys):
    out = []
    for r in reg_sort(keys):
        out.append(f"eg_put(ctx, {r[1:]}, {r});" if r[0] == "r" else f"ctx->f[{r[1:]}] = {r};")
    return " ".join(out)


def reload(keys):
    out = []
    for r in reg_sort(keys):
        out.append(f"{r} = eg_get(ctx, {r[1:]});" if r[0] == "r" else f"{r} = ctx->f[{r[1:]}];")
    return " ".join(out)


# --------------------------------------------------------------------------
# float groups
# --------------------------------------------------------------------------
# A run of side-effect-free instructions inside a basic block is emitted
# twice. The fast version computes into fresh locals with plain host float
# arithmetic, with the FPU in flush-to-zero mode (eegen mode, see
# eegen_rt.h), then asks once whether any operation could have needed
# PS2Recomp's slow path: the sticky invalid / overflow / underflow /
# input-denormal flags, and an inf / NaN test on the float values the group
# hands on. If not, the locals are committed; if so, the exact version (the
# per-operation code, flush-to-zero off) runs the group again from the same
# state. Nothing in a group writes memory (a load that is not RDRAM leaves
# for the exact version before it happens), so running it twice is safe.

FP_ARITH = {"add.s", "sub.s", "mul.s", "adda.s", "suba.s", "mula.s"}
FP_SENSITIVE = FP_ARITH | {"c.eq.s", "c.lt.s", "c.le.s"}
GROUP_OK = FP_SENSITIVE | {
    "mov.s", "neg.s", "abs.s", "mtc1", "mfc1", "cvt.s.w", "cvt.w.s", "c.f.s", "nop", "sync", "pref",
    "lb", "lh", "lw", "lbu", "lhu", "lwu", "ld", "lwc1", "mfhi", "mflo",
    "sll", "srl", "sra", "sllv", "srlv", "srav", "addu", "subu", "and", "or", "xor", "nor", "slt", "sltu",
    "daddu", "dsubu", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32", "dsllv", "dsrlv", "dsrav",
    "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui", "daddiu"}
SIMPLE = re.compile(r"([rf]\d+) = (.*);")
REG = re.compile(r"\b([rf]\d{1,2})\b")


def group_ok(n):
    i = n.extra.get("ins")
    if n.kind != "op" or i is None or i.mn not in GROUP_OK:
        return False
    return not i.defs or (len(i.defs) == 1 and SIMPLE.fullmatch(i.code) is not None)


def float_groups(nodes):
    """[start, end) node ranges: runs of group_ok nodes that nothing jumps
    into after their first node, with at least one flush-sensitive op."""
    targeted = set()
    for n in nodes:
        for t in n.targets:
            targeted.add(t if isinstance(t, str) else f"L_{t:x}")
    groups, k = [], 0
    while k < len(nodes):
        if not group_ok(nodes[k]):
            k += 1
            continue
        j = k + 1
        while j < len(nodes) and group_ok(nodes[j]) and nodes[j].label not in targeted:
            j += 1
        if any(nodes[x].extra["ins"].mn in FP_SENSITIVE for x in range(k, j)):
            groups.append((k, j))
        k = j
    return groups


def fast_expr(expr, exact):
    expr = (expr.replace("eg_fadd(", "EG_FADD_FAST(").replace("eg_fsub(", "EG_FSUB_FAST(")
            .replace("eg_fmul(", "EG_FMUL_FAST("))
    return expr.replace("EG_LD(", f"EG_LD_OR({exact}, ")


def emit_group(nodes, s, e, gid):
    """C++ for nodes[s:e] as a float group (labels: the caller emits the
    first node's)."""
    exact, done = f"G{gid}_x", f"G{gid}_e"
    cur, produced, handed_on, outs, fast = {}, set(), set(), [], []
    for x in range(s, e):
        i = nodes[x].extra["ins"]
        arith = i.mn in FP_ARITH
        for u in i.uses:
            name = cur.get(u)
            if name in produced and not arith:
                handed_on.add(name)
        rename = lambda text: REG.sub(lambda m: cur.get(m.group(1), m.group(1)), text)
        if i.defs:
            dst, expr = SIMPLE.fullmatch(i.code).groups()
            name = f"{dst}_{gid}_{x - s}"
            fast.append(f"{'float' if dst[0] == 'f' else 'uint64_t'} {name} = {fast_expr(rename(expr), exact)};")
            cur[dst] = name
            if arith:
                produced.add(name)
        elif i.mn in ("adda.s", "suba.s", "mula.s"):
            value = fast_expr(rename(i.code[len("ctx->f_acc = "):-1]), exact)
            fast.append(f"const float acc_{gid}_{x - s} = {value}; ctx->f_acc = acc_{gid}_{x - s};")
            outs.append(f"acc_{gid}_{x - s}")
        elif i.code:
            fast.append(fast_expr(rename(i.code), exact))
    final = set(cur.values())
    outs += sorted(n for n in produced if n in handed_on or n in final)
    terms = [f"fabsf({o})" for o in outs] or ["0.0f"]
    while len(terms) > 1:  # a balanced sum: short dependency chains
        terms = [f"({terms[k]} + {terms[k + 1]})" if k + 1 < len(terms) else terms[k] for k in range(0, len(terms), 2)]
    commit = " ".join(f"{r} = {n};" for r, n in sorted(cur.items()))
    counts = "".join(nodes[x].extra["cnt"] for x in range(s, e))
    lines = ["    { " + " ".join(fast) + f" const float k_ = {terms[0]}; "
             f"if (EG_LIKELY(eg_fp_ok(k_))) {{ {commit} {counts}EG_GROUP_FAST(); goto {done}; }} }}",
             f"{exact}: EG_GROUP_EXACT(); eg_exact_enter();"]
    for x in range(s, e):
        if x > s and nodes[x].label:
            lines.append(f"{nodes[x].label}: ;")
        if nodes[x].code:
            lines.append("    " + nodes[x].code)
    lines.append(f"    eg_exact_leave(); {done}: ;")
    return lines


def call_other(prog, fn):
    """A call into the runtime's code (PS2Recomp's functions, native stubs)
    runs with the FPU in PS2Recomp's mode."""
    if prog.fp_groups and not fn.startswith("EG_"):
        return f"eg_mode_ps2recomp(); {fn}(rdram, ctx, runtime); eg_mode_eegen();"
    return f"{fn}(rdram, ctx, runtime);"


def emit_function(prog, start, end):
    nodes = build_nodes(prog, start, end)
    din, lin, lout = dataflow(nodes)
    groups = {s: e for s, e in float_groups(nodes)} if prog.fp_groups else {}
    regs = set()
    temps = []
    callees = set()
    for n in nodes:
        regs |= n.uses | n.defs
        if "temp" in n.extra:
            temps.append(n.extra["temp"])
    body = []
    skip_to = 0
    for k, n in enumerate(nodes):
        if k < skip_to:
            continue
        line = []
        if n.label:
            body.append(f"{n.label}: ;")
        if k in groups:
            body.extend(emit_group(nodes, k, groups[k], k))
            skip_to = groups[k]
            continue
        if n.kind == "op":
            if n.code:
                line.append(n.code)
        elif n.kind == "cjump":
            t = n.targets[0]
            line.append(f"if ({n.code}) goto {t if isinstance(t, str) else f'L_{t:x}'};")
        elif n.kind == "jump":
            line.append(f"goto L_{n.targets[0]:x};")
        elif n.kind == "switch":
            cases = " ".join(f"case 0x{t:x}u: goto L_{t:x};" for t in n.targets[:-1])
            line.append(f"switch ({n.extra['value']}) {{ {cases} default: goto {n.extra['default']}; }}")
        elif n.kind == "sync":
            line.append(n.code + spill(din[k]))
            e = n.extra
            if e["call"] == "jal":
                t, ret = e["target"], e["ret"]
                fn = prog.callee(t)
                if fn:
                    callees.add(fn)
                    line.append(f"ctx->pc = 0x{t:x}u; {call_other(prog, fn)} "
                                f"if (EG_UNLIKELY(ctx->pc != 0x{ret:x}u)) {{ if (ctx->pc != 0x{t:x}u) return; ctx->pc = 0x{ret:x}u; }}")
                else:
                    line.append(f"if (!runtime->dispatchGuestBranch(rdram, ctx, 0x{t:x}u, 0x{e['addr']:x}u, 0x{ret:x}u, "
                                f"PS2Runtime::GuestBranchKind::DirectCall, \"JAL\")) return;")
            elif e["call"] == "jalr":
                line.append(f"ctx->pc = {e['target']}; if (!runtime->dispatchGuestBranch(rdram, ctx, {e['target']}, "
                            f"0x{e['addr']:x}u, 0x{e['ret']:x}u, PS2Runtime::GuestBranchKind::IndirectCall, \"JALR\")) return;")
            elif e["call"] == "syscall":
                line.append(f"ctx->pc = 0x{e['addr'] + 4:x}u; runtime->handleSyscall(rdram, ctx, 0x{e['code_no']:x}u);")
            else:
                line.append(f"ctx->pc = 0x{e['addr']:x}u; runtime->handleBreak(rdram, ctx);")
            line.append(reload(lout[k]))
        elif n.kind == "exit":
            line.append(spill(din[k]))
            e = n.extra
            if e["how"] == "return":
                line.append(f"ctx->pc = {e['target']}; return;")
            else:
                t = e["target"]
                fn = prog.callee(t) if t in prog.starts else None
                if fn:
                    callees.add(fn)
                    line.append(f"ctx->pc = 0x{t:x}u; {call_other(prog, fn)} return;")
                else:
                    line.append(f"ctx->pc = 0x{t:x}u; return;")
        text = " ".join(x for x in line if x)
        if text:
            body.append("    " + text)
    out = [f"// eegen: guest function 0x{start:08x} - 0x{end:08x}", '#include "eegen_rt.h"']
    if prog.fp_groups:
        out.append('#include "eegen_fp.h"')
    out.append("")
    for fn in sorted(callees):
        if fn != f"EG_{start:08x}":
            out.append(f"void {fn}(uint8_t *, R5900Context *, PS2Runtime *);")
    out.append("")
    out.append(f"void EG_{start:08x}(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)")
    out.append("{")
    gp = [r for r in reg_sort(regs) if r[0] == "r"]
    fp = [r for r in reg_sort(regs) if r[0] == "f"]
    if gp:
        out.append("    uint64_t " + ", ".join(gp) + ";")
    if fp:
        out.append("    float " + ", ".join(fp) + ";")
    for t in temps:
        out.append(f"    {t};")
    entry = reload(lin[0])
    if entry:
        out.append("    " + entry)
    out.extend(body)
    out.append("}")
    stats = {"nodes": len(nodes), "entry_loads": len(lin[0]),
             "syncs": sum(1 for n in nodes if n.kind == "sync")}
    return "\n".join(out) + "\n", stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("function_map")
    ap.add_argument("out_dir")
    ap.add_argument("--functions", required=True)
    ap.add_argument("--ps2recomp-table")
    ap.add_argument("--native")
    ap.add_argument("--skip", default="")
    ap.add_argument("--hardware-semantics", action="store_true")
    ap.add_argument("--no-float-groups", action="store_true")
    args = ap.parse_args()
    prog = Program(args)
    ends = dict(prog.funcs)
    wanted = []
    for line in Path(args.functions).read_text().split("\n"):
        if line.strip():
            wanted.append(int(line.split()[0], 16))
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("EG_*.cpp"):
        old.unlink()
    # first pass: which functions translate (calls between them are direct)
    refused = {}
    for a in wanted:
        if a not in ends:
            refused[a] = "not a function start"
            continue
        if a in prog.skip:
            refused[a] = "in the skip list"
            continue
        if a in prog.native:
            refused[a] = "native in the runtime"
            continue
        try:
            build_nodes(prog, a, ends[a])
            prog.ours.add(a)
        except Unsupported as e:
            refused[a] = str(e)
    total = collections.Counter()
    for a in sorted(prog.ours):
        text, stats = emit_function(prog, a, ends[a])
        (out / f"EG_{a:08x}.cpp").write_text(text, newline="\n")
        total.update(stats)
    with open(out / "eg_table.cpp", "w", newline="\n") as f:
        f.write('#include "eegen_rt.h"\n\n')
        for a in sorted(prog.ours):
            f.write(f"void EG_{a:08x}(uint8_t *, R5900Context *, PS2Runtime *);\n")
        f.write("\nconst EgFunction eg_functions[] = {\n")
        for a in sorted(prog.ours):
            f.write(f"    {{0x{a:x}u, EG_{a:08x}}},\n")
        f.write("    {0u, nullptr},\n};\n")
        f.write(f"const unsigned eg_function_count = {len(prog.ours)}u;\n")
        # the FPU mode the functions expect (eegen_fp.h)
        f.write(f"extern const bool eg_float_groups = {'true' if prog.fp_groups else 'false'};\n")
    reasons = collections.Counter(re.sub(r" at 0x[0-9a-f]+", "", r) for r in refused.values())
    with open(out / "eegen_report.txt", "w", newline="\n") as f:
        f.write(f"translated {len(prog.ours)} of {len(wanted)} functions"
                f" ({'hardware' if prog.hw else 'PS2Recomp'} semantics)\n")
        for a, r in sorted(refused.items()):
            f.write(f"refused 0x{a:08x}: {r}\n")
    print(f"translated {len(prog.ours)} of {len(wanted)} functions; {dict(total)}")
    if reasons:
        print("refused:", "; ".join(f"{r} x{n}" for r, n in reasons.most_common(12)))


if __name__ == "__main__":
    main()
