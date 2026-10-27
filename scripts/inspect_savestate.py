#!/usr/bin/env python3
"""Compare a PCSX2 savestate's memory with the boot ELF.

For one moment of real gameplay, answers:
  - does the code in EE RAM still match the ELF? (self-modifying code)
  - is there code in RAM outside the ELF image? (dynamically loaded code)
  - which VU0 / VU1 microcode overlays from the ELF are resident?
Differences can be attributed to a PCSX2 .pnach patch list, since states
made with patches enabled contain the patched words.

Usage:
    python scripts/inspect_savestate.py STATE.p2s ELF
        [--pnach FILE.pnach | --patches-zip patches.zip] [--json OUT.json]
"""

import argparse
import json
import re
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_elf import analyze_vu, parse_elf

JR_RA = 0x03E00008
BUCKET = 0x10000  # 64 KiB windows for the code scan


def pnach_addresses(text):
    """Word addresses touched by 'patch=' lines (EE only)."""
    addrs = set()
    for line in text.splitlines():
        m = re.match(r"\s*patch\s*=\s*\d+\s*,\s*EE\s*,\s*([0-9A-Fa-f]{8})\s*,"
                     r"\s*(\w+)", line)
        if not m:
            continue
        raw, kind = int(m[1], 16), m[2].lower()
        if kind == "extended" and raw >> 28 not in (0, 1, 2):
            continue  # conditionals and other control codes
        addrs.add((raw & 0x01FFFFFF) & ~3)
    return addrs


def diff_runs(a, b, base):
    """Yield (start addr, words) runs where two equal-length buffers differ."""
    run_start, run_len = None, 0
    for i in range(0, len(a) - 3, 4):
        if a[i:i + 4] != b[i:i + 4]:
            if run_start is None:
                run_start, run_len = base + i, 0
            run_len += 1
        elif run_start is not None:
            yield run_start, run_len
            run_start = None
    if run_start is not None:
        yield run_start, run_len


def code_scan(mem, lo, hi):
    """jr ra / ra-save counts per 64 KiB window in [lo, hi)."""
    out = []
    for start in range(lo - lo % BUCKET, hi, BUCKET):
        s, e = max(start, lo), min(start + BUCKET, hi)
        words = struct.unpack_from(f"<{(e - s) // 4}I", mem, s)
        jr = sum(1 for w in words if w == JR_RA)
        save = sum(1 for w in words if w >> 16 in (0xAFBF, 0xFFBF, 0x7FBF))
        if jr:
            out.append({"window": hex(start), "jr_ra": jr, "save_ra": save})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("state", type=Path)
    ap.add_argument("elf", type=Path)
    ap.add_argument("--pnach", type=Path)
    ap.add_argument("--patches-zip", type=Path,
                    help="PCSX2 resources/patches.zip; the .pnach matching "
                         "the state's CRC is used")
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)

    elf_data = args.elf.read_bytes()
    elf = parse_elf(elf_data)
    with zipfile.ZipFile(args.state) as z:
        ee = z.read("eeMemory.bin")
        vu0 = z.read("vu0MicroMem.bin")
        vu1 = z.read("vu1MicroMem.bin")
        version = z.read("PCSX2 Savestate Version.id")

    patched = set()
    if args.pnach:
        patched = pnach_addresses(args.pnach.read_text(errors="replace"))
    elif args.patches_zip:
        crc = re.search(r"\(([0-9A-F]{8})\)", args.state.name)
        with zipfile.ZipFile(args.patches_zip) as z:
            for n in z.namelist():
                if crc and crc[1] in n.upper():
                    patched |= pnach_addresses(z.read(n).decode("utf-8",
                                                                "replace"))

    report = {"state": args.state.name,
              "pcsx2": re.findall(rb"v\d+\.\d+\.\d+", version)[0].decode()
              if re.findall(rb"v\d+\.\d+\.\d+", version) else None,
              "patched_words_known": len(patched), "sections": [],
              "unexplained_code_diffs": []}

    # 1. ELF image vs RAM, per section
    for s in elf["sections"]:
        if not s["addr"] or not s["size"] or s["type"] == 8:
            continue
        img = elf_data[s["offset"]:s["offset"] + s["size"]]
        ram = ee[s["addr"]:s["addr"] + s["size"]]
        runs = list(diff_runs(img, ram, s["addr"]))
        words = sum(n for _, n in runs)
        explained = sum(1 for a, n in runs for k in range(n)
                        if a + 4 * k in patched)
        report["sections"].append({"name": s["name"], "addr": hex(s["addr"]),
                                   "bytes": s["size"], "diff_words": words,
                                   "diff_runs": len(runs),
                                   "explained_by_patches": explained})
        if s["flags"] & 0x4 or s["name"] == ".rodata":
            for a, n in runs:
                if any(a + 4 * k not in patched for k in range(n)):
                    report["unexplained_code_diffs"].append(
                        {"section": s["name"], "addr": hex(a), "words": n})

    # 2. code outside the ELF image
    seg = elf["segments"][0]
    image_end = (seg["vaddr"] + seg["memsz"] + 0xFFF) & ~0xFFF
    report["code_outside_elf"] = {
        "kernel_area_0_to_1MiB": code_scan(ee, 0, 0x100000),
        "after_elf_image": code_scan(ee, image_end, len(ee)),
        "elf_image_end": hex(image_end)}

    # 3. resident VU microcode
    vu = analyze_vu(elf, elf_data)
    vt = elf_data[next(s["offset"] for s in elf["sections"]
                       if s["name"] == ".vutext"):]
    vt_addr = int(vu["vutext"]["addr"], 16)
    resident = []
    for o in vu["overlays"]:
        src = vt[int(o["ee_addr"], 16) - vt_addr:][:o["bytes"]]
        va = int(o["vu_addr"], 16)
        row = {"overlay": o["name"], "vu_addr": o["vu_addr"]}
        for unit, mem in (("vu0", vu0), ("vu1", vu1)):
            if va + len(src) <= len(mem):
                n = len(src) // 8
                same = sum(1 for i in range(n)
                           if src[8 * i:8 * i + 8] == mem[va + 8 * i:
                                                          va + 8 * i + 8])
                row[f"{unit}_match"] = round(same / max(1, n), 3)
        resident.append(row)
    report["vu_overlays_resident"] = resident

    for sec in report["sections"]:
        print(f"  {sec['name']:10s} {sec['addr']:>10s} diff words "
              f"{sec['diff_words']:7d} in {sec['diff_runs']:5d} runs, "
              f"{sec['explained_by_patches']} explained by patches")
    print(f"  unexplained code/rodata diffs: "
          f"{len(report['unexplained_code_diffs'])}")
    for r in report["unexplained_code_diffs"][:20]:
        print(f"     {r}")
    co = report["code_outside_elf"]
    print(f"  code windows in kernel area: "
          f"{len(co['kernel_area_0_to_1MiB'])}, after ELF image "
          f"({co['elf_image_end']}+): {len(co['after_elf_image'])}")
    for w in co["after_elf_image"][:20]:
        print(f"     {w}")
    for r in resident:
        print(f"  {r['overlay']:40s} vu0 {r.get('vu0_match', '-')!s:6s} "
              f"vu1 {r.get('vu1_match', '-')}")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n",
                             newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
