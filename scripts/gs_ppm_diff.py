#!/usr/bin/env python3
"""Compares two PPM pictures by numbers, no viewing: size, pixels that differ, largest channel difference, mean of each.

  gs_ppm_diff.py A.ppm B.ppm      prints one line; exit 0 if identical, 1 if not
"""
import sys


def load(path):
    data = open(path, "rb").read()
    parts = data.split(None, 4)
    if parts[0] != b"P6":
        raise SystemExit("not P6: " + path)
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4][: w * h * 3]


def main():
    wa, ha, a = load(sys.argv[1])
    wb, hb, b = load(sys.argv[2])
    if (wa, ha) != (wb, hb):
        print("size differs: %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 1
    diff = 0
    worst = 0
    for i in range(0, len(a), 3):
        if a[i:i + 3] != b[i:i + 3]:
            diff += 1
            worst = max(worst, abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
    n = wa * ha
    print("%dx%d: %d of %d pixels differ (%.4f %%), max channel diff %d, means %.3f / %.3f" %
          (wa, ha, diff, n, 100.0 * diff / n, worst, sum(a) / len(a), sum(b) / len(b)))
    return 0 if diff == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
