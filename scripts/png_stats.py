#!/usr/bin/env python3
"""Numbers for PNG screenshots, no viewing: size, hash, share of non-black pixels, and the difference to a reference.

  png_stats.py FILE...                 one line per file
  png_stats.py --ref REF FILE...       also mean and max channel error and the share of pixels that differ by more than 8
"""
import hashlib
import struct
import sys
import zlib


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, w, h, depth, ctype = 8, b"", 0, 0, 8, 2
    while pos < len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    channels = {2: 3, 6: 4, 0: 1}.get(ctype)
    if depth != 8 or channels is None:
        raise SystemExit(f"{path}: only 8-bit grey, RGB or RGBA")
    raw = zlib.decompress(idat)
    stride = w * channels
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + (((line[i - channels] if i >= channels else 0) + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    rgb = bytearray()
    for line in rows:
        for x in range(w):
            px = line[x * channels:x * channels + channels]
            rgb += px[:3] if channels >= 3 else bytes((px[0],) * 3)
    return w, h, bytes(rgb)


def main():
    args = sys.argv[1:]
    ref = None
    if args[:1] == ["--ref"]:
        ref = load(args[1])
        args = args[2:]
    for path in args:
        w, h, rgb = load(path)
        lit = sum(1 for i in range(0, len(rgb), 3) if rgb[i] + rgb[i + 1] + rgb[i + 2] > 48)
        line = f"{path}: {w}x{h} md5 {hashlib.md5(rgb).hexdigest()[:8]} non-black {100.0 * lit / (w * h):.1f} %"
        if ref:
            if (ref[0], ref[1]) != (w, h):
                line += f"; reference is {ref[0]}x{ref[1]}: sizes differ, no pixel comparison"
            else:
                errs = [abs(a - b) for a, b in zip(rgb, ref[2])]
                bad = sum(1 for i in range(0, len(errs), 3) if max(errs[i:i + 3]) > 8)
                line += f"; vs reference: mean error {sum(errs) / len(errs):.2f}, max {max(errs)}, pixels over 8: {bad} ({100.0 * bad / (w * h):.2f} %)"
        print(line)


main()
