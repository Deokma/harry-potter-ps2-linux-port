#!/usr/bin/env python3
"""Walk a game frame's VIF1 DMA chain in a saved EE RAM image and sum up its work.

    python -I scripts/vif_chain.py STATE [--tadr 0xADDR]

STATE: a savestate or replay state (see diff_ram_regions.py). The chain address comes
from the word the game keeps it in (0x3284F4, this game's NTSC executable) unless given.
With TTE set the upper half of each DMA tag holds two VIFcodes, then the tag's data
follows as more VIFcodes and their payloads. Prints tag and VIFcode counts, the payload
of DIRECT (GS image data on PATH2) and UNPACK (VU1 input), and each MSCAL's address.
"""
import argparse
import collections
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from diff_ram_regions import load_ram

TAGS = "refe cnt next ref refs call ret end".split()
NAMES = {0x00: "NOP", 0x01: "STCYCL", 0x02: "OFFSET", 0x03: "BASE", 0x04: "ITOP", 0x05: "STMOD",
         0x06: "MSKPATH3", 0x07: "MARK", 0x10: "FLUSHE", 0x11: "FLUSH", 0x13: "FLUSHA", 0x14: "MSCAL",
         0x15: "MSCALF", 0x17: "MSCNT", 0x20: "STMASK", 0x30: "STROW", 0x31: "STCOL", 0x4A: "MPG",
         0x50: "DIRECT", 0x51: "DIRECTHL"}


class Vif:
    """A VIF1 parser over a stream of 32-bit words, across DMA packets."""

    def __init__(self):
        self.ops = collections.Counter()
        self.ibits = collections.Counter()
        self.pending = 0             # words of payload still to skip
        self.mscal = collections.Counter()
        self.direct_bytes = self.direct_hl_bytes = self.unpack_words = self.mpg_bytes = 0
        self.unpack_vectors = 0
        self.mscal_vectors = []      # vectors unpacked since the last MSCAL
        self.since = 0
        self.formats = collections.Counter()
        self.cl = self.wl = 1

    def code(self, w):
        cmd = (w >> 24) & 0x7F
        imm, num = w & 0xFFFF, (w >> 16) & 0xFF
        if w & 0x80000000:           # the interrupt bit: VIF1 stalls and raises INT_VIF1
            self.ibits[NAMES.get(cmd, "UNPACK" if cmd >= 0x60 else f"?{cmd:02x}")] += 1
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            bits = ((32 >> vl) * (vn + 1)) * n
            words = (bits + 31) // 32
            self.ops["UNPACK"] += 1
            self.unpack_words += words
            self.unpack_vectors += n
            self.since += n
            self.formats[f"V{vn + 1}-{32 >> vl}"] += n
            self.pending = words
            return
        name = NAMES.get(cmd, f"?{cmd:02x}")
        self.ops[name] += 1
        if cmd == 0x01:
            self.cl, self.wl = imm & 0xFF, (imm >> 8) & 0xFF
        elif cmd == 0x20:
            self.pending = 1
        elif cmd in (0x30, 0x31):
            self.pending = 4
        elif cmd == 0x4A:
            n = num or 256
            self.pending = n * 2
            self.mpg_bytes += n * 8
        elif cmd in (0x50, 0x51):
            n = imm or 65536
            self.pending = n * 4
            if cmd == 0x50:
                self.direct_bytes += n * 16
            else:
                self.direct_hl_bytes += n * 16
        elif cmd in (0x14, 0x15):
            self.mscal[imm] += 1
            self.mscal_vectors.append(self.since)
            self.since = 0

    def feed(self, words):
        for w in words:
            if self.pending:
                self.pending -= 1
            else:
                self.code(w)


def walk(ram, tadr, tte=True, limit=2000000):
    vif = Vif()
    tags = collections.Counter()
    stack = []
    qwords = 0
    addr = tadr & 0x1FFFFFF0
    for _ in range(limit):
        lo, hi = struct.unpack_from("<QQ", ram, addr)
        qwc, tag_id = lo & 0xFFFF, (lo >> 28) & 7
        ptr = (lo >> 32) & 0x1FFFFFF0
        tags[TAGS[tag_id]] += 1
        if tte:
            vif.feed(struct.unpack_from("<II", ram, addr + 8))
        follows = addr + 16
        if tag_id in (0, 3, 4):      # refe, ref, refs: data elsewhere
            data = ptr
        else:
            data = follows
        if (lo >> 32) & 0x80000000 and tag_id in (0, 3, 4):
            return vif, tags, qwords, "data in scratchpad"
        vif.feed(struct.unpack_from(f"<{qwc * 4}I", ram, data))
        qwords += qwc
        after = follows + qwc * 16
        if tag_id in (0, 7):         # refe, end
            return vif, tags, qwords, "ended"
        if tag_id == 1:              # cnt
            addr = after
        elif tag_id == 2:            # next
            addr = ptr
        elif tag_id in (3, 4):       # ref, refs
            addr = follows
        elif tag_id == 5:            # call
            stack.append(after)
            addr = ptr
        elif tag_id == 6:            # ret
            if not stack:
                return vif, tags, qwords, "ended by ret"
            addr = stack.pop()
    return vif, tags, qwords, "tag limit reached"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("state")
    ap.add_argument("--tadr", type=lambda v: int(v, 0))
    args = ap.parse_args(argv)
    ram = load_ram(args.state)
    tadr = args.tadr if args.tadr is not None else struct.unpack_from("<I", ram, 0x3284F4)[0]
    vif, tags, qwords, how = walk(ram, tadr)
    print(f"chain at {tadr:#x}: {sum(tags.values())} tags, {qwords} qwords, {how}; tags {dict(tags)}")
    print(f"VIFcodes {dict(vif.ops.most_common(14))}")
    print(f"with the interrupt bit: {dict(vif.ibits)}")
    print(f"DIRECT {vif.direct_bytes} bytes, DIRECTHL {vif.direct_hl_bytes} bytes, MPG {vif.mpg_bytes} bytes, "
          f"UNPACK {vif.unpack_vectors} vectors in {vif.unpack_words * 4} bytes, formats {dict(vif.formats.most_common(6))}")
    n = len(vif.mscal_vectors)
    print(f"MSCAL x{n} at {[hex(k) for k, _ in vif.mscal.most_common(10)]} x{[v for _, v in vif.mscal.most_common(10)]}")
    if n:
        print(f"vectors per MSCAL: mean {sum(vif.mscal_vectors) / n:.1f}, max {max(vif.mscal_vectors)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
