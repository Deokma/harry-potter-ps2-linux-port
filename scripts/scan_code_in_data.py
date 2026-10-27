#!/usr/bin/env python3
"""Look for MIPS (EE/IOP) code or VU microcode hidden in a disc's data files.

For every file on the image, counts word-aligned instruction signatures and
compares them with what random bytes would give:

    jr_ra      03E00008               (function return), random: 2^-32/word
    sp_frame   addiu sp,sp,-N         random: 2^-17/word
    save_ra    sw/sd/sq ra,N(sp)      random: 3*2^-16/word
    vu_nop     upper NOP 0x000002FF   (VU microcode), random: 2^-32/word

Also reports a zlib compressibility ratio of the first MiB: data that is
already compressed (ratio near 1.0) could hide code from this scan.

Usage:
    python scripts/scan_code_in_data.py IMAGE [--json OUT.json] [--top N]
"""

import argparse
import json
import re
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_disc import (open_image, read_file, read_volume_descriptors,
                          walk_tree)

PATTERNS = {
    "jr_ra": re.compile(re.escape(b"\x08\x00\xe0\x03")),
    "sp_frame": re.compile(rb"[\x00-\xff][\x80-\xff]\xbd\x27", re.S),
    "save_ra": re.compile(rb"[\x00-\xff][\x00-\xff]\xbf[\xaf\xff\x7f]", re.S),
    "vu_nop": re.compile(re.escape(b"\xff\x02\x00\x00")),
}
RANDOM_PER_WORD = {"jr_ra": 2 ** -32, "sp_frame": 2 ** -17,
                   "save_ra": 3 * 2 ** -16, "vu_nop": 2 ** -32}


def scan(data):
    hits = {}
    for key, rx in PATTERNS.items():
        if key == "vu_nop":  # upper word of a 64-bit VU instruction
            hits[key] = sum(1 for m in rx.finditer(data) if m.start() % 8 == 4)
        else:
            hits[key] = sum(1 for m in rx.finditer(data) if m.start() % 4 == 0)
    return hits


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args(argv)
    img = open_image(args.image)
    pvd, _ = read_volume_descriptors(img)
    entries, _ = walk_tree(img, pvd["root_lba"], pvd["root_bytes"])
    results = []
    for e in entries:
        if e["dir"] or not e["bytes"]:
            continue
        data = b"".join(read_file(img, e))
        hits = scan(data)
        words = len(data) / 4
        sample = data[:1 << 20]
        ratio = len(zlib.compress(sample, 1)) / max(1, len(sample))
        expected = {k: RANDOM_PER_WORD[k] * words for k in hits}
        results.append({
            "path": e["path"], "bytes": e["bytes"], "hits": hits,
            "expected_random": {k: round(v, 3) for k, v in expected.items()},
            "jr_ra_per_mib": round(hits["jr_ra"] / (len(data) / 2 ** 20), 3),
            "zlib_ratio_first_mib": round(ratio, 3)})
    results.sort(key=lambda r: -r["jr_ra_per_mib"])
    print(f"{'file':34s} {'MiB':>7s} {'jr_ra':>6s} {'/MiB':>7s} "
          f"{'sp_frm':>7s} {'(rnd)':>6s} {'save_ra':>7s} {'(rnd)':>6s} "
          f"{'vu_nop':>6s} {'zlib':>5s}")
    for r in results[:args.top]:
        h, x = r["hits"], r["expected_random"]
        print(f"{r['path'][-34:]:34s} {r['bytes'] / 2**20:7.1f} "
              f"{h['jr_ra']:6d} {r['jr_ra_per_mib']:7.2f} "
              f"{h['sp_frame']:7d} {x['sp_frame']:6.0f} "
              f"{h['save_ra']:7d} {x['save_ra']:6.0f} {h['vu_nop']:6d} "
              f"{r['zlib_ratio_first_mib']:5.2f}")
    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + "\n",
                             newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
