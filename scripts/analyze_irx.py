#!/usr/bin/env python3
"""List PS2 IOP modules: name/version, sizes, imported and exported libraries.

Imports are listed with the function indices each module uses; exports by
library name only.

Accepts .IRX files and IOPRP*.IMG reboot images (ROMDIR bundles; each
member is analysed too). This is the inventory of IOP services a native
runtime has to replace (HLE).

Usage:
    python scripts/analyze_irx.py FILE_OR_DIR... [--json OUT.json]
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

IMPORT_MAGIC = 0x41E00000
EXPORT_MAGIC = 0x41C00000
JR_RA = 0x03E00008
SHT_IOPMOD = 0x70000080


def _cstr(b):
    return b.split(b"\0", 1)[0].decode("ascii", "replace")


def iopmod_info(data):
    """Name/version/sizes from the .iopmod section of an IRX ELF."""
    if data[:4] != b"\x7fELF":
        return None
    e_type, = struct.unpack_from("<H", data, 16)
    e_shoff, = struct.unpack_from("<I", data, 32)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 46)
    info = {"elf_type": hex(e_type)}
    for i in range(e_shnum):
        sh = struct.unpack_from("<10I", data, e_shoff + i * e_shentsize)
        if sh[1] == SHT_IOPMOD:
            off = sh[4]
            _, entry, _, tsize, dsize, bsize, ver = \
                struct.unpack_from("<6IH", data, off)
            info.update({"name": _cstr(data[off + 26:off + 26 + 64]),
                         "version": f"{ver >> 8}.{ver & 0xFF:02d}",
                         "text": tsize, "data": dsize, "bss": bsize})
    return info


def lib_tables(data):
    """Scan for IOP import/export tables (word-aligned magic + name)."""
    imports, exports = {}, {}
    for off in range(0, len(data) - 24, 4):
        magic, nxt = struct.unpack_from("<II", data, off)
        if magic not in (IMPORT_MAGIC, EXPORT_MAGIC) or nxt != 0:
            continue
        ver, = struct.unpack_from("<H", data, off + 8)
        name = _cstr(data[off + 12:off + 20])
        if not re.fullmatch(r"[A-Za-z0-9_]{2,8}", name):
            continue
        p = off + 20
        if magic == IMPORT_MAGIC:
            funcs = []
            while p + 8 <= len(data):
                a, b = struct.unpack_from("<II", data, p)
                if a != JR_RA or b >> 16 != 0x2400:
                    break
                funcs.append(b & 0xFFFF)
                p += 8
            imports.setdefault(name, {"version": f"{ver >> 8}.{ver & 0xFF}",
                                      "functions": []})
            imports[name]["functions"] += funcs
        else:
            # Entry counts are not reported: modules are linked at 0, so an
            # unrelocated pointer to offset 0 is indistinguishable from the
            # table's NULL terminator without applying relocations.
            exports[name] = {"version": f"{ver >> 8}.{ver & 0xFF}"}
    for v in imports.values():
        v["functions"] = sorted(set(v["functions"]))
    return imports, exports


def analyze_module(name, data):
    info = {"file": name, "bytes": len(data)}
    mod = iopmod_info(data)
    if mod:
        info.update(mod)
    imports, exports = lib_tables(data)
    info["imports"] = imports
    info["exports"] = exports
    return info


def romdir(data):
    """Yield (name, bytes) for each member of a ROMDIR image."""
    entries, pos = [], 0
    while pos + 16 <= len(data):
        name, _, size = struct.unpack_from("<10sHI", data, pos)
        if name[0] == 0:
            break
        entries.append((_cstr(name), size))
        pos += 16
    off = 0
    for name, size in entries:
        yield name, data[off:off + size]
        off += (size + 15) & ~15


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    files = []
    for p in args.paths:
        if p.is_dir():
            files += sorted(f for f in p.rglob("*")
                            if f.suffix.upper() in (".IRX", ".IMG"))
        else:
            files.append(p)
    modules = []
    for f in files:
        data = f.read_bytes()
        if f.suffix.upper() == ".IMG" and data[:5] == b"RESET":
            for name, member in romdir(data):
                if member[:4] == b"\x7fELF":
                    m = analyze_module(f"{f.name}:{name}", member)
                    modules.append(m)
        else:
            modules.append(analyze_module(f.name, data))
    for m in modules:
        imp = ", ".join(f"{k}({len(v['functions'])})"
                        for k, v in sorted(m["imports"].items()))
        exp = ", ".join(sorted(m["exports"]))
        print(f"{m['file']:24s} {m.get('name', '?'):24s} "
              f"v{m.get('version', '?'):6s} text={m.get('text', 0):6d}")
        print(f"    exports: {exp or '-'}")
        print(f"    imports: {imp or '-'}")
    if args.json:
        args.json.write_text(json.dumps(modules, indent=2) + "\n",
                             newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
