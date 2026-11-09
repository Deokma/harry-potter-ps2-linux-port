#!/usr/bin/env python3
"""Fixes to PS2Recomp's generated code that a regeneration would lose. Safe to run twice.

The runtime builds from ps2xRuntime/src/runner/, a copy of trial/out-b/: run this on both.

- CP0 Count reads go through ps2portReadCount (a host clock); nothing else advances cop0_count.
  PS2Recomp's emitter is patched the same way, so this only matters for output made before that.
- 0x2600e8 gets its own small function and table entry. It is a two-instruction function
  (jr ra; move v0, a0) that the function map merged into FUN_00260008, so indirect calls to it found
  no target and the loading screen never finished.
- Other real functions that the map merged into the one before them (MID_ENTRIES) become entry
  points of the function that holds their code: a label, a case in its re-entry switch, and a table
  entry. Not every address has a case, so a table entry alone would run the wrong code. A new one
  shows up in a boot log as [guest-branch:missing-target]. The table index is (address - 0x100040) / 4.

Addresses only, no game code. usage: ps2recomp_gen_fixes.py FOLDER...
"""
import csv
import re
import sys
from pathlib import Path

EXT = "extern uint32_t ps2portReadCount(R5900Context *);"
SIG = "void FUN_002600e8_0x2600e8(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime)"
BODY = (
    "\n// ps2port: see scripts/ps2recomp_gen_fixes.py\n" + SIG + " {\n"
    "    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 4));\n    ctx->pc = GPR_U32(ctx, 31);\n}\n"
)
MID_ENTRIES = [0x1af300, 0x126428, 0x2a3180, 0x2fff98, 0x3020e8, 0x3021a0,
               0x28a140, 0x1ffdc0, 0x1ffdc8, 0x1ffdd0, 0x1ffdd8]  # last five: found 2026-11-09 by a lui/addiu scan
TABLE_BASE = 0x100040
FUNCTION_MAP = Path(__file__).resolve().parents[1] / "private/third_party/trial/function_map.csv"


def read(p):
    return p.read_text(encoding="utf-8", newline="")


def write(p, s):
    p.write_text(s, encoding="utf-8", newline="")


def count_reads(folder):
    n = 0
    for p in folder.glob("FUN_*.cpp"):
        s = read(p)
        if "ctx->cop0_count" not in s:
            continue
        t, k = re.subn(r"SET_GPR_S32\(ctx, (\d+), \(int32_t\)ctx->cop0_count\);",
                       lambda m: "{ " + EXT + " SET_GPR_S32(ctx, " + m.group(1) + ", (int32_t)ps2portReadCount(ctx)); }", s)
        if k:
            write(p, t)
            n += k
    return n


def table_entry(folder):
    changed = 0
    host = folder / "FUN_002600f0_0x2600f0.cpp"
    s = read(host)
    if "FUN_002600e8" not in s:
        write(host, s.rstrip() + "\n" + BODY)
        changed += 1
    reg = folder / "register_functions.cpp"
    s = read(reg)
    if "FUN_002600e8" not in s:
        index = (0x2600e8 - TABLE_BASE) // 4
        m = re.search(r"^(\s*)g_ps2RecompiledFunctionTable\[\d+\] = FUN_002600f0_0x2600f0;.*$", s, re.M)
        if not m:
            raise SystemExit("no table line for 0x2600f0 in " + str(reg))
        s = s[: m.start()] + f"{m.group(1)}g_ps2RecompiledFunctionTable[{index}] = FUN_002600e8_0x2600e8; // 0x2600e8\n" + s[m.start():]
        i = s.find("namespace {")
        s = s[:i] + SIG + ";\n" + s[i:]
        write(reg, s)
        changed += 1
    return changed


def containing(functions, address):
    for start, end, name in functions:
        if start <= address < end:
            return start, name
    return None


def mid_entries(folder, functions):
    changed = 0
    reg = folder / "register_functions.cpp"
    table = read(reg)
    for address in MID_ENTRIES:
        hit = containing(functions, address)
        if not hit:
            print(f"  0x{address:x}: in no function of the map")
            continue
        start, name = hit
        fn = f"{name}_0x{start:x}"
        p = folder / f"{fn}.cpp"
        s = read(p)
        case = f"case 0x{address:x}u: goto label_{address:x};"
        if case not in s:
            sw = re.search(r"switch \(ctx->pc\) \{\r?\n", s)
            if not sw:
                # PS2Recomp emits a re-entry switch only for functions with resume points: add an empty one before the
                # function's first pc store (0x28a140 lives in such a function).
                first = re.search(rf"^    ctx->pc = 0x{start:x}u;\r?\n", s, re.M)
                if not first:
                    print(f"  0x{address:x}: {fn} has no re-entry switch and no first pc store")
                    continue
                s = s[: first.start()] + "    switch (ctx->pc) {\n        default: break;\n    }\n\n" + s[first.start():]
                sw = re.search(r"switch \(ctx->pc\) \{\r?\n", s)
            if f"label_{address:x}:" not in s:
                c = re.search(rf"^\s*// 0x{address:x}: ", s, re.M)
                if not c:
                    print(f"  0x{address:x}: no instruction comment in {fn}")
                    continue
                s = s[: c.start()] + f"label_{address:x}:\n" + s[c.start():]
            s = s[: sw.end()] + f"        {case}\n" + s[sw.end():]
            write(p, s)
            changed += 1
        index = (address - TABLE_BASE) // 4
        line = f"g_ps2RecompiledFunctionTable[{index}] = {fn};"
        if line not in table:
            m = re.search(r"^(\s*)g_ps2RecompiledFunctionTable\[0\] = ", table, re.M)
            if not m:
                raise SystemExit("no table start in " + str(reg))
            table = table[: m.start()] + f"{m.group(1)}{line} // 0x{address:x}\n" + table[m.start():]
            changed += 1
    write(reg, table)
    return changed


def main():
    functions = [(int(r["Start"], 16), int(r["End"], 16), r["Name"]) for r in csv.DictReader(open(FUNCTION_MAP))]
    for arg in sys.argv[1:]:
        folder = Path(arg)
        print(folder.name, "count reads", count_reads(folder), "0x2600e8 edits", table_entry(folder),
              "mid-entry edits", mid_entries(folder, functions))


if __name__ == "__main__":
    main()
