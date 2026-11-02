"""Per-function profile from the replay harness's PC samples (HARNESS_SAMPLE).

    python3 -I scripts/sample_profile.py BINARY SAMPLES [--nm NM] [--top N]
                                         [--hot FILE --cover PERCENT]

BINARY is the (static) harness the samples came from; SAMPLES holds raw
64-bit host program counters. Each sample is mapped to the symbol that
contains it (nm -n). Prints the top symbols with their share of samples.
With --hot, writes the guest addresses of the generated functions (symbols
named like FUN_002635d0 or EG_002635d0) that together cover PERCENT of the
samples, hottest first, one per line with their share.
"""
import argparse
import bisect
import collections
import re
import struct
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument("binary")
ap.add_argument("samples")
ap.add_argument("--nm", default="nm")
ap.add_argument("--top", type=int, default=30)
ap.add_argument("--hot")
ap.add_argument("--cover", type=float, default=90.0)
args = ap.parse_args()

syms = []
for line in subprocess.run([args.nm, "-n", "-C", "--defined-only", args.binary],
                           capture_output=True, text=True, check=True).stdout.split("\n"):
    parts = line.split(" ", 2)
    if len(parts) == 3 and parts[1] in "tTwW":
        syms.append((int(parts[0], 16), parts[2]))
addrs = [a for a, _ in syms]

data = open(args.samples, "rb").read()
pcs = struct.unpack(f"<{len(data) // 8}Q", data)
counts = collections.Counter()
for pc in pcs:
    i = bisect.bisect_right(addrs, pc) - 1
    counts[syms[i][1] if i >= 0 else "?"] += 1

total = len(pcs)
print(f"{total} samples")
cum = 0
for name, n in counts.most_common(args.top):
    cum += n
    print(f"{100 * n / total:6.2f}% {100 * cum / total:6.2f}%  {name[:90]}")

guest = re.compile(r"\b(?:FUN|EG)_([0-9a-fA-F]{8})")  # PS2Recomp's or eegen's
by_fn = collections.Counter()
for name, n in counts.items():
    m = guest.search(name)
    by_fn[int(m.group(1), 16) if m else None] += n
generated = sum(n for a, n in by_fn.items() if a is not None)
print(f"generated code {100 * generated / total:.1f}% in {sum(1 for a in by_fn if a is not None)} functions")

if args.hot:
    cum = 0
    with open(args.hot, "w", newline="\n") as f:
        for a, n in sorted(((a, n) for a, n in by_fn.items() if a is not None), key=lambda x: -x[1]):
            if 100 * cum / total >= args.cover:
                break
            cum += n
            f.write(f"0x{a:08x} {100 * n / total:.3f}\n")
    print(f"wrote {args.hot}: {100 * cum / total:.1f}% of samples")
