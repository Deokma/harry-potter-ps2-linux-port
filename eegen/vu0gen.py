#!/usr/bin/env python3
"""vu0gen: VU0 microprograms -> C++ (host vector code), first version.

    python3 -I eegen/vu0gen.py VU0CODE OUT.inc --entries 0x16,0x34,...

VU0CODE is a dump of VU0 micro memory (game data, stays in private/), and so
is OUT.inc, which the replay harness includes. An entry is translated only
if its trace from the entry point is straight-line (taken branches and a
jump through a register holding a known constant are followed) and uses the
instructions below; anything else is refused with the reason and stays with
the interpreter.

Semantics follow PS2Recomp's VU interpreter so that the two can be compared
byte for byte (HARNESS_VU0=4); see eegen/eegen_vu0.h for how the host float
modes give those semantics and when a program gives up and leaves the call
to the interpreter. MAC / status / clip flags are not produced: no translated
program reads them, and whether the game's EE code does is what the
whole-frame comparison checks.
"""
import argparse
import struct
import sys

LANES = "xyzw"
ALL = 0xF


def lanes_of(dest):
    """Lane indices of a dest field (bit 3 = x ... bit 0 = w)."""
    return [c for c in range(4) if dest >> (3 - c) & 1]


class Refuse(Exception):
    pass


