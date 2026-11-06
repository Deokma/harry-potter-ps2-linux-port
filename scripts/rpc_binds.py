#!/usr/bin/env python3
"""List sceSifBindRpc / sceSifCallRpc call sites in an R5900 ELF.

For each `jal bind`, follows a1 backwards through lui/ori/addiu to the server id and prints
the call site and the enclosing function. For the servers named with --sid, also lists every
function that calls sceSifCallRpc on a client struct set up by the same function group.

usage: rpc_binds.py ELF BIND_ADDR CALL_ADDR [--functions hp.cvs-like map]
Prints addresses only, no game data.
"""
import struct
import sys


def segments(path):
    b = open(path, "rb").read()
    phoff = struct.unpack_from("<I", b, 28)[0]
    phnum = struct.unpack_from("<H", b, 44)[0]
    segs = []
    for i in range(phnum):
        t, off, va, _pa, fs, _ms, flags, _al = struct.unpack_from("<8I", b, phoff + 32 * i)
        if t == 1 and flags & 1:
            segs.append((va, b[off : off + fs]))
    return segs


def func_start(words, base, i):
    # walk back to the `addiu sp, sp, -N` that opens the function
    for j in range(i, max(i - 400, 0), -1):
        w = words[j]
        if w >> 16 == 0x27BD and (w & 0x8000):
            return base + 4 * j
    return 0


def main():
    elf = sys.argv[1]
    bind = int(sys.argv[2], 16)
    call = int(sys.argv[3], 16)
    jbind = 0x0C000000 | (bind >> 2)
    jcall = 0x0C000000 | (call >> 2)
    for base, data in segments(elf):
        words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
        calls = {}
        for i, w in enumerate(words):
            if w == jbind:
                # a1 = register 5; look at the delay slot and up to 12 instructions before
                hi = lo = None
                for j in range(i + 1, max(i - 12, 0), -1):
                    x = words[j]
                    op, rs, rt, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
                    if rt != 5:
                        continue
                    if op == 0x0F and hi is None:
                        hi = imm
                    elif op == 0x0D and lo is None and rs == 5:
                        lo = imm
                    elif op == 0x09 and lo is None and rs == 5:
                        lo = imm - 0x10000 if imm & 0x8000 else imm
                sid = ((hi or 0) << 16) + (lo or 0) & 0xFFFFFFFF if hi is not None else None
                print("bind site=0x%06x func=0x%06x sid=%s" % (
                    base + 4 * i, func_start(words, base, i),
                    "0x%08x" % sid if sid is not None else "?"))
            elif w == jcall:
                f = func_start(words, base, i)
                calls[f] = calls.get(f, 0) + 1
        print("callrpc functions:", " ".join("0x%06x*%d" % (f, n) for f, n in sorted(calls.items())))


if __name__ == "__main__":
    main()
