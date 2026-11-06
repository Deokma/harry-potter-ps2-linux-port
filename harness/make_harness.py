"""ps2port trial, step 3: prepare the replay harness sources for one recording.

    python -I make_harness.py RECORDING OUT_DIR HARNESS_DIR [v0|v1|v2]

RECORDING.in gives the entry address; RECORDING.targets lists every function
the call reached on the PC. From PS2Recomp's output OUT_DIR this copies the
generated files that own those addresses into HARNESS_DIR/gen/ with
HARNESS_COUNT() after every guest instruction (a no-op in the timing build),
and writes HARNESS_DIR/gen/harness_table.cpp registering all their entry
points (one slot per guest word, as harness::g_table expects).

Code-generation experiments (the result check in main.cpp validates them):
  v0  PS2Recomp's output as is
  v1  + harness_fastgpr.h: scalar access to the low 64 bits of the GPRs
  v2  v1 + no per-instruction `ctx->pc = ...` stores
  v3  v2 + harness_fastmem.h (direct RAM access) + no loop checkpoints

Everything it writes is derived from the game: keep it under private/.
"""
import re
import struct
import sys
from pathlib import Path

recording, out_dir, harness_dir = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
variant = sys.argv[4] if len(sys.argv) > 4 else "v0"

header = recording.with_name(recording.name + ".in").read_bytes()[:16 + 1024]
ctx_size = struct.unpack_from("<I", header, 8)[0]
# R5900Context: 32 x 16-byte GPRs, then pc
entry = struct.unpack_from("<I", header, 16 + 32 * 16)[0]

wanted = {entry}
targets = recording.with_name(recording.name + ".targets")
if targets.exists():
    for line in targets.read_text().split("\n"):
        if line.strip():
            wanted.add(int(line.split()[0], 16))

table_line = re.compile(r"g_ps2RecompiledFunctionTable\[\d+\] = (\w+); // 0x([0-9a-f]+)")
owner = {}
entries = {}
for line in (out_dir / "register_functions.cpp").read_text().split("\n"):
    m = table_line.search(line)
    if m:
        name, addr = m.group(1), int(m.group(2), 16)
        owner[addr] = name
        entries.setdefault(name, []).append(addr)

missing = sorted(a for a in wanted if a not in owner)
names = sorted({owner[a] for a in wanted if a in owner})
if not targets.exists():
    # a replay state built from a savestate: the whole program
    names = sorted(set(owner.values()))
if missing:
    print("not in the table:", " ".join(f"0x{a:08x}" for a in missing))

gen = harness_dir / "gen"
gen.mkdir(parents=True, exist_ok=True)
for old in gen.glob("*.cpp"):
    old.unlink()
instr = re.compile(r"^(\s*)// 0x[0-9a-f]+: 0x[0-9a-f]+ ")
def mnemonic_class(line):
    """Instruction class for the counting build's mix (see main.cpp)."""
    parts = line.split("//", 1)[1].split()
    mn = parts[2] if len(parts) > 2 else "?"
    if mn == "nop":
        return 9
    if mn in ("lq", "sq", "lqc2", "sqc2"):
        return 2
    if mn.startswith("v") or mn in ("qmfc2", "qmtc2", "cfc2", "ctc2", "bc2f", "bc2t", "bc2fl", "bc2tl"):
        return 5
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
    if mn.startswith("p") and mn not in ("pref",) or mn.endswith("1") and mn[:-1] in ("mfhi", "mflo", "mthi", "mtlo"):
        return 6
    if mn in ("syscall", "break", "sync", "sync.p", "sync.l", "cache", "pref", "ei", "di", "eret") or mn.startswith(("mfc0", "mtc0")):
        return 8
    return 0


pc_store = re.compile(r"^\s*ctx->pc = 0x[0-9A-Fa-f]+u;$")
# vf0 is a constant on the VU, but PS2Recomp's macro code writes it: this
# game's wait for a microprogram (vadd.xyzw vf0, vf0, vf0) doubles vf0.w
vf0_write = re.compile(r"ctx->vu0_vf\[0\] = PS2_VBLEND\(.*?\);")
counted = dropped = 0
for name in names:
    src = next(out_dir.glob(f"{name}.cpp"), None) or next(out_dir.glob(f"{name}_*.cpp"))
    lines = []
    after_instr = False
    for line in src.read_text().split("\n"):
        line = vf0_write.sub("", line)
        if variant == "v3":
            # loop checkpoints reduced to nothing (a local countdown would
            # cost about as much)
            line = line.replace("runtime->eeCheckpointDue()", "false")
        if variant in ("v2", "v3") and after_instr and pc_store.match(line):
            dropped += 1  # the per-instruction marker store
            after_instr = False
            continue
        lines.append(line)
        if variant in ("v1", "v2", "v3") and line.startswith("#include <ps2_recompiled_stubs.h>"):
            lines.append('#include "harness_fastgpr.h"')
            if variant == "v3":
                lines.append('#include "harness_fastmem.h"')
        m = instr.match(line)
        after_instr = bool(m)
        if m:
            lines.append(f"{m.group(1)}HARNESS_COUNT_K({mnemonic_class(line)});")
            counted += 1
    (gen / src.name).write_bytes("\n".join(lines).encode())

# Native library stubs the copied code references: those PS2Recomp defines in
# Kernel/Stubs/LibC.cpp (math, memory, strings) are compiled from there by
# build.sh; every other one gets a default that returns 0 and is counted.
libc = Path(__file__).resolve().parents[1] / "private/third_party/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/LibC.cpp"
from_libc = set(re.findall(r"^    void (\w+)\(uint8_t \*rdram", libc.read_text(), re.M))
refs = {"ps2_stubs": set(), "ps2_syscalls": set()}
for f in gen.glob("FUN_*.cpp"):
    for ns, name in re.findall(r"\b(ps2_stubs|ps2_syscalls)::(\w+)\(", f.read_text()):
        refs[ns].add(name)
refs["ps2_stubs"] -= from_libc
with open(gen / "harness_stubs.cpp", "w", newline="\n") as f:
    f.write('#include "ps2_runtime.h"\n#include "ps2_runtime_macros.h"\n#include "harness.h"\n\n')
    for ns, names_ in refs.items():
        f.write(f"namespace {ns}\n{{\n")
        for n in sorted(names_):
            f.write(f'    void {n}(uint8_t *, R5900Context *ctx, PS2Runtime *) '
                    f'{{ harness::stubCalled("{ns}::{n}"); SET_GPR_S32(ctx, 2, 0); }}\n')
        f.write("}\n")

with open(gen / "harness_table.cpp", "w", newline="\n") as f:
    f.write('#include "ps2_runtime.h"\n#include "harness.h"\n\n')
    for name in names:
        f.write(f"void {name}(uint8_t *, R5900Context *, PS2Runtime *);\n")
    f.write("\nvoid harness::registerFunctions()\n{\n")
    for name in names:
        for addr in entries[name]:
            f.write(f"    g_table[0x{addr:x}u >> 2] = {name};\n")
    f.write("}\n")

print(f"entry 0x{entry:08x}, context {ctx_size} bytes: {len(wanted)} addresses -> "
      f"{len(names)} functions, {counted} instruction sites counted, variant {variant}"
      + (f", {dropped} pc stores dropped" if variant in ("v2", "v3") else ""))