class Program:
    def __init__(self, entry):
        self.entry = entry
        self.body = []
        self.defined = {}    # register -> lanes defined in this program
        self.pending = []    # (register, dest) written by the current pair
        self.inputs = {}     # register -> lanes read before being defined
        self.written = set()
        self.vi_inputs = set()
        self.vi_defined = set()
        self.vi_written = set()
        self.q_input = False
        self.q_defined = False
        self.q_at = None
        self.stores = 0
        self.temp = 0
        self.vi_const = {0: 0}
        self.pairs = 0
        self.ops = 0

    # --- registers ------------------------------------------------------
    def read(self, reg, lanes):
        if reg == "vf0":
            return "eg_vf0"
        missing = set(lanes) - self.defined.get(reg, set())
        if missing:
            self.inputs.setdefault(reg, set()).update(missing)
        return reg

    def read_vi(self, n):
        if n == 0:
            return "0"
        if n not in self.vi_defined:
            self.vi_inputs.add(n)
        return f"vi{n}"

    def read_q(self, pair):
        if self.q_at is None:
            self.q_input = True
        elif pair - self.q_at < 7:
            raise Refuse("Q read before the division finishes")
        return "q"

    def assign(self, reg, value, dest):
        """A statement giving reg the lanes in dest of value."""
        # both halves of a pair read what was there before it
        self.pending.append((reg, dest))
        self.written.add(reg)
        if dest == ALL:
            return f"{reg} = {value};"
        m = ", ".join("-1" if dest >> (3 - c) & 1 else "0" for c in range(4))
        # the old lanes stay as they are, read or not
        return f"{reg} = EG_VU_MERGE({value}, {reg}, {m});"

    def new_temp(self):
        self.temp += 1
        return f"t{self.temp}"

    # --- upper instructions ---------------------------------------------
    def upper(self, u, pair):
        """Returns (statement computing a temp, commit statement), or None."""
        if u & (0xF << 27) & ~(1 << 29) or u >> 31 & 1:
            raise Refuse(f"upper flags {u >> 27 & 0x1F:#x} (I, E handled; D, T not)")
        op = u & 0x3F
        dest, ft, fs, fd = u >> 21 & 0xF, u >> 16 & 31, u >> 11 & 31, u >> 6 & 31
        if op >= 0x3C:
            k = (u & 3) | (u >> 4 & 0x7C)
            if k in (0x2F, 0x30):
                return None
            if k == 0x2E:
                return self.opmula(fs, ft, dest)
            kinds = {0x00: "add", 0x04: "sub", 0x08: "madd", 0x0C: "msub", 0x18: "mul"}
            vecs = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub"}
            qs = {0x1C: "mul", 0x20: "add", 0x21: "madd", 0x24: "sub", 0x25: "msub"}
            target = "acc"
        else:
            k = op
            if k == 0x2E:
                return self.opmsub(fd, fs, ft, dest)
            if k in (0x2B, 0x2F) or 0x10 <= k <= 0x17:
                return self.minmax(k, fd, fs, ft, dest)
            kinds = {0x00: "add", 0x04: "sub", 0x08: "madd", 0x0C: "msub", 0x18: "mul"}
            vecs = {0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2C: "sub", 0x2D: "msub"}
            qs = {0x1C: "mul", 0x20: "add", 0x21: "madd", 0x24: "sub", 0x25: "msub"}
            target = f"vf{fd}"
        if k in vecs:
            kind, second = vecs[k], ("vec", ft)
        elif k in qs:
            kind, second = qs[k], ("q", 0)
        elif (k & ~3) in kinds and k < 0x1C:
            kind, second = kinds[k & ~3], ("bc", k & 3)
        else:
            raise Refuse(f"upper op {op:#x} / {k:#x}")
        if target == "vf0":
            return None
        self.ops += 1
        lanes = lanes_of(dest)
        s = self.read(f"vf{fs}", lanes)
        if second[0] == "vec":
            whole_t = self.read(f"vf{ft}", lanes)
        elif second[0] == "bc":
            t = self.read(f"vf{ft}", [second[1]])
            whole_t = f"EG_VU_BC({t}, {second[1]})"
        else:
            whole_t = f"EG_VU_ALL({self.read_q(pair)})"
        name = self.new_temp()
        if kind in ("add", "sub", "mul"):
            sym = {"add": "+", "sub": "-", "mul": "*"}[kind]
            stmt = f"const eg_v4 {name} = {s} {sym} {whole_t};"
            return stmt, self.assign(target, name, dest)
        # MADD / MSUB: the product and the sum each round (-ffp-contract=off)
        acc = self.read("acc", lanes)
        sym = "+" if kind == "madd" else "-"
        stmt = f"const eg_v4 {name} = {acc} {sym} {s} * {whole_t};"
        return stmt, self.assign(target, name, dest)

    def opmula(self, fs, ft, dest):
        if dest & 1:
            raise Refuse("OPMULA with w")
        self.ops += 1
        s = self.read(f"vf{fs}", [0, 1, 2])
        t = self.read(f"vf{ft}", [0, 1, 2])
        name = self.new_temp()
        stmt = f"const eg_v4 {name} = EG_VU_YZX({s}) * EG_VU_ZXY({t});"
        return stmt, self.assign("acc", name, dest)

    def opmsub(self, fd, fs, ft, dest):
        if dest & 1:
            raise Refuse("OPMSUB with w")
        self.ops += 1
        s = self.read(f"vf{fs}", [0, 1, 2])
        t = self.read(f"vf{ft}", [0, 1, 2])
        acc = self.read("acc", [0, 1, 2])
        name = self.new_temp()
        stmt = f"const eg_v4 {name} = {acc} - EG_VU_YZX({s}) * EG_VU_ZXY({t});"
        return stmt, self.assign(f"vf{fd}", name, dest)

    def minmax(self, k, fd, fs, ft, dest):
        if fd == 0:
            return None
        self.ops += 1
        lanes = lanes_of(dest)
        fn = "eg_vu_max" if k == 0x2B or 0x10 <= k <= 0x13 else "eg_vu_min"
        s = self.read(f"vf{fs}", lanes)
        if k in (0x2B, 0x2F):
            t = self.read(f"vf{ft}", lanes)
        else:
            bc = k & 3
            t = f"EG_VU_BC({self.read(f'vf{ft}', [bc])}, {bc})"
        name = self.new_temp()
        return f"const eg_v4 {name} = {fn}({s}, {t});", self.assign(f"vf{fd}", name, dest)

    # --- lower instructions ---------------------------------------------
    def lower(self, l, pair, pc):
        """Returns (statement, commit statement, branch target)."""
        op = l >> 25
        if op == 0x40:
            f = l & 0x3F
            if f >= 0x3C:
                return self.lower_special(l, pair)
            if f == 0x32:  # IADDI
                vit, vis = l >> 16 & 15, l >> 11 & 15
                imm = l >> 6 & 31
                imm = imm - 32 if imm & 16 else imm
                if vit == 0:
                    return None, None, None
                value = self.read_vi(vis)
                self.vi_defined.add(vit)
                self.vi_written.add(vit)
                self.vi_const.pop(vit, None)
                name = self.new_temp()
                return (f"const int16_t {name} = (int16_t)({value} + {imm});",
                        f"vi{vit} = {name};", None)
            raise Refuse(f"lower special {f:#x}")
        if op == 0x08:  # IADDIU
            vit, vis = l >> 16 & 15, l >> 11 & 15
            imm = (l >> 21 & 0xF) << 11 | l & 0x7FF
            if vit == 0:
                return None, None, None
            value = self.read_vi(vis)
            self.vi_defined.add(vit)
            self.vi_written.add(vit)
            if vis in self.vi_const:
                self.vi_const[vit] = self.vi_const[vis] + imm
            else:
                self.vi_const.pop(vit, None)
            name = self.new_temp()
            return f"const int16_t {name} = (int16_t)({value} + {imm});", f"vi{vit} = {name};", None
        if op == 0x00:  # LQ
            dest, vft, vis = l >> 21 & 0xF, l >> 16 & 31, l >> 11 & 15
            if self.stores:
                # stores wait until the program's flags have been read
                raise Refuse("LQ after SQ")
            if dest == 0 or vft == 0:
                return None, None, None
            imm = l & 0x7FF
            imm = imm - 0x800 if imm & 0x400 else imm
            self.ops += 1
            base = self.read_vi(vis)
            name = self.new_temp()
            stmt = (f"eg_v4 {name}; std::memcpy(&{name}, "
                    f"data + (((uint32_t)(int32_t)({base} + {imm}) * 16u) & 4095u), 16);")
            return stmt, self.assign(f"vf{vft}", name, dest), None
        if op == 0x01:  # SQ
            dest, fs, vit = l >> 21 & 0xF, l >> 11 & 31, l >> 16 & 15
            if dest != ALL:
                raise Refuse("SQ with a partial dest")
            imm = l & 0x7FF
            imm = imm - 0x800 if imm & 0x400 else imm
            k = self.stores
            if k == 16:
                raise Refuse("more than 16 stores")
            self.stores += 1
            v = self.read(f"vf{fs}", [0, 1, 2, 3])
            base = self.read_vi(vit)
            return (f"o.sq[{k}] = {v}; o.sqa[{k}] = ((uint32_t)(int32_t)({base} + {imm}) * 16u) & 4095u;",
                    None, None)
        if op == 0x20:  # B
            imm = l & 0x7FF
            imm = imm - 0x800 if imm & 0x400 else imm
            return None, None, pc + 1 + imm
        if op == 0x24:  # JR
            vis = l >> 11 & 15
            if vis not in self.vi_const:
                raise Refuse("JR through a register that isn't constant")
            return None, None, self.vi_const[vis]
        raise Refuse(f"lower op {op:#x}")

    def lower_special(self, l, pair):
        k = (l >> 6 & 31) << 2 | l & 3
        dest, vft, vfs = l >> 21 & 0xF, l >> 16 & 31, l >> 11 & 31
        if k == 0x30:  # MOVE (not normalized)
            if dest == 0 or vft == 0:
                return None, None, None
            self.ops += 1
            s = self.read(f"vf{vfs}", lanes_of(dest))
            name = self.new_temp()
            return f"const eg_v4 {name} = {s};", self.assign(f"vf{vft}", name, dest), None
        if k == 0x31:  # MR32 (not normalized)
            if dest == 0 or vft == 0:
                return None, None, None
            self.ops += 1
            s = self.read(f"vf{vfs}", [(c + 1) & 3 for c in lanes_of(dest)])
            name = self.new_temp()
            return f"const eg_v4 {name} = EG_VU_MR32({s});", self.assign(f"vf{vft}", name, dest), None
        if k == 0x38:  # DIV
            fsf, ftf = l >> 21 & 3, l >> 23 & 3
            self.ops += 1
            num = self.read(f"vf{vfs}", [fsf])
            den = self.read(f"vf{vft}", [ftf])
            self.q_at = pair
            self.q_defined = True
            name = self.new_temp()
            return (f"if ({den}[{ftf}] == 0.0f) return false; const float {name} = {num}[{fsf}] / {den}[{ftf}];",
                    f"q = {name};", None)
        raise Refuse(f"lower special op {k:#x}")


