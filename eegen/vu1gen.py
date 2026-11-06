#!/usr/bin/env python3
"""vu1gen: a whole VU1 microprogram -> one C++ function (host vector code).

    python3 -I eegen/vu1gen.py MICRO.bin OUT.inc [--entries 0x0,0x15,...] [--report]

MICRO.bin is a dump of VU1 micro memory (game data, stays in private/), and so is OUT.inc, which
vu1/vu1_native.cpp includes. Without --entries, every entry stub at the start of micro memory is used.

Semantics follow PS2Recomp's VU interpreter, so the two can be compared byte for byte (vu1play --mode check);
see eegen/eegen_vu0.h for how the host float modes give those semantics.

The interpreter models the pipelines cycle by cycle. A read of a VF or VI register with a write in flight stalls
until the write lands, so those registers can simply be sequential. What doesn't stall is what timing can change:
Q, P, the MAC and clip flags, and a conditional branch right after an integer write, which reads the old value.
So this walks every path from every entry with the interpreter's own timing rules, keeping the set of pipeline
states each pair can see, and decides each such read at translation time: which value it gets is then a constant.
A read whose answer differs between states is refused, with the pair named. XGKICK's stall depends on the packet
and is unknown here, so every length is followed.

Refused, so far unseen in this game: a branch in a delay slot or after an E-bit, an E-bit in a delay slot, a
JR through a register whose value isn't a constant on every path, and D or T bits.
"""
import argparse
import collections
import struct
import sys

LANE = (8, 4, 2, 1)  # component -> bit of a dest field
KICK_PIPE, FMAC, LSU, IALU, BRANCH, FDIV, EFU = range(1, 8)


class Refuse(Exception):
    pass


def comps(mask):
    return [c for c in range(4) if mask & LANE[c]]


def signed11(v):
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


class Usage:
    __slots__ = ("vf_reads", "vf_write", "vi_reads", "vi_write", "latency", "vi_latency", "delays", "pipe",
                 "wait_q", "wait_p", "mac", "clip", "q_read", "acc_read")

    def __init__(self):
        self.vf_reads = []
        self.vf_write = None
        self.vi_reads = []
        self.vi_write = 0
        self.latency = 0
        self.vi_latency = 0
        self.delays = False
        self.pipe = 0
        self.wait_q = self.wait_p = False
        self.mac = False
        self.clip = False
        self.q_read = False
        self.acc_read = False

    def read(self, reg, mask):
        if reg and mask:
            self.vf_reads.append((reg, mask))

    def write(self, reg, mask):
        if reg and mask:
            self.vf_write = (reg, mask)

    def ri(self, reg):
        if reg:
            self.vi_reads.append(reg)

    def wi(self, reg):
        if reg:
            self.vi_write = reg


def upper_usage(u):
    r = Usage()
    r.pipe = FMAC
    r.latency = 4
    op = u & 0x3F
    dest, ft, fs, fd = u >> 21 & 0xF, u >> 16 & 31, u >> 11 & 31, u >> 6 & 31
    if op <= 0x2F:
        r.read(fs, dest)
        r.write(fd, dest)
        if op <= 0x1B:
            r.read(ft, LANE[op & 3])
        elif op >= 0x28:
            r.read(ft, 0xE if op == 0x2E else dest)
        r.q_read = op in (0x1C, 0x20, 0x21, 0x24, 0x25)
        r.mac = bool(dest) and not (0x10 <= op <= 0x17 or op in (0x1D, 0x1F, 0x2B, 0x2F))
        return r
    if op < 0x3C:
        raise Refuse(f"upper op {op:#x}")
    k = (u & 3) | (u >> 4 & 0x7C)
    if k <= 0x0F or 0x18 <= k <= 0x1C or k == 0x1E or 0x20 <= k <= 0x2A or 0x2C <= k <= 0x2E:
        r.read(fs, dest)
        if k <= 0x1B:
            r.read(ft, LANE[k & 3])
        elif 0x28 <= k <= 0x2E:
            r.read(ft, 0xE if k == 0x2E else dest)
        r.q_read = k in (0x1C, 0x20, 0x21, 0x24, 0x25)
        r.mac = bool(dest)
    elif 0x10 <= k <= 0x17 or k == 0x1D:
        r.read(fs, dest)
        r.write(ft, dest)
    elif k == 0x1F:
        r.read(fs, 0xE)
        r.read(ft, 1)
        r.clip = True
    elif k not in (0x2F, 0x30):
        raise Refuse(f"upper special {k:#x}")
    return r


