#!/usr/bin/env python3
"""Static analysis of a PS2 EE ELF for the Phase 1 feasibility study.

Reports, without running anything:
  - sections and segments
  - an R5900 instruction census (integer, 128-bit MMI, FPU, VU0 macro, ...)
  - an estimate of the function count (no symbols needed)
  - indirect control flow (jalr, jr <reg>, jump tables)
  - EE kernel syscalls used, with call counts
  - references to hardware register / VU memory / scratchpad address ranges
  - an inventory of embedded VU microcode (.vutext, .DVP.overlay.*)
  - notable strings (SDK library versions, file paths, toolchain marks)

Usage:
    python scripts/analyze_elf.py ELF [--out DIR]

Writes <DIR>/elf_report.json and <DIR>/strings.txt
(default DIR: private/analysis/<ELF file name>/).
"""

import argparse
import json
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# --------------------------------------------------------------------------
# ELF
# --------------------------------------------------------------------------

SHF_EXECINSTR = 0x4


def parse_elf(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit("not a 32-bit little-endian ELF")
    (e_type, e_machine, _, e_entry, e_phoff, e_shoff, e_flags, _,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = \
        struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    segments = []
    for i in range(e_phnum):
        p = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        segments.append({"type": p[0], "offset": p[1], "vaddr": p[2],
                         "filesz": p[4], "memsz": p[5], "flags": p[6]})
    shdrs = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize)
             for i in range(e_shnum)]
    strtab = b""
    if e_shstrndx < e_shnum:
        strtab = data[shdrs[e_shstrndx][4]:][:shdrs[e_shstrndx][5]]
    sections = []
    for sh in shdrs:
        name = strtab[sh[0]:strtab.find(b"\0", sh[0])].decode("ascii")
        sections.append({"name": name, "type": sh[1], "flags": sh[2],
                         "addr": sh[3], "offset": sh[4], "size": sh[5]})
    return {"entry": e_entry, "flags": e_flags, "machine": e_machine,
            "type": e_type, "segments": segments, "sections": sections}


def section_bytes(data, sec):
    if sec["type"] == 8:  # SHT_NOBITS
        return b""
    return data[sec["offset"]:sec["offset"] + sec["size"]]


def is_vu_section(name):
    return name.startswith(".vu") or name.startswith(".DVP")


# --------------------------------------------------------------------------
# R5900 instruction classification (enough for a census, not a disassembler)
# --------------------------------------------------------------------------

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv",
    0x07: "srav", 0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn",
    0x0C: "syscall", 0x0D: "break", 0x0F: "sync", 0x10: "mfhi",
    0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x14: "dsllv", 0x16: "dsrlv",
    0x17: "dsrav", 0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and",
    0x25: "or", 0x26: "xor", 0x27: "nor", 0x28: "mfsa", 0x29: "mtsa",
    0x2A: "slt", 0x2B: "sltu", 0x2C: "dadd", 0x2D: "daddu", 0x2E: "dsub",
    0x2F: "dsubu", 0x30: "tge", 0x31: "tgeu", 0x32: "tlt", 0x33: "tltu",
    0x34: "teq", 0x36: "tne", 0x38: "dsll", 0x3A: "dsrl", 0x3B: "dsra",
    0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32"}
REGIMM = {
    0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl", 0x08: "tgei",
    0x09: "tgeiu", 0x0A: "tlti", 0x0B: "tltiu", 0x0C: "teqi", 0x0E: "tnei",
    0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall",
    0x18: "mtsab", 0x19: "mtsah"}
OPCODES = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez",
    0x07: "bgtz", 0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu",
    0x0C: "andi", 0x0D: "ori", 0x0E: "xori", 0x0F: "lui", 0x14: "beql",
    0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl", 0x18: "daddi",
    0x19: "daddiu", 0x1A: "ldl", 0x1B: "ldr", 0x1E: "lq", 0x1F: "sq",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu",
    0x25: "lhu", 0x26: "lwr", 0x27: "lwu", 0x28: "sb", 0x29: "sh",
    0x2A: "swl", 0x2B: "sw", 0x2C: "sdl", 0x2D: "sdr", 0x2E: "swr",
    0x2F: "cache", 0x31: "lwc1", 0x33: "pref", 0x36: "lqc2", 0x37: "ld",
    0x39: "swc1", 0x3E: "sqc2", 0x3F: "sd"}
