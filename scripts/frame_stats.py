#!/usr/bin/env python3
"""Frame-time distribution per stretch: python3 -I scripts/frame_stats.py RUN_DIR NAME:FROM-TO [NAME:FROM-TO ...] [--every S]

RUN_DIR holds frames.txt (PS2PORT_FRAME_LOG: ms since the first frame, frame ms) and freq.txt (device_panel.sh: seconds, kHz, cooling
state, CPU zone m-C, RssAnon kB, RssFile kB, Mali kB, /proc/stat). Times are seconds of the run; the frame log starts at the first game
frame, so it is shifted by the run time of that frame (from log.txt's first [m5f] line is not needed: frames.txt times are offset by
FIRST_S, given with --first, default 0). --every S prints one line per S seconds instead (the soak).
"""
import os
import sys


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def main():
    args = sys.argv[1:]
    every = None
    first = 0.0
    if "--every" in args:
        i = args.index("--every")
        every = float(args[i + 1])
        del args[i:i + 2]
    if "--first" in args:
        i = args.index("--first")
        first = float(args[i + 1])
        del args[i:i + 2]
    run = args[0]
    frames = []
    vblanks = []  # (seconds, the game's vblank count, vblanks its last frame took): the frame log's columns 3 and 4, when there
    for line in open(f"{run}/frames.txt"):
        p = line.split()
        if len(p) >= 2:
            frames.append((first + float(p[0]) / 1000.0, float(p[1])))
        if len(p) >= 4:
            vblanks.append((first + float(p[0]) / 1000.0, int(p[2]), int(p[3])))
    freq = []
    for line in (open(f"{run}/freq.txt") if os.path.exists(f"{run}/freq.txt") else []):
        p = line.split("cpu")[0].split()
        if len(p) >= 4:
            freq.append([float(x) for x in p[:7]] + [0.0] * (7 - len(p[:7])))

    def report(name, lo, hi):
        f = [ms for t, ms in frames if lo <= t < hi]
        q = [r for r in freq if lo <= r[0] < hi]
        if not f:
            print(f"{name}: no frames")
            return
        temp = f"{min(r[3] for r in q) / 1000:.1f}-{max(r[3] for r in q) / 1000:.1f} C" if q else "?"
        cool = f"{max(r[2] for r in q):.0f}" if q else "?"
        clock = f"{min(r[1] for r in q) / 1000:.0f}" if q else "?"
        mem = f", anon {q[-1][4] / 1024:.0f} MB, file {q[-1][5] / 1024:.0f} MB, mali {q[-1][6] / 1024:.0f} MB" if q and q[-1][4] else ""
        # game speed: the game's vblanks (60 a second on a PS2) against real time; and the vblanks a frame took, on average
        v = [x for x in vblanks if lo <= x[0] < hi]
        speed = ""
        if len(v) >= 2 and v[-1][0] > v[0][0]:
            # 0x328490 restarts every frame; the game's time is the sum of what each frame took (0x3287F0)
            speed = (f"; game speed {sum(x[2] for x in v[1:]) / 60.0 / (v[-1][0] - v[0][0]) * 100:.0f} %, "
                     f"{sum(x[2] for x in v) / len(v):.2f} vblanks a frame")
        print(f"{name} {lo:.0f}-{hi:.0f} s: {len(f)} frames, p50 {pct(f, 50):.1f} ms, p95 {pct(f, 95):.1f}, max {max(f):.1f}; "
              f"{temp}, cooling max {cool}, clock min {clock} MHz{mem}{speed}")

    if every:
        end = max(t for t, _ in frames)
        t = 0.0
        while t < end:
            report(f"{t:5.0f}", t, t + every)
            t += every
        return
    for spec in args[1:]:
        name, rng = spec.split(":")
        lo, hi = (float(x) for x in rng.split("-"))
        report(name, lo, hi)


if __name__ == "__main__":
    main()