def translate(code, entry):
    p = Program(entry)
    pc = entry
    pending = None
    last = False
    pair = 0
    while True:
        if pair > 400 or pc * 8 + 8 > len(code):
            raise Refuse("runs off the end")
        l, u = struct.unpack_from("<II", code, pc * 8)
        ebit = u >> 30 & 1
        if u >> 31 & 1:
            raise Refuse("I bit (LOI)")
        res_u = p.upper(u & ~(1 << 30), pair)
        stmt_l, commit_l, target = p.lower(l, pair, pc)
        if target is not None and (pending is not None or ebit or last):
            raise Refuse("branch in a delay slot")
        # both halves of a pair read the registers as they were before it;
        # the lower instruction's write comes last
        if res_u:
            p.body.append(res_u[0])
        if stmt_l:
            p.body.append(stmt_l)
        if res_u:
            p.body.append(res_u[1])
        if commit_l:
            p.body.append(commit_l)
        for reg, dest in p.pending:
            p.defined.setdefault(reg, set()).update(lanes_of(dest))
        p.pending.clear()
        pair += 1
        p.pairs += 1
        next_pc = pc + 1
        if pending is not None:
            next_pc, pending = pending, None
        if target is not None:
            pending = target
        if last:
            break
        if ebit:
            last = True
        pc = next_pc
    if p.q_at is not None and pair - p.q_at < 7:
        raise Refuse("division still running when the program ends")
    return p


