#!/usr/bin/env python3
"""Extract boot info and the file layout from a PS2 DVD image (ISO or CHD v5).

Reads the ISO9660 filesystem straight out of the image, so no 7z, xorriso or
chdman is needed. Parses SYSTEM.CNF, extracts the boot ELF and the IOP
modules (.IRX and the IOPRP*.IMG reboot image), and writes a deterministic
manifest and file listing:

    <out>/manifest.json   image, volume, SYSTEM.CNF, boot ELF and IOP info
    <out>/layout.tsv      every file and directory, sorted by LBA
    <out>/files/...       extracted files, keeping their on-disc paths

<out> defaults to ./private/extract/<SERIAL>/ (git-ignored).

Usage:
    python scripts/extract_disc.py IMAGE [--out DIR] [--force]
                                         [--extract GLOB]... [--all]
                                         [--hash] [--verify]
"""

import argparse
import binascii
import fnmatch
import hashlib
import json
import lzma
import re
import struct
import sys
import zlib
from array import array
from collections import Counter, OrderedDict
from functools import reduce
from operator import xor
from pathlib import Path

try:
    from compression import zstd  # Python 3.14+
except ImportError:
    zstd = None

SCRIPT_VERSION = 1
SECTOR = 2048
REPO_ROOT = Path(__file__).resolve().parent.parent
CHUNK_SECTORS = 512  # 1 MiB per read when copying files out


class ImageError(Exception):
    pass


# --------------------------------------------------------------------------
# Plain ISO image
# --------------------------------------------------------------------------

class IsoImage:
    def __init__(self, path):
        self.path = Path(path)
        self.f = open(self.path, "rb")
        self.size = self.path.stat().st_size
        self.sectors = self.size // SECTOR

    def read(self, lba, count=1):
        self.f.seek(lba * SECTOR)
        data = self.f.read(count * SECTOR)
        if len(data) != count * SECTOR:
            raise ImageError(f"short read at LBA {lba} (+{count})")
        return data

    def info(self):
        info = {"format": "iso", "file_bytes": self.size,
                "sectors": self.sectors}
        if self.size % SECTOR:
            info["trailing_bytes"] = self.size % SECTOR
        return info

    def iter_logical(self):
        self.f.seek(0)
        while chunk := self.f.read(1 << 20):
            yield chunk


# --------------------------------------------------------------------------
# CHD v5 image (MAME "compressed hunks of data", as made by chdman createdvd)
# Ported from libchdr (BSD-3-Clause): bitstream, huffman, v5 map, codecs.
# --------------------------------------------------------------------------

class BitReader:
    """MSB-first bit reader; reads past the end return zero bits."""

    def __init__(self, data):
        self.data = bytes(data) + b"\0" * 8
        self.end = len(data) * 8
        self.pos = 0

    def peek(self, n):
        if n == 0:
            return 0
        p = self.pos
        word = int.from_bytes(self.data[p >> 3:(p >> 3) + 8], "big")
        return (word >> (64 - (p & 7) - n)) & ((1 << n) - 1)

    def read(self, n):
        v = self.peek(n)
        self.pos += n
        return v

    def read_signed(self, n):
        if n == 0:
            return 0
        v = self.read(n)
        return v - (1 << n) if v >> (n - 1) else v

    def read_unary(self):
        """Count zero bits up to (and consume) the next one bit."""
        q = 0
        while True:
            v = self.peek(32)
            if v:
                z = 32 - v.bit_length()
                self.pos += z + 1
                return q + z
            q += 32
            self.pos += 32
            if self.pos > self.end + 64:
                raise ImageError("unary run past end of data")