MMI = {
    0x00: "madd", 0x01: "maddu", 0x04: "plzcw", 0x10: "mfhi1",
    0x11: "mthi1", 0x12: "mflo1", 0x13: "mtlo1", 0x18: "mult1",
    0x19: "multu1", 0x1A: "div1", 0x1B: "divu1", 0x20: "madd1",
    0x21: "maddu1", 0x30: "pmfhl", 0x31: "pmthl", 0x34: "psllh",
    0x36: "psrlh", 0x37: "psrah", 0x3C: "psllw", 0x3E: "psrlw",
    0x3F: "psraw"}
MMI0 = ["paddw", "psubw", "pcgtw", "pmaxw", "paddh", "psubh", "pcgth",
        "pmaxh", "paddb", "psubb", "pcgtb", None, None, None, None, None,
        "paddsw", "psubsw", "pextlw", "ppacw", "paddsh", "psubsh", "pextlh",
        "ppach", "paddsb", "psubsb", "pextlb", "ppacb", None, None, "pext5",
        "ppac5"]
MMI1 = [None, "pabsw", "pceqw", "pminw", "padsbh", "pabsh", "pceqh",
        "pminh", None, None, "pceqb", None, None, None, None, None,
        "padduw", "psubuw", "pextuw", None, "padduh", "psubuh", "pextuh",
        None, "paddub", "psubub", "pextub", "qfsrv", None, None, None, None]
MMI2 = ["pmaddw", None, "psllvw", "psrlvw", "pmsubw", None, None, None,
        "pmfhi", "pmflo", "pinth", None, "pmultw", "pdivw", "pcpyld", None,
        "pmaddh", "phmadh", "pand", "pxor", "pmsubh", "phmsbh", None, None,
        None, None, "pexeh", "prevh", "pmulth", "pdivbw", "pexew", "prot3w"]
MMI3 = ["pmadduw", None, None, "psravw", None, None, None, None,
        "pmthi", "pmtlo", "pinteh", None, "pmultuw", "pdivuw", "pcpyud",
        None, None, None, "por", "pnor", None, None, None, None, None, None,
        "pexch", "pcpyh", None, None, "pexcw", None]
COP1_S = {0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s",
          0x04: "sqrt.s", 0x05: "abs.s", 0x06: "mov.s", 0x07: "neg.s",
          0x16: "rsqrt.s", 0x18: "adda.s", 0x19: "suba.s", 0x1A: "mula.s",
          0x1C: "madd.s", 0x1D: "msub.s", 0x1E: "madda.s", 0x1F: "msuba.s",
          0x24: "cvt.w.s", 0x28: "max.s", 0x29: "min.s", 0x30: "c.f.s",
          0x32: "c.eq.s", 0x34: "c.lt.s", 0x36: "c.le.s"}

CATEGORY = {}
for _m in ("sll", "srl", "sra", "sllv", "srlv", "srav", "movz", "movn",
           "mfhi", "mthi", "mflo", "mtlo", "add", "addu", "sub", "subu",
           "and", "or", "xor", "nor", "slt", "sltu", "addi", "addiu",
           "slti", "sltiu", "andi", "ori", "xori", "lui", "mfsa", "mtsa",
           "mtsab", "mtsah"):
    CATEGORY[_m] = "int_alu"
for _m in ("dsllv", "dsrlv", "dsrav", "dadd", "daddu", "dsub", "dsubu",
           "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32", "daddi",
           "daddiu"):
    CATEGORY[_m] = "int_alu_64"
