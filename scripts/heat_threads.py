#!/usr/bin/env python3
"""Per-thread CPU, wakeups and context switches of a running process, over time windows (Linux /proc; PC or device).

  python3 -I scripts/heat_threads.py PID_OR_NAME WINDOWS [--out FILE] [--wait S]

PID_OR_NAME: a pid, or a process name (comm, e.g. runner) to wait for (up to --wait seconds, default 60).
WINDOWS: comma-separated FROM-TO pairs in seconds since the process started, e.g. 40-70,225-265.
For each window and each thread (threads of the same name are summed, with a count) it prints:
  cpu %      CPU time over wall time, from schedstat (ns; utime+stime ticks when schedstat is missing)
  run-wait % time spent runnable but not running (CPU contention on the host; schedstat)
  wake/s     voluntary context switches a second: how often the thread went to sleep and was woken (waits, polls, sleeps)
  invol/s    involuntary context switches a second (preempted)
  slices/s   times it was put on a CPU a second (schedstat)
  us/wake    CPU microseconds per voluntary switch: a few microseconds means a poll loop that wakes for nothing
A thread that is busy without doing work (spinning) shows high cpu with few wakes; a short-sleep poll loop shows many wakes and
little CPU per wake. Numbers are for the machine they ran on: PC numbers are not device numbers.
"""
import os
import sys
import time


def find_pid(name, wait):
    if name.isdigit():
        return int(name)
    end = time.time() + wait
    while time.time() < end:
        for d in os.listdir("/proc"):
            if not d.isdigit():
                continue
            try:
                with open(f"/proc/{d}/comm") as f:
                    if f.read().strip() == name:
                        return int(d)
            except OSError:
                pass
        time.sleep(0.2)
    return None


def start_seconds(pid):
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(")", 1)[1].split()
    ticks = int(fields[19])  # starttime is field 22; after ")" the list starts at field 3
    with open("/proc/uptime") as f:
        up = float(f.read().split()[0])
    return time.time() - (up - ticks / os.sysconf("SC_CLK_TCK"))


def snapshot(pid):
    out = {}
    tick_ns = 1e9 / os.sysconf("SC_CLK_TCK")
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return None
    for tid in tids:
        base = f"/proc/{pid}/task/{tid}"
        try:
            with open(base + "/comm") as f:
                name = f.read().strip()
            vol = invol = 0
            with open(base + "/status") as f:
                for line in f:
                    if line.startswith("voluntary_ctxt_switches"):
                        vol = int(line.split()[1])
                    elif line.startswith("nonvoluntary_ctxt_switches"):
                        invol = int(line.split()[1])
            run = wait = slices = None
            try:
                with open(base + "/schedstat") as f:
                    run, wait, slices = (int(x) for x in f.read().split()[:3])
            except OSError:
                pass
            if run is None:
                with open(base + "/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
                run = (int(fields[11]) + int(fields[12])) * tick_ns
                wait = slices = 0
            out[tid] = (name, run, wait, slices, vol, invol)
        except (OSError, ValueError):
            continue  # a thread that ended while it was read
    return out


def report(a, b, span, label, out):
    rows = {}
    for tid, after in b.items():
        before = a.get(tid)
        if before is None:
            before = (after[0], 0, 0, 0, 0, 0)  # a thread born inside the window
        name = after[0]
        r = rows.setdefault(name, [0, 0.0, 0.0, 0, 0, 0])
        r[0] += 1
        for i in range(1, 6):
            r[i] += after[i] - before[i]
    lines = [f"== {label} ({span:.1f} s wall)",
             f"{'thread':<18}{'n':>3}{'cpu %':>8}{'run-wait %':>11}{'wake/s':>9}{'invol/s':>9}{'slices/s':>10}{'us/wake':>9}"]
    total = 0.0
    for name, r in sorted(rows.items(), key=lambda kv: -kv[1][1]):
        cpu = 100.0 * r[1] / 1e9 / span
        total += cpu
        if cpu < 0.05 and r[4] == 0:
            continue
        per_wake = (r[1] / 1e3 / r[4]) if r[4] else 0.0
        lines.append(f"{name:<18}{r[0]:>3}{cpu:>8.1f}{100.0 * r[2] / 1e9 / span:>11.1f}{r[4] / span:>9.0f}{r[5] / span:>9.0f}"
                     f"{r[3] / span:>10.0f}{per_wake:>9.1f}")
    lines.append(f"{'all':<21}{total:>8.1f}")
    text = "\n".join(lines)
    print(text, flush=True)
    if out:
        with open(out, "a") as f:
            f.write(text + "\n")


def main():
    args = [a for a in sys.argv[1:]]
    out = None
    wait = 60.0
    if "--out" in args:
        i = args.index("--out")
        out = args[i + 1]
        del args[i:i + 2]
    if "--wait" in args:
        i = args.index("--wait")
        wait = float(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        sys.exit(2)
    pid = find_pid(args[0], wait)
    if pid is None:
        print(f"no process {args[0]}")
        sys.exit(1)
    t0 = start_seconds(pid)
    windows = []
    for w in args[1].split(","):
        lo, hi = w.split("-")
        windows.append((float(lo), float(hi)))
    for lo, hi in windows:
        now = time.time() - t0
        if now > lo:
            print(f"window {lo:.0f}-{hi:.0f}: started late at {now:.0f} s, measuring from now")
            lo = now
        time.sleep(max(0.0, lo - (time.time() - t0)))
        a = snapshot(pid)
        ta = time.time()
        if a is None:
            print("process gone")
            return
        time.sleep(max(0.5, hi - lo))
        b = snapshot(pid)
        tb = time.time()
        if b is None:
            print("process gone")
            return
        report(a, b, tb - ta, f"pid {pid}, {lo:.0f}-{hi:.0f} s after start", out)


if __name__ == "__main__":
    main()
