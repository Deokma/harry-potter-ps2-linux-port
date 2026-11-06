#!/usr/bin/env python3
"""Decompress GS dumps for gsplay, and convert its PPM frames.

  gsdump_unpack.py unpack SRC_DIR OUT_DIR   every *.gs.zst / *.gs.xz under SRC_DIR -> OUT_DIR/NAME_N.gs
  gsdump_unpack.py sheet OUT.png FILE...    PPM or PNG files side by side, 4 across (needs Pillow)

Needs Python 3.14+ for zstd (compression.zstd), as gsdump_stats.py does. The output stays out of git:
it holds the game's data.
"""
import lzma
import sys
from pathlib import Path


def unpack(src, out):
    from compression import zstd
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for folder in sorted(Path(src).iterdir()):
        if not folder.is_dir():
            continue
        for n, f in enumerate(sorted(folder.glob("*.gs*"))):
            raw = f.read_bytes()
            if raw[:4] == b"\x28\xb5\x2f\xfd":
                raw = zstd.decompress(raw)
            elif raw[:6] == b"\xfd7zXZ\x00":
                raw = lzma.decompress(raw)
            target = out / f"{folder.name}_{n}.gs"
            target.write_bytes(raw)
            print(target.name, f.stem[-14:], len(raw) // 1024, "KiB")


def sheet(out, files):
    from PIL import Image
    tile = (320, 224)
    rows = (len(files) + 3) // 4
    canvas = Image.new("RGB", (4 * tile[0], rows * tile[1]))
    for i, f in enumerate(files):
        canvas.paste(Image.open(f).convert("RGB").resize(tile), ((i % 4) * tile[0], (i // 4) * tile[1]))
    canvas.save(out)


def main():
    if sys.argv[1] == "unpack":
        unpack(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "sheet":
        sheet(sys.argv[2], sys.argv[3:])


if __name__ == "__main__":
    main()