for _m in ("mult", "multu", "div", "divu", "madd", "maddu", "mult1",
           "multu1", "div1", "divu1", "madd1", "maddu1", "mfhi1", "mthi1",
           "mflo1", "mtlo1", "plzcw"):
    CATEGORY[_m] = "mul_div"
for _m in ("lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "lwu", "sb", "sh",
           "swl", "sw", "sdl", "sdr", "swr", "ld", "sd", "ldl", "ldr"):
    CATEGORY[_m] = "load_store"
for _m in ("lq", "sq"):
    CATEGORY[_m] = "load_store_128"
for _m in ("beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl",
           "bltz", "bgez", "bltzl", "bgezl", "bltzal", "bgezal", "bltzall",
           "bgezall"):
    CATEGORY[_m] = "branch"
for _m in ("j", "jal", "jr", "jalr"):
    CATEGORY[_m] = "jump"
for _m in ("syscall", "break", "sync", "cache", "pref", "tge", "tgeu",
           "tlt", "tltu", "teq", "tne", "tgei", "tgeiu", "tlti", "tltiu",
           "teqi", "tnei"):
    CATEGORY[_m] = "system"


def decode(w):
    """Return (mnemonic, category) for one R5900 instruction word."""
    if w == 0:
        return "nop", "nop"
    op = w >> 26
    if op == 0x00:
        m = SPECIAL.get(w & 0x3F)
    elif op == 0x01:
        m = REGIMM.get((w >> 16) & 0x1F)
    elif op == 0x10:
        rs = (w >> 21) & 0x1F
        if rs == 0x10:
            m = {0x18: "eret", 0x38: "ei", 0x39: "di", 0x01: "tlbr",
                 0x02: "tlbwi", 0x06: "tlbwr", 0x08: "tlbp"}.get(w & 0x3F)
        else:
            m = {0: "mfc0", 4: "mtc0", 8: "bc0"}.get(rs)
        return (m or "cop0.?"), ("cop0" if m else "unknown")
    elif op == 0x11:
        rs = (w >> 21) & 0x1F
        if rs == 0x10:
            m = COP1_S.get(w & 0x3F)
        elif rs == 0x14:
            m = "cvt.s.w" if (w & 0x3F) == 0x20 else None
        else:
            m = {0: "mfc1", 2: "cfc1", 4: "mtc1", 6: "ctc1",
                 8: "bc1"}.get(rs)
        return (m or "cop1.?"), ("fpu" if m else "unknown")
    elif op == 0x12:
        rs = (w >> 21) & 0x1F
        if rs < 0x10:
            m = {1: "qmfc2", 2: "cfc2", 5: "qmtc2", 6: "ctc2",
                 8: "bc2"}.get(rs)
            return (m or "cop2.?"), ("vu0_transfer" if m else "unknown")
        f = w & 0x3F
        if f == 0x38:
            return "vcallms", "vu0_micro_call"
        if f == 0x39:
            return "vcallmsr", "vu0_micro_call"
        return "vu0_macro", "vu0_macro"
    elif op == 0x1C:
        f = w & 0x3F
        sub = (w >> 6) & 0x1F
        table = {0x08: MMI0, 0x28: MMI1, 0x09: MMI2, 0x29: MMI3}.get(f)
        m = table[sub] if table else MMI.get(f)
        if m is None:
            return "mmi.?", "unknown"
        if m in CATEGORY:
            return m, CATEGORY[m]
        return m, "mmi_128"
    else:
        m = OPCODES.get(op)
    if op in (0x31, 0x39):
        return m, "fpu"
    if op in (0x36, 0x3E):
        return m, "vu0_transfer"
    if m is None:
        return "unknown", "unknown"
    return m, CATEGORY.get(m, "other")


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def reg_rs(w):
    return (w >> 21) & 0x1F


def reg_rt(w):
    return (w >> 16) & 0x1F


JR_RA = 0x03E00008
SP, RA, V1, ZERO = 29, 31, 3, 0

