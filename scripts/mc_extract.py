#!/usr/bin/env python3
"""PS2 memory card images (PCSX2's .ps2: 8 MB, 512-byte pages with 16 bytes of ECC each) to and from the runtime's mc0 folder.

  python3 -I scripts/mc_extract.py CARD.ps2                    list the saves (top-level folders) and their files
  python3 -I scripts/mc_extract.py CARD.ps2 OUT_DIR [NAME...]  copy the folders (all, or those named) to OUT_DIR/NAME/

The runtime's memory card (Kernel/Stubs/MemoryCard.cpp) is a host folder: mc0/ next to the game's ELF, one folder per save.
Format: superblock at page 0; clusters of 2 pages; a FAT reached through an indirect list; 512-byte directory entries.
"""
import os
import struct
import sys


class Card:
    def __init__(self, path):
        raw = open(path, "rb").read()
        if not raw.startswith(b"Sony PS2 Memory Card Format "):
            sys.exit(f"{path}: not a PS2 memory card image")
        self.page = struct.unpack_from("<H", raw, 0x28)[0]
        self.ppc = struct.unpack_from("<H", raw, 0x2A)[0]
        spare = len(raw) // (8 * 1024 * 1024 // self.page) - self.page  # 16 with ECC, 0 without
        self.stride = self.page + spare
        self.raw = raw
        self.alloc_offset = struct.unpack_from("<I", raw, 0x34)[0]
        self.root = struct.unpack_from("<I", raw, 0x3C)[0]
        self.ifc = struct.unpack_from("<32I", raw, 0x50)
        self.csize = self.page * self.ppc

    def cluster(self, n):
        out = b""
        for p in range(n * self.ppc, (n + 1) * self.ppc):
            out += self.raw[p * self.stride:p * self.stride + self.page]
        return out

    def fat(self, c):
        per = self.csize // 4
        ind = self.cluster(self.ifc[c // per // per])
        fat_cluster = struct.unpack_from("<I", ind, (c // per % per) * 4)[0]
        return struct.unpack_from("<I", self.cluster(fat_cluster), (c % per) * 4)[0]

    def chain(self, first):
        c = first
        seen = 0
        while seen < 65536:
            yield c
            v = self.fat(c)
            if v == 0xFFFFFFFF or not v & 0x80000000:
                return
            c = v & 0x7FFFFFFF
            if c == 0x7FFFFFFF:
                return
            seen += 1

    def read(self, first, length):
        data = b"".join(self.cluster(self.alloc_offset + c) for c in self.chain(first))
        return data[:length]

    def entries(self, first, count):
        data = self.read(first, count * 512)
        for i in range(count):
            e = data[i * 512:(i + 1) * 512]
            mode, = struct.unpack_from("<H", e, 0)
            length, = struct.unpack_from("<I", e, 4)
            cl, = struct.unpack_from("<I", e, 0x10)
            name = e[0x40:0x60].split(b"\0")[0].decode("ascii", "replace")
            if mode & 0x8000 and name not in (".", ".."):
                yield name, bool(mode & 0x20), length, cl


def main():
    card = Card(sys.argv[1])
    # the root directory's own entry is the first one in its cluster: its length is the number of entries
    root_len = struct.unpack_from("<I", card.cluster(card.alloc_offset + card.root), 4)[0]
    saves = list(card.entries(card.root, root_len))
    want = sys.argv[3:]
    for name, is_dir, length, cl in saves:
        if not is_dir:
            continue
        files = list(card.entries(cl, length))
        if len(sys.argv) < 3:
            print(f"{name}: " + ", ".join(f"{f} {n}" for f, _, n, _ in files))
            continue
        if want and name not in want:
            continue
        out = os.path.join(sys.argv[2], name)
        os.makedirs(out, exist_ok=True)
        for f, sub, n, fc in files:
            if not sub:
                open(os.path.join(out, f), "wb").write(card.read(fc, n))
        print(f"{name}: {len(files)} files to {out}")


if __name__ == "__main__":
    main()
