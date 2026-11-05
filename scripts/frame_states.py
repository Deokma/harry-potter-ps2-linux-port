#!/usr/bin/env python3
"""Where the game is in a run of PCSX2 savestates taken one vblank apart.

For each state: the EE's pc and cycle count, the main thread's state in the
kernel's thread table, the game's tick and blend values, and how many RAM pages
changed since the state before. With that, finds the game frames in the run: a
state with the main thread in its wait, the next one with it in the wait again
after RAM has changed. For each it prints the arguments for make_replay_state.py.

The saves are game data; this script holds none. Addresses are this game's NTSC
executable (PHASE_3 C7).

Usage:
    python -I scripts/frame_states.py STATE.p2s [STATE.p2s ...]
"""

import struct
import sys
import zipfile
from pathlib import Path

RAM_PAGE = 4096
TCB_MAIN = 0x1A698      # kernel thread table: pc, sp, gp, ?, status, wait id
STATUS_WAIT = 2
RESUME_PC = 0x2CD6F0    # the vsync wait, after WaitSema
VBLANK_COUNT = 0x328490
VBLANK_NEEDED = 0x3A4FD8
CLOCK_OBJECT = 0x395730
TIME_BASE64, TIME_BASE32 = 0x32A350, 0x32A358
TICK, TICK_RUN, BLEND = 0x38C210, 0x3A5068, 0x3A506C
TICKS_PER_SECOND, TIME_UNIT = 0x326E00, 0x327360


def load(path):
    z = zipfile.ZipFile(path)
    ram = z.read("eeMemory.bin")
    data = z.read("PCSX2 Internal Structures.dat")
    regs = data.find(b"cpuRegs") + 32
    u32 = lambda a: struct.unpack_from("<I", ram, a)[0]
    u64 = lambda a: struct.unpack_from("<Q", ram, a)[0]
    f32 = lambda a: struct.unpack_from("<f", ram, a)[0]
    sp = u32(TCB_MAIN + 4)
    clock = u32(CLOCK_OBJECT)
    state = {
        "path": path, "ram": ram,
        "pc": struct.unpack_from("<I", data, regs + 512 + 32 + 128 + 8)[0],
        "count": struct.unpack_from("<I", data, regs + 512 + 32 + 9 * 4)[0],
        "cycle": struct.unpack_from("<I", data, regs + 512 + 32 + 128 + 16 + 16 + 256)[0],
        "main_pc": u32(TCB_MAIN), "main_sp": sp, "main_status": u32(TCB_MAIN + 16),
        "main_wait": u32(TCB_MAIN + 20), "main_ra": u32(sp + 31 * 16) if 0 < sp < len(ram) - 512 else 0,
        "vblanks": u32(VBLANK_COUNT), "needed": u32(VBLANK_NEEDED),
        "tick": u32(TICK), "tick_run": u32(TICK_RUN), "blend": f32(BLEND),
        "unit": u32(TIME_UNIT), "rate": u32(TICKS_PER_SECOND),
        "factor": u64(clock), "last": u64(clock + 8), "acc": u64(clock + 16),
        "base64": u64(TIME_BASE64), "base32": u32(TIME_BASE32),
        "scene": (u32(0x395740), u32(0x38C194)),
    }
    state["waiting"] = state["main_status"] == STATUS_WAIT and state["main_ra"] == RESUME_PC
    return state


def main(argv=None):
    paths = (argv if argv is not None else sys.argv[1:])
    states = [load(p) for p in paths]
    previous = None
    for s in states:
        pages = 0 if previous is None else sum(
            previous["ram"][p:p + RAM_PAGE] != s["ram"][p:p + RAM_PAGE] for p in range(0, len(s["ram"]), RAM_PAGE))
        delta = 0 if previous is None else (s["cycle"] - previous["cycle"]) & 0xFFFFFFFF
        s["pages"] = pages
        print(f"{Path(s['path']).name:32.32s} pc={s['pc']:08x} +{delta:<8d} pages={pages:<4d} "
              f"main: {'waiting' if s['waiting'] else 'status ' + str(s['main_status'])} ra={s['main_ra']:x} "
              f"vblanks {s['vblanks']}/{s['needed']} tick {s['tick']} blend {s['blend']:.2f} scene {s['scene']}")
        previous = s

    # game frames: main waiting at a vblank, then again later with work in between
    for i, start in enumerate(states):
        if not start["waiting"]:
            continue
        for j in range(i + 1, len(states)):
            end = states[j]
            if end["waiting"] and end["tick_run"] != start["tick_run"] or (
                    end["waiting"] and sum(s["pages"] for s in states[i + 1:j + 1]) > 50):
                break
        else:
            continue
        # the state just before the interval the main thread wakes in
        wake = max((k for k in range(i, j) if states[k]["waiting"]), default=i)
        first = states[wake]
        last = states[j]
        ticks = last["tick"] - first["tick"]
        # elapsed time the game read: last tick run, plus the blend it kept
        steps = first["unit"] // first["rate"] if first["rate"] else 10
        elapsed = last["tick_run"] * steps + round(last["blend"] * steps)
        read = (first["last"] - first["base64"] + first["base32"]) & 0xFFFFFFFF
        count = int(read + (elapsed + 0.5) * first["factor"] - first["acc"]) & 0xFFFFFFFF
        pages = sum(s["pages"] for s in states[wake + 1:j + 1])
        print(f"frame: {Path(first['path']).name} -> {Path(last['path']).name}: {j - wake} vblanks, "
              f"{pages} pages, {ticks} ticks, elapsed {elapsed}")
        print(f"  --pc {RESUME_PC:#x} --sp 0 --gp 0 --from-block {first['main_sp']:#x} "
              f"--poke {VBLANK_COUNT:#x}={first['needed']} --count {count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
