"""Find the eegen functions whose guest instruction count differs from
PS2Recomp's (x86 counting build, frame replay). Run in WSL from the repo root:
    python3 -I harness/eg_bisect.py REC LIST REFERENCE_COUNT
Each probe translates a subset, rebuilds only eegen's objects and runs once.
"""
import re
import subprocess
import sys

rec, listfile, ref = sys.argv[1], sys.argv[2], int(sys.argv[3])
t = "private/third_party/trial"
funcs = [l.split()[0] for l in open(listfile) if l.strip()]


def probe(subset):
    with open("/tmp/eg_subset.txt", "w") as f:
        f.write("\n".join(subset) + "\n")
    subprocess.run(["python3", "-I", "eegen/eegen.py", "private/extract/SLUS-20826/files/SLUS_208.26",
                    f"{t}/function_map.csv", f"{t}/harness/gen_eg", "--functions", "/tmp/eg_subset.txt",
                    "--ps2recomp-table", f"{t}/out-b/register_functions.cpp", "--skip", "0x2cd6a0"],
                   check=True, capture_output=True)
    subprocess.run(["sh", "harness/build.sh", "x86", "count", "frame-v0"], check=True, capture_output=True,
                   env={"REUSE": "1", "EG": "1", "PATH": "/usr/bin:/bin"})
    out = subprocess.run([f"../harness/build/harness-x86-count-frame-v0-eg", rec, "1"], cwd=f"{t}/rec",
                         capture_output=True, text=True, env={"HARNESS_SKIP": "0x2cd6a0", "HARNESS_STOP_TIMER": "2", "HARNESS_VU0": "3"}).stdout
    n = int(re.search(r"guest instructions per call (\d+)", out).group(1))
    same = re.search(r"check vs PC: 0 RDRAM", out) is not None
    print(f"{len(subset):4d} functions: {n - ref:+d} instructions, RAM {'same' if same else 'DIFFERS'}", flush=True)
    return n - ref


def search(subset):
    if probe(subset) == 0:
        return
    if len(subset) == 1:
        print("culprit", subset[0], flush=True)
        return
    half = len(subset) // 2
    search(subset[:half])
    search(subset[half:])


search(funcs)
