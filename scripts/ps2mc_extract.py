#!/usr/bin/env python3
"""Lists or extracts the saves on a PS2 memory card image (PCSX2's .ps2 files, with or without ECC bytes).

  ps2mc_extract.py CARD.ps2                 list directories and files with sizes and dates
  ps2mc_extract.py CARD.ps2 DIR OUT         copy save directory DIR (e.g. BASLUS-20826-HPATPS) to OUT/DIR/

The output folder layout is what the port's memory card stub reads (mc0/<DIR>/<files>). Save data is the user's own game
material: keep it under private/ or D:\\ps2port-data, never in the repository.
"""
import struct
import sys
from pathlib import Path


class Card:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if not self.data.startswith(b"Sony PS2 Memory Card Format"):
            raise SystemExit(f"{path}: not a PS2 memory card image")
        sb = self.data
        self.page_len, self.pages_per_cluster = struct.unpack_from("<HH", sb, 0x28)
        self.clusters_per_card, self.alloc_offset = struct.unpack_from("<II", sb, 0x30)
        self.rootdir_cluster, = struct.unpack_from("<I", sb, 0x3C)
        self.ifc = struct.unpack_from("<32I", sb, 0x50)
        pages = self.clusters_per_card * self.pages_per_cluster
        # with ECC every page is followed by 16 spare bytes
        self.raw_page = self.page_len + 16 if len(self.data) >= pages * (self.page_len + 16) else self.page_len
        self.cluster_len = self.page_len * self.pages_per_cluster

    def cluster(self, n):
        out = b""
        for p in range(self.pages_per_cluster):
            off = (n * self.pages_per_cluster + p) * self.raw_page
            out += self.data[off:off + self.page_len]
        return out

    def fat(self, n):
        per = self.cluster_len // 4
        ifc = self.ifc[(n // per) // per]
        indirect = struct.unpack_from("<I", self.cluster(ifc), ((n // per) % per) * 4)[0]
        return struct.unpack_from("<I", self.cluster(indirect), (n % per) * 4)[0]

    def chain(self, first, length):
        out, n = b"", first
        while len(out) < length:
            out += self.cluster(n + self.alloc_offset)
            nxt = self.fat(n)
            if nxt == 0xFFFFFFFF or not (nxt & 0x80000000):
                break
            n = nxt & 0x7FFFFFFF
        return out[:length]

    def entries(self, cluster, count):
        raw = self.chain(cluster, count * 512)
        for i in range(len(raw) // 512):
            e = raw[i * 512:(i + 1) * 512]
            mode, length = struct.unpack_from("<II", e, 0)
            first, = struct.unpack_from("<I", e, 0x10)
            modified = struct.unpack_from("<xBBBBBH", e, 0x18)  # sec, min, hour, day, month, year
            name = e[0x40:0x60].split(b"\0")[0].decode("ascii", "replace")
            yield mode, length, first, modified, name

    def root(self):
        first = next(self.entries(self.rootdir_cluster, 1))
        return [e for e in self.entries(self.rootdir_cluster, first[1]) if e[4] not in (".", "..") and e[0] & 0x8000]


def stamp(m):
    return "%04d-%02d-%02d %02d:%02d:%02d" % (m[5], m[4], m[3], m[2], m[1], m[0])


def main():
    card = Card(sys.argv[1])
    want = sys.argv[2] if len(sys.argv) > 3 else None
    for mode, length, first, modified, name in card.root():
        if not mode & 0x20:  # not a directory
            continue
        files = [e for e in card.entries(first, length) if e[4] not in (".", "..") and e[0] & 0x8000]
        if want is None:
            print(f"{name}/  {stamp(modified)}")
            for fmode, flen, ffirst, fmod, fname in files:
                print(f"    {fname:<32} {flen:>8}  {stamp(fmod)}")
        elif name == want:
            out = Path(sys.argv[3]) / name
            out.mkdir(parents=True, exist_ok=True)
            for fmode, flen, ffirst, fmod, fname in files:
                (out / fname).write_bytes(card.chain(ffirst, flen))
            print(f"{len(files)} files to {out}")


main()
