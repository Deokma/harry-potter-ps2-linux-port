#!/usr/bin/env python3
"""What changes between GIF packets of the same size in a raw dump (PS2PORT_GIF_RAW): python -I scripts/gif_raw_diff.py RAW [RAW...]

For each dump: packets by size, how many are repeats of an earlier one, and, between packets of the most common sizes, which words of the
qwords differ, as a share of the words compared. In the usual PACKED shape (ST, RGBAQ, XYZF2) that says whether texture coordinates,
colours or positions are what moves. Prints numbers only, plus a few example values.
"""
import collections
import struct
import sys


def packets(path):
    data = open(path, "rb").read()
    pos = 0
    out = []
    while pos + 8 <= len(data):
        pid, size = struct.unpack_from("<II", data, pos)
        pos += 8
        out.append((pid, data[pos:pos + size]))
        pos += size
    return out


for path in sys.argv[1:]:
    ps = packets(path)
    by = collections.defaultdict(list)
    for pid, d in ps:
        by[(pid, len(d))].append(d)
    print(path.split("/")[-2] if "/" in path else path, len(ps), "packets")
    for key, group in sorted(by.items(), key=lambda e: -len(e[1]))[:4]:
        uniq = len(set(group))
        print(f"  path {key[0]} size {key[1]}: {len(group)} packets, {uniq} distinct")
        if uniq < 2 or key[1] < 64:
            continue
        # the tag of the first GIF tag
        lo, hi = struct.unpack_from("<QQ", group[0], 0)
        print(f"    first tag nloop {lo & 0x7FFF} flg {lo >> 58 & 3} nreg {lo >> 60 & 15 or 16} regs {hi:#x}")
        words = key[1] // 4
        changed = collections.Counter()
        total = 0
        groups = list(dict.fromkeys(group))
        # the nearest earlier distinct packet: the one differing in the fewest words
        pairs = []
        for k in range(1, min(len(groups), 40)):
            best = None
            a = struct.unpack(f"<{words}I", groups[k])
            for j in range(max(0, k - 40), k):
                b = struct.unpack(f"<{words}I", groups[j])
                n = sum(1 for x, y in zip(a, b) if x != y)
                if best is None or n < best[0]:
                    best = (n, b)
            pairs.append((best[0], a, best[1]))
        print(f"    words differing from the nearest other packet, of {words}: {sorted(p[0] for p in pairs)[:16]}")
        for n, a, b in pairs:
            for w, (x, y) in enumerate(zip(a, b)):
                if x != y:
                    changed[w % 4] += 1
                    total += 1
        print(f"    of those differences, by word in the qword (0-3): {dict(sorted(changed.items()))}")
        n, a, b = min(pairs, key=lambda p: p[0] if p[0] else 1 << 30)
        shown = 0
        for w, (x, y) in enumerate(zip(a, b)):
            if x != y and shown < 6:
                print(f"    word {w} (qword {w // 4}.{w % 4}): {x:08x} {struct.unpack('<f', struct.pack('<I', x))[0]:g} / {y:08x} {struct.unpack('<f', struct.pack('<I', y))[0]:g}")
                shown += 1