def emit(p):
    tag = f"{p.entry:04x}"
    out = [f"// entry {p.entry:#05x}: {p.pairs} pairs, {p.ops} operations, {p.stores} stores"]
    out.append(f"__attribute__((noipa)) static bool eg_vu0_body_{tag}(const R5900Context *ctx, const uint8_t *data, EgVu0Out &o)")
    out.append("{")
    regs = sorted({r for r in p.inputs} | p.written, key=lambda r: (r == "acc", int(r[2:]) if r != "acc" else 0))
    for r in regs:
        src = "ctx->vu0_acc" if r == "acc" else f"ctx->vu0_vf[{r[2:]}]"
        out.append(f"    eg_v4 {r}; std::memcpy(&{r}, &{src}, 16);")
    if p.inputs or p.q_input:
        out.append("    eg_vi4 bad{};")
        for r, lanes in sorted(p.inputs.items()):
            m = ", ".join("-1" if c in lanes else "0" for c in range(4))
            out.append(f"    bad |= EG_VU_EXP255({r}, {m});")
        out.append("    if (eg_vu_any(bad)) return false;")
    if p.q_input or p.q_defined:
        out.append("    float q = ctx->vu0_q;")
    if p.q_input:
        out.append("    if (eg_vu_exp255(q)) return false;")
    for n in sorted(p.vi_inputs | p.vi_written):
        out.append(f"    int16_t vi{n} = (int16_t)ctx->vi[{n}];")
    out += ["    " + s for s in p.body]
    written = sorted(p.written - {"acc"}, key=lambda r: int(r[2:]))
    for r in written:
        out.append(f"    o.vf[{r[2:]}] = {r};")
    if "acc" in p.written:
        out.append("    o.acc = acc;")
    if p.q_defined:
        out.append("    o.q = q;")
    for n in sorted(p.vi_written):
        out.append(f"    o.vi[{n}] = vi{n};")
    out.append("    return true;")
    out.append("}")
    out.append(f"static bool eg_vu0_run_{tag}(R5900Context *ctx, uint8_t *data)")
    out.append("{")
    out.append("    EgVu0Out o;")
    out.append("    const uint64_t fp = eg_vu_enter();")
    out.append(f"    const bool ok = eg_vu0_body_{tag}(ctx, data, o) && !eg_vu_bad();")
    out.append("    eg_vu_leave(fp);")
    out.append("    if (!ok) return false;")
    for r in written:
        out.append(f"    std::memcpy(&ctx->vu0_vf[{r[2:]}], &o.vf[{r[2:]}], 16);")
    if "acc" in p.written:
        out.append("    std::memcpy(&ctx->vu0_acc, &o.acc, 16);")
    if p.q_defined:
        out.append("    ctx->vu0_q = o.q;")
    for n in sorted(p.vi_written):
        out.append(f"    ctx->vi[{n}] = (uint16_t)o.vi[{n}];")
    for k in range(p.stores):
        out.append(f"    std::memcpy(data + o.sqa[{k}], &o.sq[{k}], 16);")
    out.append("    return true;")
    out.append("}")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("code")
    ap.add_argument("out")
    ap.add_argument("--entries", required=True, help="comma-separated pair indices")
    args = ap.parse_args(argv)
    code = open(args.code, "rb").read()
    parts, done = ["// generated by eegen/vu0gen.py from game data; never commit"], []
    for text in args.entries.split(","):
        entry = int(text, 0)
        try:
            p = translate(code, entry)
        except Refuse as e:
            print(f"entry {entry:#05x}: refused, {e}")
            continue
        parts.append(emit(p))
        done.append(entry)
        print(f"entry {entry:#05x}: {p.pairs} pairs, {p.ops} operations, {p.stores} stores, "
              f"inputs {len(p.inputs)} registers")
    parts.append("static EgVu0Fn eg_vu0_lookup(uint32_t entry)\n{\n    switch (entry)\n    {")
    parts += [f"    case {e:#x}: return eg_vu0_run_{e:04x};" for e in done]
    parts.append("    default: return nullptr;\n    }\n}")
    with open(args.out, "w", newline="\n") as f:
        f.write("\n".join(parts) + "\n")
    print(f"{len(done)} entries in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
