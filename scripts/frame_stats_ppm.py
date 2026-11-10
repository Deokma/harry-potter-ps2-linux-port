#!/usr/bin/env python3
"""Numbers for a run's frame pictures (PPM, also when named .png), no viewing: per frame the share of lit pixels, the mean
brightness, and how much differs from the frame before (pixels changed by more than 24 in sum of channels).

  frame_stats_ppm.py DIR [EVERY]       every EVERY-th file (default 1)
"""
import os
import sys


def load(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        return None
    parts = data.split(None, 4)
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4][: w * h * 3]


def main():
    folder = sys.argv[1]
    every = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    names = sorted(n for n in os.listdir(folder) if n.endswith((".png", ".ppm")))[::every]
    prev = None
    for name in names:
        img = load(os.path.join(folder, name))
        if not img:
            print(f"{name}: not a PPM")
            continue
        w, h, px = img
        step = 3 * 7  # every 7th pixel is plenty
        samples = range(0, len(px) - 2, step)
        lit = sum(1 for i in samples if px[i] + px[i + 1] + px[i + 2] > 48)
        mean = sum(px[i] + px[i + 1] + px[i + 2] for i in samples) / (3 * len(samples))
        changed = ""
        if prev is not None and len(prev) == len(px):
            moved = sum(1 for i in samples if abs(px[i] - prev[i]) + abs(px[i + 1] - prev[i + 1]) + abs(px[i + 2] - prev[i + 2]) > 24)
            changed = f", changed {100.0 * moved / len(samples):.0f} %"
        print(f"{name}: lit {100.0 * lit / len(samples):.0f} %, mean {mean:.0f}{changed}")
        prev = px


main()
