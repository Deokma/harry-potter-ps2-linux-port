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

0x2dc178 keeps its callback argument in s5 (0x2dc1a4), which 0x2dc738 reads. A statement added at the
top of 0x2dc178 would not run: its re-entry switch has a case for the entry address too.
Addresses only, no game code. usage: ps2recomp_movie_stub.py FOLDER... (trial/out-b, src/runner)
"""
import re
import sys
from pathlib import Path

NOOP = ["002dd8e0_0x2dd8e0", "002dc868_0x2dc868", "002dcc98_0x2dcc98", "002de1e8_0x2de1e8"]
DECODE = "002dc738_0x2dc738"
PLAY = "002dc178_0x2dc178"
DECODE_BODY = "    SET_GPR_S32(ctx, 2, GPR_U32(ctx, 21) ? 0 : 1); // s5: the movie's callback\n"
MARK = "ps2port: movie stub"


def head(text):
    return text[: text.index("void FUN_")]


def stub(folder, name, body):
    p = folder / f"FUN_{name}.cpp"
    s = p.read_text(encoding="utf-8", newline="")
    if MARK in s and body in s:
        return False
    fn = f"FUN_{name}"
    p.write_text(head(s) + f"// {MARK}\nvoid {fn}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {{\n"
                 + body + "    ctx->pc = GPR_U32(ctx, 31);\n}\n", encoding="utf-8", newline="")
    return True


def unpatch_play(folder):
    """Remove the hook an earlier version of this script added."""
    p = folder / f"FUN_{PLAY}.cpp"
    t = p.read_text(encoding="utf-8", newline="")
    t, n = re.subn(r"\n    \{ extern uint32_t ps2portMovieCallback;[^\n]*", "", t)
    if n:
        p.write_text(t, encoding="utf-8", newline="")
    return bool(n)


def main():
    for arg in sys.argv[1:]:
        folder = Path(arg)
        done = [stub(folder, n, "") for n in NOOP]
        done.append(stub(folder, DECODE, DECODE_BODY))
        print(folder.name, "stubs changed", sum(done), "of", len(done), "hook removed", unpatch_play(folder))


if __name__ == "__main__":
    main()
