#!/usr/bin/env python3
"""sceSifFreeIopHeap as a native stub, like its partner. Patches PS2Recomp's generated code in the given folders.

PS2Recomp's symbol list has sceSifAllocIopHeap (0x314b90) as native but not the free (0x314c00), so the game's own free runs: an RPC to
an IOP heap service, which never reaches the allocator that gave out the block. The game uploads each sound sample through a temporary
IOP block (0x2ed2f0: alloc, send, wait, free), so every one leaked, until New Game wanted 2,169 KB of a 2 MB IOP and stalled.

runner/gen.sh picks up the native list from these files, so eegen then calls this rather than translating the function.
Addresses only, no game code. usage: ps2recomp_iop_heap_stub.py FOLDER... (trial/out-b, src/runner)
"""
import sys
from pathlib import Path

NAME = "00314c00_0x314c00"
MARK = "ps2port: native free"


def main():
    for arg in sys.argv[1:]:
        p = Path(arg) / f"FUN_{NAME}.cpp"
        text = p.read_text(encoding="utf-8", newline="")
        if MARK in text:
            print(Path(arg).name, "already patched")
            continue
        head = text[: text.index("void FUN_")]
        p.write_text(head + f"// {MARK}\nvoid FUN_{NAME}(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {{\n"
                     "    ctx->pc = getRegU32(ctx, 31);\n    ps2_stubs::sceSifFreeIopHeap(rdram, ctx, runtime);\n}\n", encoding="utf-8", newline="")
        print(Path(arg).name, "patched")


if __name__ == "__main__":
    main()