def lower_usage(l):
    r = Usage()
    if l in (0, 0x8000033C):
        return r
    op = l >> 25
    vft, vfs = l >> 16 & 31, l >> 11 & 31
    vit, vis, vid = l >> 16 & 15, l >> 11 & 15, l >> 6 & 15
    dest = l >> 21 & 0xF
    if op == 0x00:
        r.pipe, r.latency = LSU, 4
        r.ri(vis)
        r.write(vft, dest)
    elif op == 0x01:
        r.pipe, r.latency = LSU, 1
        r.ri(vit)
        r.read(vfs, dest)
    elif op == 0x04:
        r.pipe, r.latency = LSU, 4
        r.ri(vis)
        r.wi(vit)
    elif op == 0x05:
        r.pipe, r.latency = LSU, 1
        r.ri(vis)
        r.ri(vit)
    elif op in (0x08, 0x09):
        r.pipe, r.latency, r.delays = IALU, 1, True
        r.ri(vis)
        r.wi(vit)
    elif op in (0x10, 0x12, 0x13):
        r.pipe, r.latency = IALU, 1
        r.wi(1)
    elif op == 0x11:
        r.pipe, r.latency = FMAC, 4
    elif op in (0x14, 0x16, 0x17):
        r.pipe, r.latency = IALU, 1
        r.wi(vit)
    elif op == 0x15:
        r.pipe, r.latency = FMAC, 4
    elif op in (0x18, 0x1A, 0x1B):
        r.pipe, r.latency = IALU, 1
        r.ri(vis)
        r.wi(vit)
    elif op == 0x1C:
        r.pipe, r.latency = IALU, 1
        r.wi(vit)
    elif op == 0x20:
        r.pipe = BRANCH
    elif op == 0x21:
        r.pipe, r.latency = BRANCH, 1
        r.wi(vit)
    elif op == 0x24:
        r.pipe = BRANCH
        r.ri(vis)
    elif op == 0x25:
        r.pipe, r.latency = BRANCH, 1
        r.ri(vis)
        r.wi(vit)
    elif op in (0x28, 0x29):
        r.pipe = BRANCH
        r.ri(vis)
        r.ri(vit)
    elif 0x2C <= op <= 0x2F:
        r.pipe = BRANCH
        r.ri(vis)
    elif op == 0x40:
        f = l & 0x3F
        if f in (0x30, 0x31, 0x34, 0x35):
            r.pipe, r.latency, r.delays = IALU, 1, True
            r.ri(vis)
            r.ri(vit)
            r.wi(vid)
        elif f == 0x32:
            r.pipe, r.latency, r.delays = IALU, 1, True
            r.ri(vis)
            r.wi(vit)
        elif f < 0x3C:
            raise Refuse(f"lower funct {f:#x}")
        else:
            k = (l & 3) | (l >> 4 & 0x7C)
            lane_s, lane_t = LANE[l >> 21 & 3], LANE[l >> 23 & 3]
            if k in (0x30, 0x31):
                r.pipe, r.latency = FMAC, 4
                r.read(vfs, 0xF if k == 0x31 else dest)
                r.write(vft, dest)
            elif k in (0x34, 0x36):
                r.pipe, r.latency, r.vi_latency, r.delays = LSU, 4, 1, True
                r.ri(vis)
                r.wi(vis)
                r.write(vft, dest)
            elif k in (0x35, 0x37):
                r.pipe, r.latency, r.delays = LSU, 1, True
                r.ri(vit)
                r.wi(vit)
                r.read(vfs, dest)
            elif k == 0x38:
                r.pipe, r.latency = FDIV, 7
                r.read(vfs, lane_s)
                r.read(vft, lane_t)
            elif k == 0x39:
                r.pipe, r.latency = FDIV, 7
                r.read(vft, lane_t)
            elif k == 0x3A:
                r.pipe, r.latency = FDIV, 13
                r.read(vfs, lane_s)
                r.read(vft, lane_t)
            elif k == 0x3B:
                r.pipe, r.wait_q = FDIV, True
            elif k == 0x3C:
                r.pipe, r.latency, r.delays = IALU, 1, True
                r.read(vfs, lane_s)
                r.wi(vit)
            elif k == 0x3D:
                r.pipe, r.latency = FMAC, 4
                r.ri(vis)
                r.write(vft, dest)
            elif k == 0x3E:
                r.pipe, r.latency = LSU, 4
                r.ri(vis)
                r.wi(vit)
            elif k == 0x3F:
                r.pipe, r.latency = LSU, 1
                r.ri(vis)
                r.ri(vit)
            elif k == 0x64:
                r.pipe, r.latency = FMAC, 4
                r.write(vft, dest)
            elif k in (0x68, 0x69):
                r.pipe, r.latency = IALU, 1
                r.wi(vit)
            elif k == 0x6C:
                r.pipe, r.latency = KICK_PIPE, 2
                r.ri(vis)
            elif k in (0x70, 0x71, 0x72, 0x73):
                r.pipe, r.latency = EFU, {0x70: 11, 0x71: 18, 0x72: 18, 0x73: 24}[k]
                r.read(vfs, 0xE)
            elif k == 0x76:
                r.pipe, r.latency = EFU, 12
                r.read(vfs, 0xF)
            elif k == 0x78 or k == 0x7A:
                r.pipe, r.latency = EFU, 12
                r.read(vfs, lane_s)
            elif k == 0x7B:
                r.pipe, r.wait_p = EFU, True
            else:
                raise Refuse(f"lower special {k:#x}")
    else:
        raise Refuse(f"lower op {op:#x}")
    return r


class Pair:
    __slots__ = ("pc", "l", "u", "ibit", "ebit", "uu", "lu", "suppress", "branch", "target", "cond_regs", "op")

    def __init__(self, code, pc):
        if pc * 8 + 8 > len(code):
            raise Refuse(f"pair {pc:#x} is past the end")
        self.pc = pc
        self.l, self.u = struct.unpack_from("<II", code, pc * 8)
        self.ibit = bool(self.u >> 31 & 1)
        self.ebit = bool(self.u >> 30 & 1)
        if self.u >> 27 & 3:
            raise Refuse(f"pair {pc:#x}: D or T bit")
        self.uu = upper_usage(self.u)
        self.lu = lower_usage(self.l) if not self.ibit else Usage()
        uw, lw = self.uu.vf_write, self.lu.vf_write
        self.suppress = bool(uw and lw and uw[0] == lw[0])
        self.branch = None
        self.target = None
        self.cond_regs = ()
        self.op = -1 if self.ibit or self.l in (0, 0x8000033C) else self.l >> 25
        if self.op in (0x20, 0x21):
            self.branch = "always"
            self.target = (pc + 1 + signed11(self.l)) & 0x7FF
        elif self.op in (0x24, 0x25):
            self.branch = "jr"
        elif self.op in (0x28, 0x29):
            self.branch = "cond"
            self.target = (pc + 1 + signed11(self.l)) & 0x7FF
            self.cond_regs = (self.l >> 11 & 15, self.l >> 16 & 15)
        elif self.op is not None and 0x2C <= self.op <= 0x2F:
            self.branch = "cond"
            self.target = (pc + 1 + signed11(self.l)) & 0x7FF
            self.cond_regs = (self.l >> 11 & 15,)


