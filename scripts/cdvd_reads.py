#!/usr/bin/env python3
"""Map PCSX2 verbose disc reads to disc files and streaming rates.

With EmuCore.CdvdVerboseReads = true, PCSX2 logs every sector read as
  [ sec,frac] DvdRead: Reading Sector NNNNNNN (NNN Blocks of Size SSSS) ...
to emulog.txt. This attributes each read to a file via the layout.tsv
written by extract_disc.py and reports, per file and per time window, how
much was read and how fast. Times are host seconds from PCSX2 start; at
100 % speed they match emulated time.

Usage:
    python scripts/cdvd_reads.py EMULOG LAYOUT_TSV
        [--after "Loaded state"] [--window 5] [--json OUT.json]
"""

import argparse
import bisect
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

SECTOR = 2048  # user data per sector, whatever the raw block size logged
READ_RE = re.compile(r"\[\s*(\d+)[,.](\d+)\]\s+DvdRead: Reading Sector (\d+) "
                     r"\((\d+) Blocks of Size (\d+)\)")
STAMP_RE = re.compile(r"\[\s*(\d+)[,.](\d+)\]")


def load_layout(path):
    """Sorted (lba, sectors, path) for every file on the disc."""
    files = []
    with open(path, encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        col = {name: i for i, name in enumerate(header)}
        for line in f:
            row = line.rstrip("\n").split("\t")
            if row[col["kind"]] != "file":
                continue
            files.append((int(row[col["lba"]]), int(row[col["sectors"]]),
                          row[col["path"]]))
    files.sort()
    return files


def file_of(files, starts, lba):
    i = bisect.bisect_right(starts, lba) - 1
    if i >= 0:
        start, count, path = files[i]
        if lba < start + max(count, 1):
            return path
    return "(metadata / outside files)"


def parse_reads(path, after):
    """(marker time or None, [(time s, lba, sectors)]) for reads logged
    after the first line containing `after` (all reads if it is None)."""
    started = after is None
    t0 = None
    reads = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not started:
                if after in line:
                    started = True
                    m = STAMP_RE.search(line)
                    if m:
                        t0 = float(f"{m[1]}.{m[2]}")
                continue
            m = READ_RE.search(line)
            if m:
                reads.append((float(f"{m[1]}.{m[2]}"), int(m[3]), int(m[4])))
    if not started:
        sys.exit(f"marker {after!r} not found in {path}")
    return t0, reads


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("emulog", type=Path)
    ap.add_argument("layout", type=Path)
    ap.add_argument("--after", help="only count reads after the first line "
                                    "containing this text, e.g. 'Loaded state'")
    ap.add_argument("--window", type=float, default=5.0,
                    help="seconds per rate window (default 5)")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)

    files = load_layout(args.layout)
    starts = [f[0] for f in files]
    t0, reads = parse_reads(args.emulog, args.after)
    if not reads:
        sys.exit("no DvdRead lines: was EmuCore.CdvdVerboseReads on?")

    t_start = t0 if t0 is not None else reads[0][0]
    t_end = reads[-1][0]
    per_file = defaultdict(lambda: [0, 0, None, None])  # reads, sectors, first, last
    windows = defaultdict(int)
    for t, lba, count in reads:
        rec = per_file[file_of(files, starts, lba)]
        rec[0] += 1
        rec[1] += count
        rec[2] = t if rec[2] is None else rec[2]
        rec[3] = t
        windows[int((t - t_start) // args.window)] += count * SECTOR

    total = sum(r[1] for r in per_file.values()) * SECTOR
    span = max(t_end - t_start, 1e-9)
    print(f"{len(reads)} reads, {total / 1e6:.2f} MB in {span:.1f} s "
          f"(from t={t_start:.1f} s): mean {total / span / 1e3:.1f} kB/s")
    print(f"\n{'file':40s} {'reads':>6s} {'MB':>8s} {'first s':>8s} "
          f"{'last s':>8s} {'kB/s over its span':>19s}")
    rows = []
    for path, (n, sec, first, last) in sorted(per_file.items(),
                                              key=lambda kv: -kv[1][1]):
        nbytes = sec * SECTOR
        rate = nbytes / max(last - first, 1e-9) / 1e3 if last > first else None
        rows.append({"file": path, "reads": n, "bytes": nbytes,
                     "first_s": round(first - t_start, 3),
                     "last_s": round(last - t_start, 3),
                     "kB_per_s": None if rate is None else round(rate, 1)})
        print(f"{path:40s} {n:6d} {nbytes / 1e6:8.3f} {first - t_start:8.1f} "
              f"{last - t_start:8.1f} {'-' if rate is None else f'{rate:.1f}':>19s}")

    nwin = int(span // args.window) + 1
    rates = [windows.get(i, 0) / args.window / 1e3 for i in range(nwin)]
    print(f"\nkB/s per {args.window:g} s window: "
          + " ".join(f"{r:.0f}" for r in rates))
    print(f"max window {max(rates):.1f} kB/s")

    if args.json:
        args.json.write_text(json.dumps({
            "emulog": str(args.emulog), "after": args.after,
            "t_start": t_start, "t_end": t_end, "reads": len(reads),
            "bytes": total, "mean_kB_per_s": round(total / span / 1e3, 1),
            "window_s": args.window, "window_kB_per_s": [round(r, 1) for r in rates],
            "files": rows}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
