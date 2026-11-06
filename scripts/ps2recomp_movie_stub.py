#!/usr/bin/env python3
"""Thin slice: no movie decoding. Patches PS2Recomp's generated code in the given folders.

The game's play-movie function (0x2dc178) is a blocking loop: open, then per video frame decode,
draw a full-screen quad, optionally call a callback (the main menu runs from one), flip. This
keeps that loop, so the menu and the game's frame pacing still run, and removes the video and
audio work under it:

  0x2dd8e0  video setup (IPU, stream)   nothing
  0x2dc868  audio setup                 nothing
  0x2dcc98  audio teardown              nothing
  0x2de1e8  video teardown              nothing
  0x2dc738  decode one frame            returns 1 (ended) if the movie has no callback, else 0

The loop's callback argument (a3 on entry to 0x2dc178) is kept in a host variable for 0x2dc738.
Addresses only, no game code. usage: ps2recomp_movie_stub.py FOLDER... (trial/out-b, src/runner)
"""
import re
import sys
from pathlib import Path

NOOP = ["002dd8e0_0x2dd8e0", "002dc868_0x2dc868", "002dcc98_0x2dcc98", "002de1e8_0x2de1e8"]
DECODE = "002dc738_0x2dc738"
PLAY = "002dc178_0x2dc178"
MARK = "ps2port: movie stub"


def head(text):
    return text[: text.index("void FUN_")]


def stub(folder, name, body):
    p = folder / f"FUN_{name}.cpp"
    s = p.read_text(encoding="utf-8", newline="")
    if MARK in s:
        return False
    fn = f"FUN_{name}"
    p.write_text(head(s) + f"// {MARK}\nvoid {fn}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {{\n"
                 + body + "    ctx->pc = GPR_U32(ctx, 31);\n}\n", encoding="utf-8", newline="")
    return True


def patch_play(folder):
    p = folder / f"FUN_{PLAY}.cpp"
    s = p.read_text(encoding="utf-8", newline="")
    if MARK in s:
        return False
    # after the re-entry switch, so a resume at a label doesn't take a stale a3
    m = re.search(r"default:\s*break;\s*\}", s)
    if not m:
        raise SystemExit("no re-entry switch in " + p.name)
    keep = f"\n    {{ extern uint32_t ps2portMovieCallback; ps2portMovieCallback = GPR_U32(ctx, 7); }} // {MARK}"
    p.write_text(s[: m.end()] + keep + s[m.end():], encoding="utf-8", newline="")
    return True


def main():
    for arg in sys.argv[1:]:
        folder = Path(arg)
        done = [stub(folder, n, "") for n in NOOP]
        done.append(stub(folder, DECODE,
                         "    extern uint32_t ps2portMovieCallback;\n"
                         "    SET_GPR_S32(ctx, 2, ps2portMovieCallback ? 0 : 1);\n"))
        done.append(patch_play(folder))
        print(folder.name, "patched", sum(done), "of", len(done))
    # the variable itself, once, next to the runtime's other additions
    rt = Path(sys.argv[-1]).parent / "lib" / "ps2_runtime.cpp"
    if rt.exists():
        s = rt.read_text(encoding="utf-8", newline="")
        if "ps2portMovieCallback" not in s:
            rt.write_text(s.rstrip() + f"\n\n// {MARK}: the callback of the movie being played (scripts/ps2recomp_movie_stub.py)\n"
                          "uint32_t ps2portMovieCallback = 0u;\n", encoding="utf-8", newline="")
            print("variable added to", rt.name)


if __name__ == "__main__":
    main()