# --- pipeline states -----------------------------------------------------------------------------------------
# A state holds what's in flight, as cycles left: vf ((reg, component), left), vi (reg, left) for loads, fdiv left,
# efu lefts, EFU resource left, flags (left, kind, pc), the pair whose MAC flags are the committed ones (-1: from
# before this call), the (reg, pc) of a write the next branch must not see, known link values, kick maybe running.
State = collections.namedtuple("State", "vf vi fdiv efu efu_res flags mac prev links kick")


def advance(s, n):
    if n <= 0:
        return s
    mac = s.mac
    flags = []
    for left, kind, pc in s.flags:
        if left <= n:
            if kind == "M":
                mac = pc
        else:
            flags.append((left - n, kind, pc))
    return s._replace(
        vf=tuple((k, left - n) for k, left in s.vf if left > n),
        vi=tuple((k, left - n) for k, left in s.vi if left > n),
        fdiv=max(0, s.fdiv - n),
        efu=tuple(left - n for left in s.efu if left > n),
        efu_res=max(0, s.efu_res - n),
        flags=tuple(flags),
        mac=mac)


def stall_of(s, p):
    ready = 0
    vf = dict(s.vf)
    vi = dict(s.vi)
    for usage in (p.uu, p.lu):
        for reg, mask in usage.vf_reads:
            for c in comps(mask):
                ready = max(ready, vf.get((reg, c), 0))
        for reg in usage.vi_reads:
            ready = max(ready, vi.get(reg, 0))
    lu = p.lu
    if lu.pipe == FDIV and s.fdiv:
        ready = max(ready, s.fdiv)
    if lu.pipe == EFU:
        ready = max(ready, s.efu_res)
    if lu.wait_p:
        for left in s.efu:
            ready = max(ready, left)
    return ready


class Facts:
    def __init__(self):
        self.q = collections.defaultdict(set)      # pc -> pending divisions when it reads Q
        self.p = collections.defaultdict(set)      # pc -> pending EFU results when it reads P
        self.mac = collections.defaultdict(set)    # pc -> pairs whose MAC flags it can read
        self.clip = collections.defaultdict(set)   # pc -> pending clip entries when it reads clip
        self.old = collections.defaultdict(set)    # pc -> branch registers it reads the old value of (0: none)
        self.backup = set()                        # pairs saving the old value of what they write
        self.jr = collections.defaultdict(set)     # pc -> targets
        self.normal = set()
        self.slot = set()


