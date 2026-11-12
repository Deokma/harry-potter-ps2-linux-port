#!/usr/bin/env python3
"""Per-function profile of gsplay's own samples (GSPLAY_SAMPLE), also for a PIE build: each address inside the binary's
mappings (SAMPLES.maps) is turned back into a file address and looked up with nm; the rest is counted by module.

    python3 -I scripts/gs_sample_syms.py SAMPLES BINARY [--top N] [--nm NM] [--match REGEX]
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples")
    ap.add_argument("binary")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--match", help="also print the summed samples of the symbols matching this regex")
    a = ap.parse_args()
    data = open(a.samples, "rb").read()
    samples = struct.unpack("<%dQ" % (len(data) // 8), data)
    regions = []
    for line in open(a.samples + ".maps"):
        p = line.split()
        lo, hi = (int(x, 16) for x in p[0].split("-"))
        regions.append((lo, hi, int(p[2], 16), p[5] if len(p) > 5 else "[anon]"))
    regions.sort()
    starts = [r[0] for r in regions]
    name = os.path.basename(a.binary)
    syms = []
    for line in subprocess.run([a.nm, "-n", "-C", a.binary], capture_output=True, text=True).stdout.splitlines():
        p = line.split(" ", 2)
        if len(p) == 3 and p[0] and p[1] in ("t", "T", "w", "W"):
            syms.append((int(p[0], 16), p[2]))
    addrs = [s[0] for s in syms]
    by = collections.Counter()
    for s in samples:
        i = bisect.bisect_right(starts, s) - 1
        if i < 0 or s >= regions[i][1]:
            by["?"] += 1
            continue
        lo, hi, off, path = regions[i]
        if os.path.basename(path) != name:
            by["[" + os.path.basename(path) + "]"] += 1
            continue
        fa = s - lo + off
        j = bisect.bisect_right(addrs, fa) - 1
        by[syms[j][1][:110] if j >= 0 else "?bin"] += 1
    n = len(samples)
    print("%d samples" % n)
    if a.match:
        import re
        rx = re.compile(a.match)
        hit = sum(v for k, v in by.items() if rx.search(k))
        print("matching %r: %d samples (%.2f %%)" % (a.match, hit, 100.0 * hit / max(n, 1)))
    for k, v in by.most_common(a.top):
        print("%6.2f %%  %s" % (100.0 * v / n, k))


if __name__ == "__main__":
    main()