# EE kernel syscall numbers (ps2sdk ee/kernel/include/syscallnr.h)
SYSCALLS = {
    1: "ResetEE", 2: "SetGsCrt", 4: "Exit", 5: "ResumeIntrDispatch",
    6: "LoadExecPS2", 7: "ExecPS2", 8: "ResumeT3IntrDispatch",
    0x0A: "AddSbusIntcHandler", 0x0B: "RemoveSbusIntcHandler",
    0x0C: "Interrupt2Iop", 0x0D: "SetVTLBRefillHandler",
    0x0E: "SetVCommonHandler", 0x0F: "SetVInterruptHandler",
    0x10: "AddIntcHandler", 0x11: "RemoveIntcHandler",
    0x12: "AddDmacHandler", 0x13: "RemoveDmacHandler",
    0x14: "_EnableIntc", 0x15: "_DisableIntc", 0x16: "_EnableDmac",
    0x17: "_DisableDmac", 0x18: "_SetAlarm", 0x19: "_ReleaseAlarm",
    0xFC: "SetAlarm", 0xFE: "ReleaseAlarm",
    -0x1A: "_iEnableIntc", -0x1B: "_iDisableIntc", -0x1C: "_iEnableDmac",
    -0x1D: "_iDisableDmac", -0x1E: "_iSetAlarm", -0x1F: "_iReleaseAlarm",
    -0xFD: "iSetAlarm", -0xFF: "iReleaseAlarm",
    0x20: "CreateThread", 0x21: "DeleteThread", 0x22: "StartThread",
    0x23: "ExitThread", 0x24: "ExitDeleteThread", 0x25: "TerminateThread",
    -0x26: "iTerminateThread", 0x27: "DisableDispatchThread",
    0x28: "EnableDispatchThread", 0x29: "ChangeThreadPriority",
    -0x2A: "iChangeThreadPriority", 0x2B: "RotateThreadReadyQueue",
    -0x2C: "iRotateThreadReadyQueue", 0x2D: "ReleaseWaitThread",
    -0x2E: "iReleaseWaitThread", 0x2F: "GetThreadId",
    -0x2F: "iGetThreadId", 0x30: "ReferThreadStatus",
    -0x31: "iReferThreadStatus", 0x32: "SleepThread", 0x33: "WakeupThread",
    -0x34: "iWakeupThread", 0x35: "CancelWakeupThread",
    -0x36: "iCancelWakeupThread", 0x37: "SuspendThread",
    -0x38: "iSuspendThread", 0x39: "ResumeThread", -0x3A: "iResumeThread",
    0x3C: "SetupThread", 0x3D: "SetupHeap", 0x3E: "EndOfHeap",
    0x40: "CreateSema", 0x41: "DeleteSema", 0x42: "SignalSema",
    -0x43: "iSignalSema", 0x44: "WaitSema", 0x45: "PollSema",
    -0x46: "iPollSema", 0x47: "ReferSemaStatus", -0x48: "iReferSemaStatus",
    -0x49: "iDeleteSema", 0x4A: "SetOsdConfigParam",
    0x4B: "GetOsdConfigParam", 0x4C: "GetGsHParam", 0x4D: "GetGsVParam",
    0x4E: "SetGsHParam", 0x4F: "SetGsVParam", 0x50: "CreateEventFlag",
    0x51: "DeleteEventFlag", 0x52: "SetEventFlag", 0x53: "iSetEventFlag",
    0x55: "PutTLBEntry", 0x56: "SetTLBEntry", 0x57: "GetTLBEntry",
    0x58: "ProbeTLBEntry", 0x59: "ExpandScratchPad", 0x5A: "Copy",
    0x5B: "GetEntryAddress", 0x5C: "EnableIntcHandler",
    -0x5C: "iEnableIntcHandler", 0x5D: "DisableIntcHandler",
    -0x5D: "iDisableIntcHandler", 0x5E: "EnableDmacHandler",
    -0x5E: "iEnableDmacHandler", 0x5F: "DisableDmacHandler",
    -0x5F: "iDisableDmacHandler", 0x60: "KSeg0", 0x61: "EnableCache",
    0x62: "DisableCache", 0x63: "GetCop0", 0x64: "FlushCache",
    0x66: "CpuConfig", -0x67: "iGetCop0", -0x68: "iFlushCache",
    -0x6A: "iCpuConfig", 0x6B: "SifStopDma", 0x6C: "SetCPUTimerHandler",
    0x6D: "SetCPUTimer", 0x6E: "SetOsdConfigParam2",
    0x6F: "GetOsdConfigParam2", 0x70: "GsGetIMR", -0x70: "iGsGetIMR",
    0x71: "GsPutIMR", -0x71: "iGsPutIMR", 0x72: "SetPgifHandler",
    0x73: "SetVSyncFlag", 0x74: "SetSyscall", 0x75: "_print",
    0x76: "SifDmaStat", -0x76: "iSifDmaStat", 0x77: "SifSetDma",
    -0x77: "iSifSetDma", 0x78: "SifSetDChain", -0x78: "iSifSetDChain",
    0x79: "SifSetReg", 0x7A: "SifGetReg", 0x7B: "ExecOSD",
    0x7C: "Deci2Call", 0x7D: "PSMode", 0x7E: "MachineType",
    0x7F: "GetMemorySize", 0x80: "_GetGsDxDyOffset", 0x82: "_InitTLB",
    0x83: "FindAddress", 0x85: "SetMemoryMode", 0x86: "GetMemoryMode",
    0x87: "ExecPSX"}

