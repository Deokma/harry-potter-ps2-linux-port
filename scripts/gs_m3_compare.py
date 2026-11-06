#!/usr/bin/env python3
"""Measure gsplay's frames against the PCSX2 screenshot saved with each GS dump (needs Pillow).

  gs_m3_compare.py SNAPSHOTS_DIR FRAMES_DIR [SUFFIX]

For dump FOLDER/N (the N-th dump in FOLDER, sorted) the frame is FRAMES_DIR/FOLDER_N-SUFFIX.ppm. Prints
the mean absolute difference per channel on 0-255, after a 1 pixel blur that cancels the screenshot's
different scale. The screenshot may be a frame or so away from what the dump ends on, so moving things
always leave a little. Addresses and numbers only: nothing from the game goes into the output.
"""
import glob
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageFilter, ImageStat


def mean_difference(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) / 3.0


def main():
    snapshots, frames = Path(sys.argv[1]), Path(sys.argv[2])
    suffix = sys.argv[3] if len(sys.argv) > 3 else "gles"
    results = []
    for folder in sorted(p for p in snapshots.iterdir() if p.is_dir()):
        for n, dump in enumerate(sorted(glob.glob(str(folder / "*.gs*")))):
            name = f"{folder.name}_{n}"
            shot = Path(dump.rsplit(".gs", 1)[0] + ".png")
            frame = frames / f"{name}-{suffix}.ppm"
            if not shot.exists():
                print(f"{name:10s} no screenshot")
                continue
            if not frame.exists():
                print(f"{name:10s} no frame")
                continue
            a = Image.open(frame).convert("RGB")
            ref = Image.open(shot).convert("RGB").resize(a.size, Image.LANCZOS).filter(ImageFilter.GaussianBlur(1))
            d = mean_difference(a.filter(ImageFilter.GaussianBlur(1)), ref)
            results.append((name, d))
            print(f"{name:10s} {d:6.2f}")
    if results:
        values = sorted(d for _, d in results)
        print(f"dumps {len(values)}, median {values[len(values) // 2]:.2f}, worst {values[-1]:.2f}")


if __name__ == "__main__":
    main()