class Huffman:
    def __init__(self, numcodes, maxbits):
        self.numcodes = numcodes
        self.maxbits = maxbits
        self.lengths = [0] * numcodes
        self.lookup = None

    def build(self):
        """Assign canonical codes and build the maxbits-wide lookup table."""
        histo = [0] * 33
        for length in self.lengths:
            if length > self.maxbits:
                raise ImageError("huffman: code longer than maxbits")
            histo[length] += 1
        start = 0
        for length in range(32, 0, -1):
            nxt = (start + histo[length]) >> 1
            if length != 1 and nxt * 2 != start + histo[length]:
                raise ImageError("huffman: inconsistent code lengths")
            histo[length] = start
            start = nxt
        lookup = [0] * (1 << self.maxbits)
        for sym, length in enumerate(self.lengths):
            if length:
                code = histo[length]
                histo[length] += 1
                shift = self.maxbits - length
                lookup[code << shift:(code + 1) << shift] = \
                    [(sym << 5) | length] * (1 << shift)
        self.lookup = lookup

    def decode_one(self, br):
        v = self.lookup[br.peek(self.maxbits)]
        br.pos += v & 0x1f
        return v >> 5

    def import_tree_rle(self, br):
        numbits = 5 if self.maxbits >= 16 else 4 if self.maxbits >= 8 else 3
        cur = 0
        while cur < self.numcodes:
            nodebits = br.read(numbits)
            if nodebits != 1:
                self.lengths[cur] = nodebits
                cur += 1
                continue
            nodebits = br.read(numbits)
            if nodebits == 1:
                self.lengths[cur] = 1
                cur += 1
                continue
            rep = br.read(numbits) + 3
            if cur + rep > self.numcodes:
                raise ImageError("huffman: RLE tree overflow")
            self.lengths[cur:cur + rep] = [nodebits] * rep
            cur += rep
        self.build()

    def import_tree_huffman(self, br):
        small = Huffman(24, 6)
        small.lengths[0] = br.read(3)
        start = br.read(3) + 1
        count = 0
        for i in range(1, 24):
            if i < start or count == 7:
                small.lengths[i] = 0
            else:
                count = br.read(3)
                small.lengths[i] = 0 if count == 7 else count
        small.build()
        rlefullbits = (self.numcodes - 9).bit_length()
        last = cur = 0
        while cur < self.numcodes:
            value = small.decode_one(br)
            if value:
                last = value - 1
                self.lengths[cur] = last
                cur += 1
            else:
                count = br.read(3) + 2
                if count == 9:
                    count += br.read(rlefullbits)
                count = min(count, self.numcodes - cur)
                self.lengths[cur:cur + count] = [last] * count
                cur += count
        self.build()


def _codec_huff(src, destlen):
    br = BitReader(src)
    h = Huffman(256, 16)
    h.import_tree_huffman(br)
    lookup, data, pos = h.lookup, br.data, br.pos
    out = bytearray(destlen)
    for i in range(destlen):
        b = pos >> 3
        word = (data[b] << 16) | (data[b + 1] << 8) | data[b + 2]
        v = lookup[(word >> (8 - (pos & 7))) & 0xffff]
        pos += v & 0x1f
        out[i] = v >> 5
    return bytes(out)


def _codec_zlib(src, destlen):
    return zlib.decompressobj(-15).decompress(src, destlen)


def _codec_lzma(src, destlen):
    # chdman: LZMA1, level 9 normalised for a hunk-sized input -> lc3 lp0 pb2,
    # raw stream, no end marker. dict_size only has to cover the hunk.
    d = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{
        "id": lzma.FILTER_LZMA1, "dict_size": max(destlen, 4096),
        "lc": 3, "lp": 0, "pb": 2}])
    return d.decompress(src, destlen)


def _codec_zstd(src, destlen):
    if zstd is None:
        raise ImageError("CHD uses zstd; needs Python 3.14+ (compression.zstd)")
    return zstd.decompress(src)


# chdman tries FLAC on every hunk (data viewed as 16-bit stereo PCM) and
# keeps it when it wins, which happens for near-constant sectors. The payload
# is bare FLAC frames; byte 0 says whether samples are stored 'L'ittle or
# 'B'ig endian. Pure-Python FLAC frame decoder (RFC 9639), stereo only.

_FLAC_FIXED = ([], [1], [2, -1], [3, -3, 1], [4, -6, 4, -1])
_FLAC_BPS = {0: 16, 1: 8, 2: 12, 4: 16, 5: 20, 6: 24, 7: 32}


def _flac_residual(br, n, order):
    method = br.read(2)
    if method > 1:
        raise ImageError("flac: reserved residual coding method")
    pbits, escape = (4, 15) if method == 0 else (5, 31)
    porder = br.read(4)
    out = []
    for p in range(1 << porder):
        count = (n >> porder) - (order if p == 0 else 0)
        k = br.read(pbits)
        if k == escape:
            nbits = br.read(5)
            out += [br.read_signed(nbits) for _ in range(count)]
            continue
        for _ in range(count):
            v = (br.read_unary() << k) | br.read(k)
            out.append((v >> 1) ^ -(v & 1))
    return out


