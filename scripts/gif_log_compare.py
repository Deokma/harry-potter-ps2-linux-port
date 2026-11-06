#!/usr/bin/env python3
"""Compares two GIF packet logs (PS2PORT_GIF_LOG: path, size, hash per packet) by path, as sets of hashes.

    python -I scripts/gif_log_compare.py REFERENCE OTHER

Logs differ between boots in timing, so a packet-by-packet comparison means little. What should hold: the packets one run draws,
the other draws too. Prints packets, distinct hashes, and how many of each run's distinct hashes the other has, also weighted by how
often they occur.
"""
import collections
import sys


def load(path):
    by_path = collections.defaultdict(collections.Counter)
    for line in open(path, errors="replace"):
        parts = line.split()
        if len(parts) == 3:
            by_path[parts[0]][parts[2]] += 1
    return by_path


def main():
    a = load(sys.argv[1])
    b = load(sys.argv[2])
    print("path | packets ref, other | distinct ref, other | ref's distinct in other | other's distinct in ref | ref's packets in other | other's packets in ref")
    for path in sorted(set(a) | set(b)):
        ca, cb = a[path], b[path]
        na, nb = sum(ca.values()), sum(cb.values())
        common = set(ca) & set(cb)
        wa = sum(ca[h] for h in common)
        wb = sum(cb[h] for h in common)
        print(f"{path} | {na}, {nb} | {len(ca)}, {len(cb)} | {100 * len(common) / max(1, len(ca)):.1f}% | {100 * len(common) / max(1, len(cb)):.1f}% | {100 * wa / max(1, na):.1f}% | {100 * wb / max(1, nb):.1f}%")


if __name__ == "__main__":
    main()