def jr_registers(code):
    regs = set()
    for pc in range(len(code) // 8):
        l, u = struct.unpack_from("<II", code, pc * 8)
        if not u >> 31 & 1 and l >> 25 in (0x24, 0x25):
            regs.add(l >> 11 & 15)
    return regs


def analyze(code, entries):
    pairs = {}
    facts = Facts()
    tracked = jr_registers(code)

    def pair(pc):
        p = pairs.get(pc)
        if p is None:
            p = pairs[pc] = Pair(code, pc)
        return p

    start = State((), (), 0, (), 0, (), -1, None, (), False)
    work = [(e, None, start) for e in entries]
    seen = set(work)
    facts.normal.update(entries)

    def push(pc, ctx, s):
        node = (pc, ctx, s)
        if node not in seen:
            seen.add(node)
            work.append(node)
            if len(seen) > 3000000:
                raise Refuse("too many pipeline states")
            if ctx is None:
                facts.normal.add(pc)
            else:
                facts.slot.add(pc)

    while work:
        pc, ctx, s0 = work.pop()
        p = pair(pc)
        is_kick = p.lu.pipe == KICK_PIPE
        if ctx is not None and p.branch:
            raise Refuse(f"pair {pc:#x}: a branch in a delay slot or after an E-bit")
        if ctx is not None and ctx[0] == "delay" and p.ebit:
            raise Refuse(f"pair {pc:#x}: an E-bit in a delay slot")
        # XGKICK waits for a running kick, however long: follow every length
        forks = [s0]
        if is_kick and s0.kick:
            longest = max([left for _, left in s0.vf] + [left for _, left in s0.vi] + [s0.fdiv, s0.efu_res] +
                          list(s0.efu) + [e[0] for e in s0.flags] + [0])
            forks = [advance(s0, n) for n in range(longest + 1)]
        for s in forks:
            s = advance(s, stall_of(s, p))
            # --- what this pair reads that doesn't stall
            if p.uu.q_read:
                facts.q[pc].add(1 if s.fdiv else 0)
            if p.op == 0x40 and (p.l & 3 | p.l >> 4 & 0x7C) == 0x64 and p.l & 0x3F >= 0x3C:
                facts.p[pc].add(len(s.efu))
            if p.op in (0x18, 0x1A, 0x1B):
                facts.mac[pc].add(s.mac)
            if p.op in (0x10, 0x12, 0x13, 0x1C):
                facts.clip[pc].add(tuple(kind for _, kind, _ in s.flags if kind in "CF"))
            if p.cond_regs:
                old = s.prev[0] if s.prev and s.prev[0] in p.cond_regs else 0
                facts.old[pc].add(old)
                if old:
                    facts.backup.add(s.prev[1])
            # --- its effects
            vf = dict(s.vf)
            vi = dict(s.vi)
            uw, lw = p.uu.vf_write, p.lu.vf_write
            if lw and not p.suppress:
                for c in comps(lw[1]):
                    vf[(lw[0], c)] = p.lu.latency
            if uw:
                for c in comps(uw[1]):
                    vf[(uw[0], c)] = p.uu.latency
            prev = None
            links = dict(s.links)
            if p.lu.vi_write:
                reg = p.lu.vi_write
                left = p.lu.vi_latency or p.lu.latency
                if left > 1:
                    vi[reg] = left
                else:
                    vi.pop(reg, None)
                if p.lu.delays:
                    prev = (reg, pc)
                if reg in tracked:
                    value = None
                    vis = p.l >> 11 & 15
                    imm = (p.l >> 21 & 0xF) << 11 | p.l & 0x7FF
                    base = 0 if vis == 0 else links.get(vis)
                    if p.op == 0x08 and base is not None:
                        value = base + imm
                    elif p.op == 0x09 and base is not None:
                        value = base - imm
                    elif p.op in (0x21, 0x25):
                        value = pc + 2
                    if value is None:
                        links.pop(reg, None)
                    else:
                        links[reg] = value
            flags = list(s.flags)
            if p.uu.mac:
                flags.append((4, "M", pc))
            if p.uu.clip:
                flags.append((4, "C", pc))
            if p.op == 0x11:
                flags = [e for e in flags if not (e[0] == 4 and e[1] == "C")]
                flags.append((4, "F", pc))
            fdiv = p.lu.latency if p.lu.pipe == FDIV and not p.lu.wait_q else s.fdiv
            efu, efu_res = s.efu, s.efu_res
            if p.lu.pipe == EFU and not p.lu.wait_p:
                efu = efu + (p.lu.latency,)
                efu_res = p.lu.latency - 1
            s2 = State(tuple(sorted(vf.items())), tuple(sorted(vi.items())), fdiv, efu, efu_res, tuple(flags), s.mac, prev,
                       tuple(sorted(links.items())), s.kick or is_kick)
            s2 = advance(s2, 1)
            # --- where next
            if ctx is not None:
                if ctx[0] == "delay":
                    push(ctx[1], None, s2)
                # an E-bit's next pair ends the program
                continue
            if p.ebit:
                if p.branch:
                    raise Refuse(f"pair {pc:#x}: a branch with an E-bit")
                push((pc + 1) & 0x7FF, ("eslot",), s2)
                continue
            if p.branch == "always":
                push((pc + 1) & 0x7FF, ("delay", p.target), s2)
            elif p.branch == "cond":
                push((pc + 1) & 0x7FF, ("delay", p.target), s2)
                push((pc + 1) & 0x7FF, ("delay", (pc + 2) & 0x7FF), s2)
            elif p.branch == "jr":
                vis = p.l >> 11 & 15
                target = dict(s.links).get(vis)
                if target is None:
                    raise Refuse(f"pair {pc:#x}: JR vi{vis} isn't a constant here")
                target &= 0x7FF
                facts.jr[pc].add(target)
                push((pc + 1) & 0x7FF, ("delay", target), s2)
            else:
                push((pc + 1) & 0x7FF, None, s2)
    return pairs, facts, len(seen)


# --- C++ -----------------------------------------------------------------------------------------------------
def float_literal(bits):
    """The normalized value of a 32-bit float, as an exact hex literal."""
    exp = bits >> 23 & 0xFF
    if exp == 0:
        bits &= 0x80000000
    elif exp == 0xFF:
        bits = bits & 0x80000000 | 0x7F7FFFFF
    value = struct.unpack("<f", struct.pack("<I", bits))[0]
    return f"{value.hex()}f"


class Emit:
    def __init__(self, pairs, facts, trace=False, count=False):
        self.count = count
        self.pairs = pairs
        self.facts = facts
        self.temp = 0
        self.trace = trace
        self.notes = []
        self.use_q_old = any(1 in v for v in facts.q.values())
        self.use_p_old = any(1 in v for v in facts.p.values())
        self.mac_vars = set()
        self.mac_in = False
        for pc, providers in facts.mac.items():
            if len(providers) != 1:
                raise Refuse(f"pair {pc:#x}: FMEQ can see the flags of pairs {sorted(hex(x) for x in providers)}")
            for x in providers:
                if x == -1:
                    self.mac_in = True
                else:
                    self.mac_vars.add(x)
        for pc, v in facts.q.items():
            if len(v) != 1:
                raise Refuse(f"pair {pc:#x}: its Q read depends on the path")
        for pc, v in facts.p.items():
            if len(v) != 1 or max(v) > 1:
                raise Refuse(f"pair {pc:#x}: its P read depends on the path, or two results are in flight")
        for pc, v in facts.clip.items():
            if len({self.clip_value(pc, kinds) for kinds in v}) != 1:
                raise Refuse(f"pair {pc:#x}: its clip read depends on the path")
        for pc, v in facts.old.items():
            if len(v) != 1:
                raise Refuse(f"pair {pc:#x}: this branch reads an old register on some paths only")

    @staticmethod
    def clip_value(pc, kinds):
        """The committed clip flags, given the entries in flight, oldest first. clip holds the newest, with older
        CLIPs' results further up; clip_f what it held when the last FCSET came."""
        if kinds.count("F") > 1:
            raise Refuse(f"pair {pc:#x}: reads the clip flag with two FCSETs in flight")
        if "F" in kinds:
            before = kinds.index("F")
            return f"(uint32_t)(clip_f >> {6 * before})"
        return f"(uint32_t)(clip >> {6 * len(kinds)})"

    def new_temp(self):
        self.temp += 1
        return f"t{self.temp}"

    def vf(self, n):
        return "eg_vf0" if n == 0 else f"vf{n}"

    def vi(self, n):
        return "0" if n == 0 else f"vi{n}"

    def assign(self, target, value, dest):
        if dest == 0xF:
            return f"{target} = {value};"
        m = ", ".join(str(4 + c if dest & LANE[c] else c) for c in range(4))
        return f"{target} = __builtin_shuffle({target}, {value}, eg_vi4{{{m}}});"

    # --- upper ---------------------------------------------------------------------------------------------
    def second_operand(self, kind, ft, pc):
        if kind == "vec":
            return self.vf(ft)
        if kind == "q":
            return f"EG_VU_ALL({self.q_read(pc)})"
        if kind == "i":
            return "EG_VU_ALL(vu_i)"
        return f"EG_VU_BC({self.vf(ft)}, {kind})"

    def q_read(self, pc):
        return "q_old" if 1 in self.facts.q[pc] else "q"

    def upper(self, p):
        """Returns (statement computing a temp, commit statement), each possibly None."""
        u = p.u
        op = u & 0x3F
        dest, ft, fs, fd = u >> 21 & 0xF, u >> 16 & 31, u >> 11 & 31, u >> 6 & 31
        arith = {0: "add", 4: "sub", 8: "madd", 0xC: "msub", 0x18: "mul"}
        if op <= 0x2F:
            target = f"vf{fd}" if fd else None
            special = False
            k = op
        else:
            k = (u & 3) | (u >> 4 & 0x7C)
            special = True
            target = "acc"
            if k in (0x2F, 0x30):
                return None, None
            if 0x10 <= k <= 0x17:
                return self.convert(k, ft, fs, dest)
            if k == 0x1D:
                raise Refuse(f"pair {p.pc:#x}: ABS")
            if k == 0x1F:
                return self.clip_op(fs, ft)
            if k == 0x2E:
                if dest & 1:
                    raise Refuse(f"pair {p.pc:#x}: OPMULA with w")
                name = self.new_temp()
                stmt = f"const eg_v4 {name} = EG_VU_YZX({self.vf(fs)}) * EG_VU_ZXY({self.vf(ft)});"
                return self.with_mac(p, stmt, name, dest), self.assign("acc", name, dest)
        if not special and k == 0x2E:
            if dest & 1:
                raise Refuse(f"pair {p.pc:#x}: OPMSUB with w")
            name = self.new_temp()
            stmt = f"const eg_v4 {name} = acc - EG_VU_YZX({self.vf(fs)}) * EG_VU_ZXY({self.vf(ft)});"
            return self.with_mac(p, stmt, name, dest), self.assign(target, name, dest) if target else None
        if not special and (k in (0x1D, 0x1F, 0x2B, 0x2F) or 0x10 <= k <= 0x17):
            if not target or not dest:
                return None, None
            fn = "eg_vu1_max" if k in (0x1D, 0x2B) or 0x10 <= k <= 0x13 else "eg_vu1_min"
            second = "i" if k in (0x1D, 0x1F) else ("vec" if k in (0x2B, 0x2F) else k & 3)
            name = self.new_temp()
            if self.trace:
                self.notes.append(f"eg_vu1_note({p.pc}, {self.vf(fs)}, {dest}); eg_vu1_note({p.pc}, {self.second_operand(second, ft, p.pc)}, {dest});")
            if ft == 0 and isinstance(second, int):
                # a lane of vf0 is a constant that isn't negative
                value = 0x3F800000 if second == 3 else 0
                return f"const eg_v4 {name} = {fn}_const({self.vf(fs)}, {value:#x});", self.assign(target, name, dest)
            return f"const eg_v4 {name} = {fn}({self.vf(fs)}, {self.second_operand(second, ft, p.pc)});", self.assign(target, name, dest)
        vecs = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub"}
        qs = {0x1C: "mul", 0x20: "add", 0x21: "madd", 0x24: "sub", 0x25: "msub"}
        iis = {0x1E: "mul", 0x22: "add", 0x23: "madd", 0x26: "sub", 0x27: "msub"}
        if k in vecs:
            kind, second = vecs[k], "vec"
        elif k in qs:
            kind, second = qs[k], "q"
        elif k in iis:
            kind, second = iis[k], "i"
        elif (k & ~3) in arith and k < 0x1C:
            kind, second = arith[k & ~3], k & 3
        else:
            raise Refuse(f"pair {p.pc:#x}: upper op {op:#x} / {k:#x}")
        s = self.vf(fs)
        t = self.second_operand(second, ft, p.pc)
        name = self.new_temp()
        if self.trace:
            self.notes.append(f"eg_vu1_note({p.pc}, {s}, {dest}); eg_vu1_note({p.pc}, {t}, {dest});" + (f" eg_vu1_note({p.pc}, acc, {dest});" if kind in ("madd", "msub") else ""))
        if dest in LANE and p.pc not in self.mac_vars and target:
            # one lane: scalar, which the A53 does in less time than a full vector
            c = LANE.index(dest)
            ss = f"{s}[{c}]"
            if second == "vec":
                tt = f"{self.vf(ft)}[{c}]"
            elif second == "q":
                tt = self.q_read(p.pc)
            elif second == "i":
                tt = "vu_i"
            else:
                tt = f"{self.vf(ft)}[{second}]"
            if kind in ("add", "sub", "mul"):
                value = f"{ss} {({'add': '+', 'sub': '-', 'mul': '*'})[kind]} {tt}"
            else:
                value = f"acc[{c}] {'+' if kind == 'madd' else '-'} {ss} * {tt}"
            return f"const float {name} = {value};", f"{target}[{c}] = {name};"
        if kind in ("add", "sub", "mul"):
            sym = {"add": "+", "sub": "-", "mul": "*"}[kind]
            stmt = f"const eg_v4 {name} = {s} {sym} {t};"
        else:
            # MADD / MSUB: the product and the sum each round (-ffp-contract=off)
            stmt = f"const eg_v4 {name} = acc {'+' if kind == 'madd' else '-'} {s} * {t};"
        return self.with_mac(p, stmt, name, dest), self.assign(target, name, dest) if target and dest else None

    def with_mac(self, p, stmt, name, dest):
        if p.pc in self.mac_vars:
            return f"{stmt} mac_{p.pc:03x} = eg_vu1_mac({name}, {dest});"
        return stmt

    def convert(self, k, ft, fs, dest):
        if ft == 0 or dest == 0:
            return None, None
        scale = (1, 16, 4096, 32768)[k & 3]
        name = self.new_temp()
        s = self.vf(fs)
        if k < 0x14:
            value = f"__builtin_convertvector((eg_vi4){s}, eg_v4)"
            if scale != 1:
                value += f" * (1.0f / {scale}.0f)"
            return f"const eg_v4 {name} = {value};", self.assign(f"vf{ft}", name, dest)
        value = f"{s} * {scale}.0f" if scale != 1 else s
        return f"const eg_v4 {name} = (eg_v4)eg_vu1_ftoi({value});", self.assign(f"vf{ft}", name, dest)

    def clip_op(self, fs, ft):
        name = self.new_temp()
        return f"const uint32_t {name} = eg_vu1_clip({self.vf(fs)}, {self.vf(ft)});", f"clip = (clip << 6) | {name};"

    # --- lower ---------------------------------------------------------------------------------------------
    def address(self, base, imm=None):
        if imm is None:
            return f"((((uint32_t)(uint16_t){base}) * 16u) & 16383u)"
        return f"(((uint32_t)(int32_t)({base} + {imm}) * 16u) & 16383u)"

    def load(self, p, addr, vft, dest):
        """Returns (statement, commit) of a load into vft."""
        if vft == 0 or dest == 0 or p.suppress:
            return "", None
        name = self.new_temp()
        return f"eg_v4 {name}; std::memcpy(&{name}, data + {addr}, 16);", self.assign(f"vf{vft}", name, dest)

    def store(self, addr, value, dest):
        if dest == 0xF:
            return f"std::memcpy(data + {addr}, &{value}, 16);"
        if dest == 0:
            return ""
        return f"eg_vu1_store(data + {addr}, {value}, {dest});"

    def lower(self, p):
        """Returns (statement, commit statement), each possibly empty or None."""
        l = p.l
        if p.ibit or l in (0, 0x8000033C):
            return "", None
        op = l >> 25
        vft, vfs = l >> 16 & 31, l >> 11 & 31
        vit, vis, vid = l >> 16 & 15, l >> 11 & 15, l >> 6 & 15
        dest = l >> 21 & 0xF
        imm = signed11(l)
        imm15 = (l >> 21 & 0xF) << 11 | l & 0x7FF

        def set_vi(reg, value):
            if reg == 0:
                return "", None
            name = self.new_temp()
            return f"const int16_t {name} = (int16_t)({value});", f"vi{reg} = {name};"

        if op == 0x00:
            return self.load(p, self.address(self.vi(vis), imm), vft, dest)
        if op == 0x01:
            return self.store(self.address(self.vi(vit), imm), self.vf(vfs), dest), None
        if op == 0x04:
            c = comps(dest)[0] if dest else 3
            return set_vi(vit, f"eg_vu1_lane16(data + {self.address(self.vi(vis), imm)}, {c})")
        if op == 0x05:
            return f"eg_vu1_store_int(data + {self.address(self.vi(vis), imm)}, {self.vi(vit)}, {dest});", None
        if op == 0x08:
            return set_vi(vit, f"{self.vi(vis)} + {imm15}")
        if op == 0x09:
            return set_vi(vit, f"{self.vi(vis)} - {imm15}")
        if op in (0x10, 0x12, 0x13, 0x1C):
            value = self.clip_value(p.pc, next(iter(self.facts.clip[p.pc])))
            if op == 0x12:
                return set_vi(1, f"({value} & {l & 0xFFFFFF}u) != 0u")
            if op == 0x10:
                return set_vi(1, f"({value} & 0xFFFFFFu) == {l & 0xFFFFFF}u")
            if op == 0x13:
                return set_vi(1, f"(({value} & 0xFFFFFFu) | {l & 0xFFFFFF}u) == 0xFFFFFFu")
            return set_vi(vit, f"{value} & 0xFFFu")
        if op == 0x11:
            return "", f"clip_f = clip; clip = {l & 0xFFFFFF}u;"
        if op == 0x18:
            provider = next(iter(self.facts.mac[p.pc]))
            mac = "mac_in" if provider == -1 else f"mac_{provider:03x}"
            return set_vi(vit, f"(({mac} & 0xFFFFu) == (uint32_t)(uint16_t){self.vi(vis)}) ? 1 : 0")
        if op == 0x20:
            return "", None
        if op == 0x21:
            return set_vi(vit, str(p.pc + 2))
        if op == 0x24 or op == 0x25:
            stmt = f"jr = (uint32_t)(uint16_t){self.vi(vis)} & 0x7FFu;"
            if op == 0x25:
                s2, c2 = set_vi(vit, str(p.pc + 2))
                return stmt + " " + s2, c2
            return stmt, None
        if op in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            old = next(iter(self.facts.old[p.pc]))

            def branch_vi(n):
                return "vi_old" if n and n == old else self.vi(n)
            a = branch_vi(vis)
            if op == 0x28:
                return f"c = {a} == {branch_vi(vit)};", None
            if op == 0x29:
                return f"c = {a} != {branch_vi(vit)};", None
            return f"c = {a} {({0x2C: '<', 0x2D: '>', 0x2E: '<=', 0x2F: '>='})[op]} 0;", None
        if op == 0x40:
            f = l & 0x3F
            if f == 0x30:
                return set_vi(vid, f"{self.vi(vis)} + {self.vi(vit)}")
            if f == 0x31:
                return set_vi(vid, f"{self.vi(vis)} - {self.vi(vit)}")
            if f == 0x32:
                imm5 = l >> 6 & 31
                imm5 = imm5 - 32 if imm5 & 16 else imm5
                return set_vi(vit, f"{self.vi(vis)} + {imm5}")
            if f == 0x34:
                return set_vi(vid, f"{self.vi(vis)} & {self.vi(vit)}")
            if f == 0x35:
                return set_vi(vid, f"{self.vi(vis)} | {self.vi(vit)}")
            k = (l & 3) | (l >> 4 & 0x7C)
            fsf, ftf = l >> 21 & 3, l >> 23 & 3
            if k == 0x30 or k == 0x31:
                if vft == 0 or dest == 0 or p.suppress:
                    return "", None
                name = self.new_temp()
                s = self.vf(vfs)
                return f"const eg_v4 {name} = {s if k == 0x30 else f'EG_VU_MR32({s})'};", self.assign(f"vf{vft}", name, dest)
            if k in (0x34, 0x36):
                delta = 1 if k == 0x34 else -1
                base = self.vi(vis)
                addr = self.address(base if k == 0x34 else f"({base} - 1)")
                stmt, commit = self.load(p, addr, vft, dest)
                if vis:
                    commit = (commit or "") + f" vi{vis} = (int16_t)(vi{vis} + {delta});"
                return stmt, commit
            if k in (0x35, 0x37):
                delta = 1 if k == 0x35 else -1
                base = self.vi(vit)
                addr = self.address(base if k == 0x35 else f"({base} - 1)")
                return self.store(addr, self.vf(vfs), dest), (f"vi{vit} = (int16_t)(vi{vit} + {delta});" if vit else None)
            if k == 0x38 or k == 0x39:
                name = self.new_temp()
                value = (f"eg_vu1_div({self.vf(vfs)}[{fsf}], {self.vf(vft)}[{ftf}])" if k == 0x38
                         else f"eg_vu1_sqrt({self.vf(vft)}[{ftf}])")
                keep = "q_old = q; " if self.use_q_old else ""
                return f"const float {name} = {value};", f"{keep}q = {name};"
            if k == 0x3B or k == 0x7B:
                return "", None
            if k == 0x3C:
                return set_vi(vit, f"((eg_vi4){self.vf(vfs)})[{fsf}]")
            if k == 0x3D:
                if vft == 0 or dest == 0 or p.suppress:
                    return "", None
                name = self.new_temp()
                v = f"(int32_t){self.vi(vis)}"
                return f"const eg_v4 {name} = (eg_v4)(eg_vi4{{{v}, {v}, {v}, {v}}});", self.assign(f"vf{vft}", name, dest)
            if k == 0x64:
                if vft == 0 or dest == 0 or p.suppress:
                    return "", None
                name = self.new_temp()
                value = "p_old" if 1 in self.facts.p[p.pc] else "p"
                return f"const eg_v4 {name} = EG_VU_ALL({value});", self.assign(f"vf{vft}", name, dest)
            if k == 0x68:
                return set_vi(vit, "top & 0x3FFu")
            if k == 0x69:
                return set_vi(vit, "itop & 0x3FFu")
            if k == 0x6C:
                return f"eg_vu1_kick(data, (uint32_t)(uint16_t){self.vi(vis)}, fp);", None
            if k in (0x73, 0x76, 0x7A):
                name = self.new_temp()
                value = {0x73: f"eg_vu1_erleng({self.vf(vfs)})", 0x76: f"eg_vu1_esum({self.vf(vfs)})", 0x7A: f"eg_vu1_ercpr({self.vf(vfs)}[{fsf}])"}[k]
                keep = "p_old = p; " if self.use_p_old else ""
                return f"const float {name} = {value};", f"{keep}p = {name};"
        raise Refuse(f"pair {p.pc:#x}: no translation for lower {l:#010x}")

    def pair(self, pc):
        p = self.pairs[pc]
        out = []
        if pc in self.facts.backup:
            out.append(f"vi_old = vi{p.lu.vi_write};")
        stmt_u, commit_u = self.upper(p)
        stmt_l, commit_l = self.lower(p)
        out += self.notes
        self.notes = []
        for s in (stmt_u, stmt_l):
            if s:
                out.append(s)
        if p.ibit:
            out.append(f"vu_i = {float_literal(p.l)};")
        if p.uu.vf_write and not (p.uu.vf_write[0] != 0):
            raise Refuse(f"pair {pc:#x}: write to vf0")
        for s in (commit_u, commit_l):
            if s:
                out.append(s)
        return "{ " + " ".join(out) + " }" if out else None

    def body(self):
        out = []
        order = sorted(self.facts.normal)
        for index, pc in enumerate(order):
            p = self.pairs[pc]
            out.append(f"L{pc:03x}:;")
            if self.count:
                out.append(f"    ++g_egVu1Counts[{pc}];")
            code = self.pair(pc)
            if code:
                out.append("    " + code)
            following = order[index + 1] if index + 1 < len(order) else None
            if self.count and (p.ebit or p.branch):
                out.append(f"    ++g_egVu1Counts[{(pc + 1) & 0x7FF}];")
            if p.ebit:
                code = self.pair((pc + 1) & 0x7FF)
                if code:
                    out.append("    " + code)
                out.append("    goto done;")
                continue
            if p.branch:
                code = self.pair((pc + 1) & 0x7FF)
                if code:
                    out.append("    " + code)
                if p.branch == "cond":
                    out.append(f"    if (c) goto L{p.target:03x};")
                    if following != pc + 2:
                        out.append(f"    goto L{(pc + 2) & 0x7FF:03x};")
                elif p.branch == "always":
                    out.append(f"    goto L{p.target:03x};")
                else:
                    cases = " ".join(f"case {t:#x}: goto L{t:03x};" for t in sorted(self.facts.jr[pc]))
                    out.append(f"    switch (jr) {{ {cases} default: goto bad; }}")
                continue
            if following != pc + 1:
                out.append(f"    goto L{(pc + 1) & 0x7FF:03x};")
        return out

    def function(self, entries):
        out = ["bool egVu1Run(EgVu1State &st, uint8_t *data, uint32_t entry, uint32_t top, uint32_t itop)", "{"]
        out.append("    const uint64_t fp = eg_vu_enter();")
        for n in range(1, 32):
            out.append(f"    eg_v4 vf{n}; std::memcpy(&vf{n}, st.vf[{n}], 16);")
        out.append("    eg_v4 acc; std::memcpy(&acc, st.acc, 16);")
        for n in range(1, 16):
            out.append(f"    int16_t vi{n} = (int16_t)st.vi[{n}];")
        out.append("    float q = st.q, q_old = st.q, p = st.p, p_old = st.p, vu_i = st.i;")
        out.append("    uint64_t clip = st.clip, clip_f = st.clip;")
        out.append("    bool c = false, ok = true; uint32_t jr = 0; int16_t vi_old = 0;")
        if self.mac_in:
            out.append("    const uint32_t mac_in = st.mac;")
        for pc in sorted(self.mac_vars):
            out.append(f"    uint32_t mac_{pc:03x} = 0;")
        out.append("    switch (entry)")
        out.append("    {")
        for e in entries:
            out.append(f"    case {e:#x}: goto L{e:03x};")
        out.append("    default: ok = false; goto done;")
        out.append("    }")
        out += self.body()
        out.append("bad:")
        out.append("    ok = false;")
        out.append("done:")
        for n in range(1, 32):
            out.append(f"    std::memcpy(st.vf[{n}], &vf{n}, 16);")
        out.append("    std::memcpy(st.acc, &acc, 16);")
        for n in range(1, 16):
            out.append(f"    st.vi[{n}] = (uint16_t)vi{n};")
        out.append("    st.q = q; st.p = p; st.i = vu_i; st.clip = (uint32_t)clip & 0xFFFFFFu;")
        out.append("    eg_vu_leave(fp);")
        out.append("    return ok;")
        out.append("}")
        return out


def entry_stubs(code):
    """The stubs at the start of micro memory: IADDIU viN, vi0, body / JR viN / a delay slot."""
    found = []
    for pc in range(0, 60, 3):
        l0, _ = struct.unpack_from("<II", code, pc * 8)
        l1, _ = struct.unpack_from("<II", code, (pc + 1) * 8)
        if l0 >> 25 == 0x08 and l1 >> 25 == 0x24 and (l0 >> 16 & 15) == (l1 >> 11 & 15) and (l0 >> 11 & 15) == 0:
            found.append(pc)
        else:
            break
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("micro")
    ap.add_argument("out")
    ap.add_argument("--entries", help="comma-separated pair indices; default: the stubs found")
    ap.add_argument("--report", action="store_true", help="print each timing-sensitive read")
    ap.add_argument("--trace", action="store_true", help="count operands a host float can't hold, by pair")
    ap.add_argument("--count", action="store_true", help="count how often each pair runs")
    ap.add_argument("--reached", help="write which pairs can run, as a C array g_vu1Reached")
    args = ap.parse_args(argv)
    code = open(args.micro, "rb").read()
    entries = [int(t, 0) for t in args.entries.split(",")] if args.entries else entry_stubs(code)
    print("entries:", " ".join(hex(e) for e in entries))
    pairs, facts, states = analyze(code, entries)
    print(f"{len(pairs)} pairs reached, {states} pipeline states, {len(facts.normal)} entered normally, "
          f"{len(facts.slot)} as a slot")
    if args.report:
        print("Q read in flight:", sorted(hex(pc) for pc, v in facts.q.items() if 1 in v))
        print("Q reads:", len(facts.q), "P reads:", {hex(pc): sorted(v) for pc, v in facts.p.items()})
        print("MAC reads:", {hex(pc): sorted(hex(x) for x in v) for pc, v in facts.mac.items()})
        print("clip reads:", {hex(pc): sorted(v) for pc, v in facts.clip.items()})
        print("old-value branches:", {hex(pc): sorted(v) for pc, v in facts.old.items() if v != {0}})
        print("JR:", {hex(pc): sorted(hex(x) for x in v) for pc, v in facts.jr.items()})
    if args.reached:
        with open(args.reached, "w", newline="\n") as f:
            f.write(f"static const unsigned char g_vu1Reached[{len(code) // 8}] = {{\n")
            f.write(",".join("1" if pc in pairs else "0" for pc in range(len(code) // 8)))
            f.write("\n};\n")
    gen = Emit(pairs, facts, args.trace, args.count)
    lines = ["// generated by eegen/vu1gen.py from game data; never commit"] + gen.function(entries)
    with open(args.out, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {args.out}: {len(lines)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
