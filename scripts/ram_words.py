#!/usr/bin/env python3
"""Prints 32-bit guest words from RAM images: a raw 32 MB dump, a harness recording (.in/.out: RAM at the end), or a PCSX2
savestate (.p2s, a zip with eeMemory.bin).

  ram_words.py FILE ADDR[=NAME] [ADDR[=NAME] ...]       e.g.  ram_words.py x.p2s 0x38c194=mode 0x38c210=tick
"""
import struct
import sys
import zipfile

RAM = 32 * 1024 * 1024


def load(path):
    if path.endswith(".p2s"):
        with zipfile.ZipFile(path) as z:
            name = next(n for n in z.namelist() if n.lower().startswith("eememory"))
            return z.read(name)
    data = open(path, "rb").read()
    return data[len(data) - RAM:] if len(data) > RAM else data


def main():
    ram = load(sys.argv[1])
    out = []
    for spec in sys.argv[2:]:
        addr, _, name = spec.partition("=")
        a = int(addr, 0) & (RAM - 1)
        v = struct.unpack_from("<I", ram, a)[0]
        out.append(f"{name or hex(a)} {v} (0x{v:x})")
    print("; ".join(out))


main()