# lui immediates that point at interesting EE address ranges. Offsets are
# signed, so 0x1001 also reaches 0x10008000+ (DMAC) and 0x1101 VU1 memory.
HW_LUI = {
    0x1000: "EE HW: timers/IPU/GIF/VIF (0x1000xxxx)",
    0x1001: "EE HW: DMAC/INTC/SIF (0x1000[8-F]xxx..0x1001xxxx)",
    0x1100: "VU0/VU1 micro+data memory (0x1100xxxx)",
    0x1101: "VU1 data memory window (0x1100Cxxx..)",
    0x1200: "GS privileged regs (0x1200xxxx)",
    0x1201: "GS CSR/IMR (0x12001xxx)",
    0x7000: "scratchpad (0x7000xxxx)",
    0xBFC0: "BIOS ROM (0xBFC0xxxx)",
    0x2000: "uncached RAM mirror (0x2000xxxx)",
    0x3000: "uncached-accelerated RAM mirror (0x3000xxxx)",
}


# --------------------------------------------------------------------------
# Analyses
# --------------------------------------------------------------------------

def analyze_code(elf, data):
    code_secs = [s for s in elf["sections"]
                 if s["flags"] & SHF_EXECINSTR and not is_vu_section(s["name"])
                 and s["size"]]
    data_secs = [s for s in elf["sections"]
                 if s["type"] == 1 and not s["flags"] & SHF_EXECINSTR
                 and s["addr"] and s["size"] and not is_vu_section(s["name"])]
    ranges = [(s["addr"], s["addr"] + s["size"]) for s in code_secs]

    def in_code(a):
        return a & 3 == 0 and any(lo <= a < hi for lo, hi in ranges)

    words = {}  # addr -> word
    for s in code_secs:
        raw = section_bytes(data, s)
        for i in range(0, len(raw) - 3, 4):
            words[s["addr"] + i] = struct.unpack_from("<I", raw, i)[0]

    mnem, cats = Counter(), Counter()
    jal_targets, prologues = Counter(), set()
    jr_ra = jalr = jr_other = branch_likely = 0
    lui_hits = Counter()
    lui_code_refs = set()
    syscall_sites = []
    unknown_words = 0
    addrs = sorted(words)
    for idx, pc in enumerate(addrs):
        w = words[pc]
        m, cat = decode(w)
        mnem[m] += 1
        cats[cat] += 1
        if cat == "unknown":
            unknown_words += 1
        op = w >> 26
        if m == "jal":
            jal_targets[((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)] += 1
        elif m == "jr":
            if w == JR_RA:
                jr_ra += 1
            else:
                jr_other += 1
        elif m == "jalr":
            jalr += 1
        elif m in ("beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl",
                   "bltzall", "bgezall"):
            branch_likely += 1
        elif m == "syscall":
            num = None
            for back in range(1, 5):
                prev = words.get(pc - 4 * back)
                if prev is None:
                    break
                pop = prev >> 26
                if reg_rt(prev) == V1 and reg_rs(prev) == ZERO and \
                        pop in (0x09, 0x0D, 0x19):
                    imm = prev & 0xFFFF
                    num = imm if pop == 0x0D else sext16(imm)
                    break
            syscall_sites.append((pc, num))
        if op == 0x09 and reg_rs(w) == SP and reg_rt(w) == SP and \
                sext16(w & 0xFFFF) < 0:
            prologues.add(pc)
        if op == 0x0F:
            imm = w & 0xFFFF
            if imm in HW_LUI:
                lui_hits[imm] += 1
            # lui rX,hi ; addiu/ori rY,rX,lo within a few instructions
            rt = reg_rt(w)
            for fwd in range(1, 6):
                nxt = words.get(pc + 4 * fwd)
                if nxt is None:
                    break
                nop_ = nxt >> 26
                if nop_ in (0x09, 0x0D) and reg_rs(nxt) == rt:
                    lo = nxt & 0xFFFF
                    addr = ((imm << 16) + (lo if nop_ == 0x0D
                                            else sext16(lo))) & 0xFFFFFFFF
                    if in_code(addr):
                        lui_code_refs.add(addr)
                    break

    # code pointers stored in data sections
    data_ptrs = []  # (data addr, target)
    for s in data_secs:
        raw = section_bytes(data, s)
        for i in range(0, len(raw) - 3, 4):
            v = struct.unpack_from("<I", raw, i)[0]
            if in_code(v):
                data_ptrs.append((s["addr"] + i, v))

    def after_return(a):
        # the last non-nop words before `a` are `jr ra` + delay slot; nop
        # padding between functions is skipped (the delay slot may be a nop
        # too, hence both checks)
        q, pad = a - 4, 0
        while words.get(q) == 0 and pad < 8:
            q, pad = q - 4, pad + 1
        return words.get(q) == JR_RA or words.get(q - 4) == JR_RA

    def looks_like_entry(a):
        return a in jal_targets or a in prologues or after_return(a)

    ptr_targets = {t for _, t in data_ptrs}
    fn_ptr_targets = {t for t in ptr_targets | lui_code_refs
                      if looks_like_entry(t)}
    label_targets = (ptr_targets | lui_code_refs) - fn_ptr_targets

    # jump tables: runs of >= 3 consecutive data words that point at labels
    tables, run = [], []
    for a, t in data_ptrs + [(None, None)]:
        if t is not None and t in label_targets and \
                (not run or a == run[-1][0] + 4):
            run.append((a, t))
            continue
        if len(run) >= 3:
            tables.append({"addr": run[0][0], "entries": len(run)})
        run = [(a, t)] if t is not None and t in label_targets else []

    jal_in_code = {t for t in jal_targets if in_code(t)}
    starts_low = jal_in_code | fn_ptr_targets
    starts_mid = starts_low | prologues
    # conservative boundaries for a function map: a prologue only counts as
    # a start right after a return (`jr ra` + delay slot), never mid-body
    starts_map = starts_low | {p for p in prologues
                               if words.get(p - 8) == JR_RA}
    if in_code(elf["entry"]):
        starts_map.add(elf["entry"])
    code_bytes = sum(hi - lo for lo, hi in ranges)

    # syscall stubs: number -> (stub entry, how many jal call it)
    stubs = []
    for pc, num in syscall_sites:
        entry = next((pc - 4 * k for k in range(0, 4)
                      if pc - 4 * k in jal_targets), None)
        stubs.append({"addr": hex(pc), "number": num,
                      "name": SYSCALLS.get(num, "?") if num is not None
                      else "?",
                      "jal_callers": jal_targets.get(entry, 0) if entry
                      else 0})
    stubs.sort(key=lambda s: (s["number"] is None, s["number"] or 0))

    return {
        "_function_map": {"starts": sorted(starts_map), "ranges": ranges},
        "code_sections": [s["name"] for s in code_secs],
        "code_bytes": code_bytes,
        "instructions": len(words),
        "categories": dict(cats.most_common()),
        "top_mnemonics": dict(mnem.most_common(60)),
        "unknown_words": unknown_words,
        "functions": {
            "jal_targets": len(jal_in_code),
            "prologue_sites": len(prologues),
            "fn_pointer_targets": len(fn_ptr_targets),
            "estimate_low": len(starts_low),
            "estimate_mid": len(starts_mid),
            "jr_ra_count": jr_ra,
            "avg_bytes_per_function_mid":
                round(code_bytes / max(1, len(starts_mid))),
        },
        "control_flow": {
            "jal_sites": sum(jal_targets.values()),
            "jalr_sites": jalr,
            "jr_non_ra_sites": jr_other,
            "branch_likely": branch_likely,
            "code_pointers_in_data": len(data_ptrs),
            "code_refs_via_lui": len(lui_code_refs),
            "label_targets": len(label_targets),
            "jump_tables": len(tables),
            "jump_table_entries": sum(t["entries"] for t in tables),
        },
        "syscall_stubs": stubs,
        "hw_lui_refs": {f"0x{k:04X} {HW_LUI[k]}": v
                        for k, v in sorted(lui_hits.items())},
    }


def _vu_stats(raw):
    n = len(raw) // 8
    ebits = xgkick = 0
    for i in range(n):
        lower, upper = struct.unpack_from("<II", raw, 8 * i)
        if upper & (1 << 30):
            ebits += 1
        if not upper & (1 << 31) and (lower & 0xFE0007FF) == 0x800006FC:
            xgkick += 1
    return {"vu_instructions": n, "end_bits": ebits, "xgkick": xgkick}


def analyze_vu(elf, data):
    """VU microcode inventory.

    .vutext is the load image inside the EE segment. .DVP.ovlytab holds
    (name offset, EE load address, VU micro-memory address) per overlay; the
    .DVP.overlay.* sections themselves are zero-filled placeholders that
    only carry each overlay's size.
    """
    secs = {s["name"]: s for s in elf["sections"]}
    vutext = secs.get(".vutext")
    if not vutext or not vutext["size"]:
        return {"vutext": None, "overlays": []}
    vt = section_bytes(data, vutext)
    result = {"vutext": {"addr": hex(vutext["addr"]),
                         "bytes": vutext["size"], **_vu_stats(vt)},
              "overlays": []}
    tab, names = secs.get(".DVP.ovlytab"), secs.get(".DVP.ovlystrtab")
    if not tab or not names:
        return result
    tab, names = section_bytes(data, tab), section_bytes(data, names)
    for i in range(0, len(tab) - 11, 12):
        name_off, lma, vma = struct.unpack_from("<III", tab, i)
        name = names[name_off:names.find(b"\0", name_off)].decode("ascii")
        size = secs[name]["size"] if name in secs else 0
        start = lma - vutext["addr"]
        result["overlays"].append({
            "name": name, "ee_addr": hex(lma), "vu_addr": hex(vma),
            "bytes": size, **_vu_stats(vt[start:start + size])})
    return result


STRING_RE = re.compile(rb"[\x20-\x7e]{5,}")
STRING_CLASSES = {
    "sdk_versions": re.compile(r"PsII\w*\s+\d{3,4}"),
    "device_paths": re.compile(r"(cdrom0?|host\d?|rom0|mc\d|pfs\d|hdd\d)"
                               r":", re.I),
    "module_names": re.compile(r"\.(irx|img|elf)\b", re.I),
    "data_files": re.compile(r"\.(hog|pss|vag|vb|bd|hd|sq|adp|str)\b",
                             re.I),
    "source_files": re.compile(r"[\w/\\]+\.(c|cc|cpp|h|hpp|s|dsm|vsm)\b"),
    "toolchain": re.compile(r"GCC|SN Systems|ProDG|Metrowerks|CodeWarrior|"
                            r"snlib|libsn", re.I),
    "middleware": re.compile(r"RenderWare|Criterion|Havok|Miles|Bink|"
                             r"Warthog|KnowWonder|Electronic Arts|\bEA\b",
                             re.I),
    "sce_symbols": re.compile(r"\bsce[A-Z]\w+"),
    "cpp_hints": re.compile(r"__vt_|__tf|__ti|_GLOBAL_\$|pure virtual|"
                            r"__cxa|type_info|bad_alloc", re.I),
}


def analyze_strings(elf, data):
    found = []
    for s in elf["sections"]:
        if s["type"] != 1 or s["flags"] & SHF_EXECINSTR or not s["size"]:
            continue
        if is_vu_section(s["name"]):
            continue
        raw = section_bytes(data, s)
        for m in STRING_RE.finditer(raw):
            found.append((s["addr"] + m.start(), m.group().decode("ascii")))
    classes = {}
    for key, rx in STRING_CLASSES.items():
        hits = sorted({t for _, t in found if rx.search(t)})
        classes[key] = {"count": len(hits), "samples": hits[:40]}
    return found, classes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    data = args.elf.read_bytes()
    elf = parse_elf(data)
    out = args.out or REPO_ROOT / "private" / "analysis" / args.elf.name
    out.mkdir(parents=True, exist_ok=True)

    code = analyze_code(elf, data)
    code.pop("_function_map")  # internal; see export_function_map.py
    vu = analyze_vu(elf, data)
    strings, string_classes = analyze_strings(elf, data)
    report = {
        "elf": args.elf.name,
        "bytes": len(data),
        "entry": hex(elf["entry"]),
        "e_flags": hex(elf["flags"]),
        "segments": [{**s, "vaddr": hex(s["vaddr"])}
                     for s in elf["segments"]],
        "sections": [{"name": s["name"], "addr": hex(s["addr"]),
                      "size": s["size"], "flags": hex(s["flags"]),
                      "nobits": s["type"] == 8}
                     for s in elf["sections"] if s["name"]],
        "symbols_present": any(s["type"] in (2, 11)
                               for s in elf["sections"]),
        "code": code,
        "vu_microcode": vu,
        "strings": string_classes,
    }
    (out / "elf_report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         newline="\n")
    with open(out / "strings.txt", "w", newline="\n") as f:
        for addr, text in strings:
            f.write(f"{addr:08x}  {text}\n")

    f_ = code["functions"]
    print(f"{args.elf.name}: {code['instructions']} EE instructions "
          f"({code['code_bytes'] / 1024:.0f} KiB code)")
    print(f"  functions: low {f_['estimate_low']}, mid {f_['estimate_mid']} "
          f"(jal targets {f_['jal_targets']}, prologues "
          f"{f_['prologue_sites']}, jr ra {f_['jr_ra_count']})")
    cf = code["control_flow"]
    print(f"  indirect: jalr {cf['jalr_sites']}, jr reg "
          f"{cf['jr_non_ra_sites']}, jump tables {cf['jump_tables']} "
          f"({cf['jump_table_entries']} entries)")
    print(f"  categories: {code['categories']}")
    called = Counter()
    for s in code["syscall_stubs"]:
        called[s["name"]] += s["jal_callers"]
    print(f"  syscall stubs: {len(code['syscall_stubs'])} linked, "
          f"{sum(1 for v in called.values() if v)} called via jal: " +
          ", ".join(f"{k} x{v}" for k, v in called.most_common() if v))
    if vu["vutext"]:
        print(f"  VU microcode: {vu['vutext']['vu_instructions']} instr, "
              f"{len(vu['overlays'])} overlays, "
              f"{vu['vutext']['xgkick']} xgkick, "
              f"{vu['vutext']['end_bits']} end bits")
    print(f"  output: {out}")


if __name__ == "__main__":
    main()