def _flac_subframe(br, n, bps):
    br.read(1)
    kind = br.read(6)
    wasted = 0
    if br.read(1):
        wasted = br.read_unary() + 1
        bps -= wasted
    if kind == 0:
        s = [br.read_signed(bps)] * n
    elif kind == 1:
        s = [br.read_signed(bps) for _ in range(n)]
    elif 8 <= kind <= 12 or kind >= 32:
        order = kind - 8 if kind <= 12 else kind - 31
        s = [br.read_signed(bps) for _ in range(order)]
        if kind <= 12:
            coefs, shift = _FLAC_FIXED[order], 0
        else:
            precision = br.read(4) + 1
            shift = br.read_signed(5)
            coefs = [br.read_signed(precision) for _ in range(order)]
        for r in _flac_residual(br, n, order):
            acc = 0
            for j, c in enumerate(coefs):
                acc += c * s[-1 - j]
            s.append(r + (acc >> shift))
    else:
        raise ImageError(f"flac: reserved subframe type {kind}")
    return [v << wasted for v in s] if wasted else s


def _flac_frame(br):
    if br.read(15) != 0x7FFC:
        raise ImageError("flac: lost frame sync")
    br.read(1)  # blocking strategy
    bs_code, sr_code, ch_code = br.read(4), br.read(4), br.read(4)
    bps = _FLAC_BPS.get(br.read(3))
    br.read(1)
    lead, first = 0, br.read(8)  # UTF-8 style frame/sample number
    while lead < 7 and first & (0x80 >> lead):
        lead += 1
    br.read(8 * max(0, lead - 1))
    if bs_code == 1:
        blocksize = 192
    elif 2 <= bs_code <= 5:
        blocksize = 576 << (bs_code - 2)
    elif bs_code == 6:
        blocksize = br.read(8) + 1
    elif bs_code == 7:
        blocksize = br.read(16) + 1
    elif bs_code >= 8:
        blocksize = 256 << (bs_code - 8)
    else:
        raise ImageError("flac: reserved block size")
    if sr_code == 12:
        br.read(8)
    elif sr_code in (13, 14):
        br.read(16)
    if bps is None or ch_code not in (1, 8, 9, 10):
        raise ImageError("flac: unsupported sample size / channel layout")
    br.read(8)  # header CRC-8
    a = _flac_subframe(br, blocksize, bps + (ch_code == 9))
    b = _flac_subframe(br, blocksize, bps + (ch_code in (8, 10)))
    br.pos = (br.pos + 7) & ~7
    br.read(16)  # frame CRC-16
    if ch_code == 8:  # left / side
        return a, [x - y for x, y in zip(a, b)]
    if ch_code == 9:  # side / right
        return [x + y for x, y in zip(a, b)], b
    if ch_code == 10:  # mid / side
        left, right = [], []
        for m, s in zip(a, b):
            m = (m << 1) | (s & 1)
            left.append((m + s) >> 1)
            right.append((m - s) >> 1)
        return left, right
    return a, b


def _codec_flac(src, destlen):
    if src[:1] not in (b"L", b"B"):
        raise ImageError("flac: bad endianness marker")
    br = BitReader(src[1:])
    want = destlen // 4
    left, right = [], []
    while len(left) < want:
        l, r = _flac_frame(br)
        left += l
        right += r
    pcm = array("h")
    for l, r in zip(left[:want], right[:want]):
        pcm.append(l)
        pcm.append(r)
    if (src[:1] == b"B") != (sys.byteorder == "big"):
        pcm.byteswap()
    return pcm.tobytes()


CODECS = {b"huff": _codec_huff, b"zlib": _codec_zlib, b"lzma": _codec_lzma,
          b"zstd": _codec_zstd, b"flac": _codec_flac}

# v5 map entry types
C_TYPE3, C_NONE, C_SELF, C_PARENT = 3, 4, 5, 6
C_RLE_SMALL, C_RLE_LARGE = 7, 8
C_SELF_0, C_SELF_1, C_PARENT_SELF, C_PARENT_0, C_PARENT_1 = 9, 10, 11, 12, 13
C_ZERO = 255  # internal: uncompressed-CHD hunk that is all zeros


