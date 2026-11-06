#!/usr/bin/env python3
"""Fixes to PS2Recomp's generated code that a regeneration would lose. Safe to run twice.

The runtime builds from ps2xRuntime/src/runner/, a copy of trial/out-b/: run this on both.

- CP0 Count reads go through ps2portReadCount (a host clock); nothing else advances cop0_count.
  PS2Recomp's emitter is patched the same way, so this only matters for output made before that.
- 0x2600e8 gets a table entry. It is a two-instruction function (jr ra; move v0, a0) that the
  function map merged into FUN_00260008, so indirect calls to it found no target and the loading
  screen never finished. The table index is (address - 0x100040) / 4.

Addresses only, no game code. usage: ps2recomp_gen_fixes.py FOLDER...
"""
import re
import sys
from pathlib import Path

EXT = "extern uint32_t ps2portReadCount(R5900Context *);"
SIG = "void FUN_002600e8_0x2600e8(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime)"
BODY = (
    "\n// ps2port: see scripts/ps2recomp_gen_fixes.py\n" + SIG + " {\n"
    "    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 4));\n    ctx->pc = GPR_U32(ctx, 31);\n}\n"
)


def read(p):
    return p.read_text(encoding="utf-8", newline="")


def count_reads(folder):
    n = 0
    for p in folder.glob("FUN_*.cpp"):
        s = read(p)
        if "ctx->cop0_count" not in s:
            continue
        t, k = re.subn(r"SET_GPR_S32\(ctx, (\d+), \(int32_t\)ctx->cop0_count\);",
                       lambda m: "{ " + EXT + " SET_GPR_S32(ctx, " + m.group(1) + ", (int32_t)ps2portReadCount(ctx)); }", s)
        if k:
            p.write_text(t, encoding="utf-8", newline="")
            n += k
    return n


def table_entry(folder):
    changed = 0
    host = folder / "FUN_002600f0_0x2600f0.cpp"
    s = read(host)
    if "FUN_002600e8" not in s:
        host.write_text(s.rstrip() + "\n" + BODY, encoding="utf-8", newline="")
        changed += 1
    reg = folder / "register_functions.cpp"
    s = read(reg)
    if "FUN_002600e8" not in s:
        index = (0x2600e8 - 0x100040) // 4
        m = re.search(r"^(\s*)g_ps2RecompiledFunctionTable\[\d+\] = FUN_002600f0_0x2600f0;.*$", s, re.M)
        if not m:
            raise SystemExit("no table line for 0x2600f0 in " + str(reg))
        s = s[: m.start()] + f"{m.group(1)}g_ps2RecompiledFunctionTable[{index}] = FUN_002600e8_0x2600e8; // 0x2600e8\n" + s[m.start():]
        i = s.find("namespace {")
        s = s[:i] + SIG + ";\n" + s[i:]
        reg.write_text(s, encoding="utf-8", newline="")
        changed += 1
    return changed


def main():
    for arg in sys.argv[1:]:
        folder = Path(arg)
        print(folder.name, "count reads changed", count_reads(folder), "table edits", table_entry(folder))


if __name__ == "__main__":
    main()
