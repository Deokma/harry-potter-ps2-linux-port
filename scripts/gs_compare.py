#!/usr/bin/env python3
"""Compare frames from gsplay (PPM or PNG, needs Pillow).

  gs_compare.py A B [--sheet OUT.png]   mean absolute difference and share of differing pixels
  gs_compare.py --pairs OUT.png A1 B1 A2 B2 ...   the same per pair, and a sheet of them side by side
"""
import sys

from PIL import Image, ImageChops, ImageStat


def compare(a_path, b_path):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        b = b.resize(a.size, Image.BILINEAR)
    diff = ImageChops.difference(a, b)
    mean = sum(ImageStat.Stat(diff).mean) / 3.0
    gray = diff.convert("L")
    hist = gray.histogram()
    total = a.size[0] * a.size[1]
    over8 = sum(hist[9:]) / total * 100.0
    over32 = sum(hist[33:]) / total * 100.0
    return mean, over8, over32, a.size


def main():
    if sys.argv[1] == "--pairs":
        out = sys.argv[2]
        files = sys.argv[3:]
        tile = (240, 180)
        rows = (len(files) + 3) // 4
        sheet = Image.new("RGB", (4 * tile[0], rows * tile[1]))
        for i in range(0, len(files), 2):
            try:
                mean, o8, o32, size = compare(files[i], files[i + 1])
                print(f"{files[i].split('/')[-1]:28s} mean {mean:6.2f}  over8 {o8:5.1f}%  over32 {o32:5.1f}%  {size[0]}x{size[1]}")
            except OSError as e:
                print(files[i], "missing", e)
                continue
            for k in (0, 1):
                j = i + k
                sheet.paste(Image.open(files[j]).convert("RGB").resize(tile), ((j % 4) * tile[0], (j // 4) * tile[1]))
        sheet.save(out)
    else:
        mean, o8, o32, size = compare(sys.argv[1], sys.argv[2])
        print(f"mean {mean:.2f}, over 8: {o8:.1f}%, over 32: {o32:.1f}%, {size[0]}x{size[1]}")


if __name__ == "__main__":
    main()