class ChdImage:
    def __init__(self, path, cache_hunks=256):
        self.path = Path(path)
        self.f = open(self.path, "rb")
        hdr = self.f.read(124)
        if hdr[:8] != b"MComprHD":
            raise ImageError("not a CHD file")
        _, version = struct.unpack(">II", hdr[8:16])
        if version != 5:
            raise ImageError(f"CHD v{version} not supported, only v5 "
                             "(re-create it with a current chdman)")
        self.compressors = [hdr[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logical, self.mapoff, self.metaoff = \
            struct.unpack(">QQQ", hdr[32:56])
        self.hunkbytes, self.unitbytes = struct.unpack(">II", hdr[56:64])
        self.rawsha1 = hdr[64:84].hex()
        self.sha1 = hdr[84:104].hex()
        if hdr[104:124] != bytes(20):
            raise ImageError("CHD with a parent image is not supported")
        if self.unitbytes != SECTOR:
            raise ImageError(f"CHD unit size {self.unitbytes}: this is a CD "
                             "CHD (createcd); only DVD CHDs are supported")
        self.hunkcount = -(-self.logical // self.hunkbytes)
        self.sectors = self.logical // SECTOR
        self._load_map()
        self.cache = OrderedDict()
        self.cache_hunks = cache_hunks

    def _load_map(self):
        n = self.hunkcount
        self.types = bytearray(n)
        self.offs = [0] * n
        self.lens = [0] * n
        self.crcs = [None] * n
        f = self.f
        if self.compressors[0] == bytes(4):
            f.seek(self.mapoff)
            raw = f.read(4 * n)
            for i in range(n):
                off = int.from_bytes(raw[4 * i:4 * i + 4], "big")
                self.types[i] = C_NONE if off else C_ZERO
                self.offs[i] = off * self.hunkbytes
                self.lens[i] = self.hunkbytes
            return

        f.seek(self.mapoff)
        h = f.read(16)
        mapbytes = int.from_bytes(h[0:4], "big")
        firstoffs = int.from_bytes(h[4:10], "big")
        mapcrc = int.from_bytes(h[10:12], "big")
        lengthbits, selfbits, parentbits = h[12], h[13], h[14]
        br = BitReader(f.read(mapbytes))

        # pass 1: compression type per hunk (huffman + RLE)
        dec = Huffman(16, 8)
        dec.import_tree_rle(br)
        types = self.types
        rep = last = 0
        for i in range(n):
            if rep:
                types[i] = last
                rep -= 1
                continue
            v = dec.decode_one(br)
            if v == C_RLE_SMALL:
                types[i] = last
                rep = 2 + dec.decode_one(br)
            elif v == C_RLE_LARGE:
                types[i] = last
                rep = 2 + 16 + (dec.decode_one(br) << 4)
                rep += dec.decode_one(br)
            else:
                types[i] = last = v

        # pass 2: offsets, lengths, CRCs; rebuild libchdr's raw map for the CRC
        raw = bytearray(12 * n)
        cur = firstoffs
        last_self = last_parent = 0
        units_per_hunk = self.hunkbytes // self.unitbytes
        for i in range(n):
            t = types[i]
            off, length, crc = cur, 0, 0
            if t <= C_TYPE3:
                length = br.read(lengthbits)
                cur += length
                crc = br.read(16)
            elif t == C_NONE:
                length = self.hunkbytes
                cur += length
                crc = br.read(16)
            elif t == C_SELF:
                last_self = off = br.read(selfbits)
            elif t == C_PARENT:
                last_parent = off = br.read(parentbits)
            elif t in (C_SELF_0, C_SELF_1):
                if t == C_SELF_1:
                    last_self += 1
                t, off = C_SELF, last_self
            elif t == C_PARENT_SELF:
                t = C_PARENT
                last_parent = off = i * self.hunkbytes // self.unitbytes
            elif t in (C_PARENT_0, C_PARENT_1):
                if t == C_PARENT_1:
                    last_parent += units_per_hunk
                t, off = C_PARENT, last_parent
            types[i] = t
            self.offs[i], self.lens[i] = off, length
            if t <= C_NONE:
                self.crcs[i] = crc
            struct.pack_into(">BBHHIH", raw, 12 * i, t, length >> 16,
                             length & 0xffff, off >> 32, off & 0xffffffff, crc)
        if binascii.crc_hqx(raw, 0xffff) != mapcrc:
            raise ImageError("CHD map CRC mismatch (corrupt file or "
                             "decoder bug)")

    def codec_stats(self):
        names = {i: c.decode("ascii", "replace")
                 for i, c in enumerate(self.compressors)}
        names.update({C_NONE: "uncompressed", C_SELF: "duplicate-of-hunk",
                      C_PARENT: "parent", C_ZERO: "zero"})
        stats = Counter(names.get(t, f"type{t}") for t in self.types)
        return dict(sorted(stats.items(), key=lambda kv: -kv[1]))

    def hunk(self, i):
        data = self.cache.get(i)
        if data is not None:
            self.cache.move_to_end(i)
            return data
        t = self.types[i]
        if t <= C_TYPE3:
            codec = self.compressors[t]
            fn = CODECS.get(codec)
            if fn is None:
                raise ImageError(
                    f"hunk {i} uses codec {codec!r}, which this script does "
                    "not implement; convert with 'chdman extractdvd' instead")
            self.f.seek(self.offs[i])
            data = fn(self.f.read(self.lens[i]), self.hunkbytes)
        elif t == C_NONE:
            self.f.seek(self.offs[i])
            data = self.f.read(self.hunkbytes)
        elif t == C_SELF:
            data = self.hunk(self.offs[i])
        elif t == C_ZERO:
            data = bytes(self.hunkbytes)
        else:
            raise ImageError(f"hunk {i} references a parent CHD")
        if len(data) != self.hunkbytes:
            raise ImageError(f"hunk {i}: decoded {len(data)} bytes, "
                             f"expected {self.hunkbytes}")
        crc = self.crcs[i]
        if crc is not None and binascii.crc_hqx(data, 0xffff) != crc:
            raise ImageError(f"hunk {i}: CRC16 mismatch")
        self.cache[i] = data
        if len(self.cache) > self.cache_hunks:
            self.cache.popitem(last=False)
        return data

    def read(self, lba, count=1):
        start, size = lba * SECTOR, count * SECTOR
        if start + size > self.logical:
            raise ImageError(f"read past end of CHD at LBA {lba} (+{count})")
        hb = self.hunkbytes
        first, last = start // hb, (start + size - 1) // hb
        buf = b"".join(self.hunk(h) for h in range(first, last + 1))
        skip = start - first * hb
        return buf[skip:skip + size]

    def info(self):
        return {"format": "chd", "chd_version": 5,
                "file_bytes": self.path.stat().st_size,
                "logical_bytes": self.logical, "sectors": self.sectors,
                "hunk_bytes": self.hunkbytes, "hunks": self.hunkcount,
                "compressors": [c.decode("ascii", "replace").strip("\0")
                                for c in self.compressors if c != bytes(4)],
                "hunk_codec_usage": self.codec_stats(),
                "raw_sha1_from_header": self.rawsha1}

    def iter_logical(self):
        remaining = self.logical
        for i in range(self.hunkcount):
            data = self.hunk(i)
            yield data[:remaining]
            remaining -= len(data)


def open_image(path):
    with open(path, "rb") as f:
        magic = f.read(8)
    if magic == b"MComprHD":
        return ChdImage(path)
    return IsoImage(path)


# --------------------------------------------------------------------------
# ISO9660
# --------------------------------------------------------------------------

def _str(b):
    return b.decode("ascii", "replace").rstrip(" \0")


def _dir_date(b):
    """7-byte directory record date -> ISO 8601 string."""
    y, mo, d, h, mi, s, tz = struct.unpack("6Bb", b)
    if mo == 0:
        return None
    tz *= 15
    return (f"{1900 + y:04d}-{mo:02d}-{d:02d}T{h:02d}:{mi:02d}:{s:02d}"
            f"{'+' if tz >= 0 else '-'}{abs(tz) // 60:02d}:{abs(tz) % 60:02d}")


def _vd_date(b):
    """17-byte volume descriptor date -> ISO 8601 string."""
    digits = b[:16].decode("ascii", "replace")
    if not digits.strip("0 \0"):
        return None
    tz = struct.unpack("b", b[16:17])[0] * 15
    return (f"{digits[0:4]}-{digits[4:6]}-{digits[6:8]}T{digits[8:10]}:"
            f"{digits[10:12]}:{digits[12:14]}.{digits[14:16]}"
            f"{'+' if tz >= 0 else '-'}{abs(tz) // 60:02d}:{abs(tz) % 60:02d}")


def read_volume_descriptors(img, base=0):
    """Return (pvd dict, list of descriptor type codes)."""
    pvd, kinds = None, []
    for lba in range(base + 16, base + 64):
        sec = img.read(lba)
        if sec[1:6] != b"CD001":
            raise ImageError(f"no ISO9660 descriptor at LBA {lba}")
        kinds.append(sec[0])
        if sec[0] == 1 and pvd is None:
            root = sec[156:190]
            pvd = {
                "system_id": _str(sec[8:40]),
                "volume_id": _str(sec[40:72]),
                "volume_space_sectors": struct.unpack_from("<I", sec, 80)[0],
                "logical_block_size": struct.unpack_from("<H", sec, 128)[0],
                "volume_set_id": _str(sec[190:318]),
                "publisher_id": _str(sec[318:446]),
                "data_preparer_id": _str(sec[446:574]),
                "application_id": _str(sec[574:702]),
                "created": _vd_date(sec[813:830]),
                "modified": _vd_date(sec[830:847]),
                "root_lba": struct.unpack_from("<I", root, 2)[0],
                "root_bytes": struct.unpack_from("<I", root, 10)[0],
            }
        if sec[0] == 255:
            break
    if pvd is None:
        raise ImageError("no primary volume descriptor")
    if pvd["logical_block_size"] != SECTOR:
        raise ImageError(f"block size {pvd['logical_block_size']} != 2048")
    return pvd, kinds


def _dir_records(data, size):
    pos = 0
    while pos < size:
        rlen = data[pos]
        if rlen == 0:  # records never span sectors: skip the padding
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        rec = data[pos:pos + rlen]
        pos += rlen
        if rec[33:33 + rec[32]] not in (b"\0", b"\1"):
            yield rec


def walk_tree(img, root_lba, root_bytes):
    """Return a list of entries for every file and directory under root."""
    entries, warnings, seen = [], [], set()
    stray_last_flags = []

    def walk(lba, size, prefix):
        if lba in seen:
            warnings.append(f"directory loop at LBA {lba} ({prefix or '/'})")
            return
        seen.add(lba)
        records = list(_dir_records(img.read(lba, -(-size // SECTOR)), size))
        i = 0
        while i < len(records):
            rec = records[i]
            raw_name = rec[33:33 + rec[32]]
            flags = rec[25]
            extents = [(struct.unpack_from("<I", rec, 2)[0],
                        struct.unpack_from("<I", rec, 10)[0])]
            # 0x80 = "more records follow for this file". Only a real
            # multi-extent file when the next record carries the same name;
            # some PS2 mastering tools set the bit on every file.
            while flags & 0x80 and i + 1 < len(records) and \
                    records[i + 1][33:33 + records[i + 1][32]] == raw_name:
                i += 1
                flags = records[i][25]
                extents.append((struct.unpack_from("<I", records[i], 2)[0],
                                struct.unpack_from("<I", records[i], 10)[0]))
            i += 1
            name = raw_name.decode("ascii", "replace")
            is_dir = bool(flags & 0x02)
            if not is_dir:
                name = re.sub(r";\d+$", "", name).rstrip(".") or name
            path = f"{prefix}/{name}"
            if flags & 0x80:
                stray_last_flags.append(path)
            entry = {"path": path, "lba": extents[0][0],
                     "bytes": sum(n for _, n in extents), "dir": is_dir,
                     "date": _dir_date(rec[18:25]),
                     "hidden": bool(flags & 0x01)}
            if len(extents) > 1:
                entry["extents"] = extents
            entries.append(entry)
            if is_dir:
                walk(extents[0][0], extents[0][1], path)

    walk(root_lba, root_bytes, "")
    if stray_last_flags:
        warnings.append(f"{len(stray_last_flags)} records have the "
                        "multi-extent flag (0x80) without a continuation "
                        "record; treated as single-extent (mastering quirk)")
    entries.sort(key=lambda e: (e["lba"], e["path"]))
    return entries, warnings


def find_entry(entries, path):
    want = path.strip("/").upper()
    for e in entries:
        if not e["dir"] and e["path"].strip("/").upper() == want:
            return e
    return None


def read_file(img, entry):
    """Yield the file's bytes in chunks."""
    for lba, remaining in entry.get("extents",
                                    [(entry["lba"], entry["bytes"])]):
        while remaining > 0:
            n = min(CHUNK_SECTORS, -(-remaining // SECTOR))
            chunk = img.read(lba, n)[:remaining]
            yield chunk
            remaining -= len(chunk)
            lba += n


def read_whole(img, entry):
    return b"".join(read_file(img, entry))


# --------------------------------------------------------------------------
# SYSTEM.CNF and ELF
# --------------------------------------------------------------------------

def parse_system_cnf(text):
    fields = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            fields[k.strip().upper()] = v.strip()
    return fields


def boot_path(boot2):
    """'cdrom0:\\SLES_520.55;1' -> 'SLES_520.55'."""
    p = re.sub(r"^cdrom0?:", "", boot2.strip(), flags=re.I)
    p = re.sub(r";\d+$", "", p.replace("\\", "/"))
    return p.strip("/")


def serial_from_boot(path):
    """'SLES_520.55' -> 'SLES-52055'."""
    m = re.match(r"([A-Z]{4})_(\d{3})\.(\d{2})$", Path(path).name.upper())
    return f"{m[1]}-{m[2]}{m[3]}" if m else None


def pcsx2_elf_crc(data):
    """PCSX2's game CRC: XOR of every little-endian 32-bit word of the ELF."""
    words = array("I", data[:len(data) // 4 * 4])
    if sys.byteorder == "big":
        words.byteswap()
    return reduce(xor, words, 0)


PT_NAMES = {0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE",
            6: "PHDR", 0x70000000: "MIPS_REGINFO"}


def elf_summary(data):
    if data[:4] != b"\x7fELF":
        return {"valid": False}
    if data[4] != 1 or data[5] != 1:
        return {"valid": True, "note": "not a 32-bit little-endian ELF"}
    (e_type, e_machine, _, e_entry, e_phoff, e_shoff, e_flags, _,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = \
        struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    phdrs = []
    for i in range(e_phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = \
            struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        phdrs.append({"type": PT_NAMES.get(p_type, hex(p_type)),
                      "offset": p_off, "vaddr": hex(p_vaddr),
                      "filesz": p_filesz, "memsz": p_memsz,
                      "flags": "".join(c for c, bit in
                                       (("R", 4), ("W", 2), ("X", 1))
                                       if p_flags & bit)})
    sections = []
    if e_shnum and e_shoff + e_shnum * e_shentsize <= len(data):
        shdrs = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize)
                 for i in range(e_shnum)]
        strtab = b""
        if e_shstrndx < e_shnum:
            off, size = shdrs[e_shstrndx][4], shdrs[e_shstrndx][5]
            strtab = data[off:off + size]
        for sh in shdrs:
            name = strtab[sh[0]:strtab.find(b"\0", sh[0])].decode(
                "ascii", "replace") if strtab else ""
            sections.append(name)
    return {"valid": True, "type": e_type, "machine": e_machine,
            "flags": hex(e_flags), "entry": hex(e_entry),
            "program_headers": phdrs, "section_count": e_shnum,
            "section_names": sections}


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def sha256(data):
    return hashlib.sha256(data).hexdigest()


def hash_image(img):
    md5, sha1, crc = hashlib.md5(), hashlib.sha1(), 0
    for chunk in img.iter_logical():
        md5.update(chunk)
        sha1.update(chunk)
        crc = zlib.crc32(chunk, crc)
    return {"md5": md5.hexdigest(), "sha1": sha1.hexdigest(),
            "crc32": f"{crc:08x}"}


def write_file(out_root, entry, img):
    dest = out_root / "files" / entry["path"].lstrip("/")
    dest.parent.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha256()
    with open(dest, "wb") as f:
        for chunk in read_file(img, entry):
            f.write(chunk)
            h.update(chunk)
    return h.hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", help="PS2 disc image (.iso or .chd)")
    ap.add_argument("--out", type=Path,
                    help="output dir (default: private/extract/<SERIAL>)")
    ap.add_argument("--force", action="store_true",
                    help="reuse an output dir that came from another image")
    ap.add_argument("--extract", action="append", default=[], metavar="GLOB",
                    help="also extract files whose disc path matches GLOB "
                         "(case-insensitive, e.g. '*.VAG'); repeatable")
    ap.add_argument("--all", action="store_true",
                    help="extract every file on the disc")
    ap.add_argument("--hash", action="store_true",
                    help="hash the whole image (md5/sha1/crc32, as on redump)")
    ap.add_argument("--verify", action="store_true",
                    help="CHD only: decode every hunk and check the SHA-1 "
                         "stored in the CHD header")
    args = ap.parse_args(argv)

    img = open_image(args.image)
    image_info = {"file_name": Path(args.image).name, **img.info()}

    pvd, kinds = read_volume_descriptors(img)
    volume = {**pvd, "descriptor_types": kinds}
    warnings = []
    layer0 = pvd["volume_space_sectors"]
    if layer0 + 16 < img.sectors:
        try:
            pvd1, _ = read_volume_descriptors(img, layer0)
            volume["layer1"] = pvd1
            warnings.append("dual-layer disc: only layer 0 is walked")
        except ImageError:
            pass

    entries, tree_warnings = walk_tree(img, pvd["root_lba"],
                                       pvd["root_bytes"])
    warnings += tree_warnings
    files = [e for e in entries if not e["dir"]]

    cnf_entry = find_entry(entries, "SYSTEM.CNF")
    if cnf_entry is None:
        raise ImageError("SYSTEM.CNF not found: not a PS2 game disc?")
    cnf_text = read_whole(img, cnf_entry).decode("ascii", "replace")
    cnf = parse_system_cnf(cnf_text)
    if "BOOT2" not in cnf:
        raise ImageError("SYSTEM.CNF has no BOOT2 (PS1 disc?)")
    elf_path = boot_path(cnf["BOOT2"])
    serial = serial_from_boot(elf_path) or Path(elf_path).name
    elf_entry = find_entry(entries, elf_path)
    if elf_entry is None:
        raise ImageError(f"boot ELF {elf_path} not found on disc")

    out = args.out or REPO_ROOT / "private" / "extract" / serial
    manifest_path = out / "manifest.json"
    if manifest_path.exists() and not args.force:
        old = json.loads(manifest_path.read_text())
        old_src = old.get("image", {}).get("file_name")
        if old_src != image_info["file_name"]:
            sys.exit(f"{out} was made from '{old_src}'; pass --out or --force")
    out.mkdir(parents=True, exist_ok=True)

    elf_data = read_whole(img, elf_entry)
    write_file(out, cnf_entry, img)
    write_file(out, elf_entry, img)
    boot = {"path": elf_entry["path"], "lba": elf_entry["lba"],
            "bytes": elf_entry["bytes"], "sha256": sha256(elf_data),
            "pcsx2_crc": f"{pcsx2_elf_crc(elf_data):08X}",
            "elf": elf_summary(elf_data)}

    patterns = [p.upper() for p in args.extract]
    if args.all:
        patterns.append("*")
    iop, extra = [], 0
    for e in files:
        upath = e["path"].upper()
        # IOP side: loose modules plus the IOPRP reboot image (module bundle)
        if upath.endswith(".IRX") or re.search(r"/IOPRP\w*\.IMG$", upath):
            digest = write_file(out, e, img)
            iop.append({"path": e["path"], "lba": e["lba"],
                        "bytes": e["bytes"], "sha256": digest})
        elif any(fnmatch.fnmatchcase(upath, p) or
                 fnmatch.fnmatchcase(upath.lstrip("/"), p) for p in patterns):
            if e is not cnf_entry and e is not elf_entry:
                write_file(out, e, img)
                extra += 1

    by_ext = Counter()
    bytes_by_ext = Counter()
    for e in files:
        ext = Path(e["path"]).suffix.upper() or "(none)"
        by_ext[ext] += 1
        bytes_by_ext[ext] += e["bytes"]
    data_end = max((e["lba"] + -(-e["bytes"] // SECTOR) for e in entries),
                   default=0)

    manifest = {
        "script": {"name": "extract_disc.py", "version": SCRIPT_VERSION},
        "image": image_info,
        "volume": volume,
        "system_cnf": {"raw": cnf_text, "fields": cnf},
        "serial": serial,
        "boot_elf": boot,
        "iop_modules": iop,
        "filesystem": {
            "files": len(files),
            "directories": sum(e["dir"] for e in entries),
            "file_bytes": sum(e["bytes"] for e in files),
            "data_end_lba": data_end,
            "padding_sectors_after_data": img.sectors - data_end,
            "by_extension": {k: {"files": by_ext[k], "bytes": bytes_by_ext[k]}
                             for k in sorted(by_ext,
                                             key=lambda k: -bytes_by_ext[k])},
        },
        "extracted_extra_files": extra,
        "warnings": warnings,
    }
    if args.hash:
        manifest["image"]["hashes"] = hash_image(img)
    if args.verify:
        if not isinstance(img, ChdImage):
            sys.exit("--verify only applies to CHD images")
        digest = hashlib.sha1()
        for chunk in img.iter_logical():
            digest.update(chunk)
        ok = digest.hexdigest() == img.rawsha1
        manifest["image"]["verify_raw_sha1"] = "ok" if ok else "MISMATCH"
        if not ok:
            warnings.append("CHD raw SHA-1 mismatch after full decode")

    with open(out / "layout.tsv", "w", newline="\n") as f:
        f.write("lba\tsectors\tbytes\tdate\tkind\tpath\n")
        for e in entries:
            f.write(f"{e['lba']}\t{-(-e['bytes'] // SECTOR)}\t{e['bytes']}\t"
                    f"{e['date'] or ''}\t{'dir' if e['dir'] else 'file'}\t"
                    f"{e['path']}\n")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n",
                             newline="\n")

    print(f"image     : {image_info['file_name']} ({image_info['format']}, "
          f"{img.sectors} sectors)")
    print(f"serial    : {serial}  VER={cnf.get('VER', '?')}  "
          f"VMODE={cnf.get('VMODE', '?')}")
    print(f"boot ELF  : {elf_entry['path']}  {elf_entry['bytes']} bytes  "
          f"entry={boot['elf'].get('entry')}  pcsx2_crc={boot['pcsx2_crc']}")
    print(f"files     : {len(files)} files, "
          f"{manifest['filesystem']['directories']} dirs, "
          f"{manifest['filesystem']['file_bytes'] / 2**20:.1f} MiB")
    print(f"IOP       : {len(iop)} files (IRX modules + IOPRP image)")
    for w in warnings:
        print(f"warning   : {w}")
    print(f"output    : {out}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ImageError as exc:
        sys.exit(f"error: {exc}")
