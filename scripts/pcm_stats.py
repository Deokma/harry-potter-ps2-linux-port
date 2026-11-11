#!/usr/bin/env python3
"""Numbers for a PC run's sound, no listening: the mixer's output (PS2PORT_SPU_PCM, raw 16-bit stereo 48 kHz) against a log of
that file's size over real time, written while the run goes (lines "SECONDS BYTES", see runner/try_sound.sh).

  pcm_stats.py PCM SIZELOG [WINDOW]     per WINDOW seconds of real time (default 10): the mix rate in frames a second (48,000
                                        is real time) and the loudness (RMS and peak of both channels; 0 is silence)
"""
import math
import struct
import sys


def main():
    pcm_path, log_path = sys.argv[1], sys.argv[2]
    window = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
    samples = []
    for line in open(log_path):
        parts = line.split()
        if len(parts) == 2:
            samples.append((float(parts[0]), int(parts[1])))
    if len(samples) < 2:
        print("size log too short")
        return
    data = open(pcm_path, "rb")
    t0 = samples[0][0]
    start_t, start_b = samples[0]
    total_frames = 0
    for t, b in samples[1:]:
        if t - start_t < window and (t, b) != samples[-1]:
            continue
        frames = (b - start_b) // 4
        rate = frames / (t - start_t) if t > start_t else 0.0
        data.seek(start_b - start_b % 4)
        raw = data.read(frames * 4)
        n = len(raw) // 2
        rms = peak = 0.0
        if n:
            vals = struct.unpack("<%dh" % n, raw[: n * 2])
            step = max(1, n // 200000)  # enough samples for a level
            picked = vals[::step]
            rms = math.sqrt(sum(v * v for v in picked) / len(picked))
            peak = max(abs(v) for v in picked)
        total_frames += frames
        print(f"{start_t - t0:6.0f}-{t - t0:4.0f} s: mixed {rate:7.0f} frames/s ({100.0 * rate / 48000:3.0f} %), rms {rms:6.0f}, peak {peak:5.0f}")
        start_t, start_b = t, b
    span = samples[-1][0] - t0
    print(f"whole run: {total_frames / span if span else 0:.0f} frames/s over {span:.0f} s")


main()
