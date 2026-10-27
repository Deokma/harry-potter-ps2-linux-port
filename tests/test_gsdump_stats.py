#!/usr/bin/env python3
"""Self-test for scripts/gsdump_stats.py on a synthetic PCSX2 GS dump.

Builds a small dump in the PCSX2 v2.6.3 format with known geometry
(4 vsyncs, display flipping every vsync) and checks the per-frame numbers.

Run:  python -I tests/test_gsdump_stats.py
"""

import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import gsdump_stats as g
from compression import zstd

OFS = 0x8000  # XYOFFSET used below: window coordinate = (X - OFS) / 16


def tag(nloop, regs, flg=0, eop=0, pre=0, prim=0):
    lo = nloop | (eop << 15) | (pre << 46) | (prim << 47) | (flg << 58) | \
        ((len(regs) & 0xF) << 60)
    hi = sum(r << (4 * i) for i, r in enumerate(regs))
    return struct.pack("<QQ", lo, hi)


def ad(addr, value):
    return struct.pack("<QQ", value, addr)


def xyz2(x, y, adc=0):
    return struct.pack("<QQ", (OFS + x * 16) | ((OFS + y * 16) << 32),
                       adc << 47)


def rgbaq(a):
    return struct.pack("<QQ", 0x80 | (0x80 << 32), 0x80 | (a << 32))


def frame_payload_path3():
    out = tag(9, [0xE])
    out += ad(g.FRAME_1, 10 << 16)                              # FBP 0, 640
    out += ad(g.SCISSOR_1, (639 << 16) | (447 << 48))
    out += ad(g.XYOFFSET_1, OFS | (OFS << 32))
    out += ad(g.TEST_1, (1 << 16) | (2 << 17))                  # ZTE, GEQUAL
    out += ad(g.ZBUF_1, 0x100)
    out += ad(g.ALPHA_1, 0 | (1 << 2) | (0 << 4) | (1 << 6))    # Cs-Cd*As+Cd
    out += ad(g.COLCLAMP, 1)
    out += ad(g.PRMODECONT, 1)
    out += ad(g.PRIM, 3 | 0x40)                                 # tri, ABE
    out += tag(3, [0x1, 0x5])                                   # 5000 px
    out += rgbaq(0x80) + xyz2(0, 0) + rgbaq(0x80) + xyz2(100, 0) + \
        rgbaq(0x80) + xyz2(0, 100)
    out += tag(2, [0x5], pre=1, prim=6)                         # sprite
    out += xyz2(10, 10) + xyz2(110, 60)                         # 5000 px
    out += tag(2, [0xE])
    out += ad(g.ALPHA_1, 0 | (1 << 2) | (1 << 4) | (1 << 6))    # C = Ad
    out += ad(g.PRIM, 3 | 0x40)
    out += tag(3, [0x5])                                        # 5000 px
    out += xyz2(200, 200) + xyz2(300, 200) + xyz2(200, 300)
    out += tag(2, [0xE])                                        # tex = RT
    out += ad(g.TEX0_1, (10 << 14) | (9 << 26) | (9 << 30))
    out += ad(g.PRIM, 6 | 0x10)
    out += tag(2, [0x5])
    out += xyz2(0, 0) + xyz2(64, 64)                            # 4096 px
    out += tag(3, [0xE])                                        # 8x8 upload
    out += ad(g.BITBLTBUF, (0x1000 << 32) | (1 << 48))
    out += ad(g.TRXREG, 8 | (8 << 32))
    out += ad(g.TRXDIR, 0)
    out += tag(16, [], flg=2, eop=1) + bytes(16 * 16)
    return out


def frame_payload_path1():
    """A PATH1 packet followed by VU1-memory junk that must be ignored."""
    out = tag(3, [0x5], eop=1, pre=1, prim=3)                   # 5000 px
    out += xyz2(400, 0) + xyz2(500, 0) + xyz2(400, 100)
    out += tag(0x7FFF, [0x5]) + bytes(16)                       # junk
    return out


def priv(fbp):
    r = bytearray(g.PRIV_SIZE)
    struct.pack_into("<Q", r, 0x00, 2)                          # EN2
    struct.pack_into("<Q", r, 0x20, 1)                          # interlaced
    struct.pack_into("<Q", r, 0x90, fbp | (10 << 9))            # DISPFB2
    struct.pack_into("<Q", r, 0xA0, (3 << 23) | (2559 << 32) | (447 << 44))
    return bytes(r)


def build_dump():
    serial = b"TEST-00000"
    state = bytearray(4 + 39 * 8 + 200 + 4 * 20 + 4)
    struct.pack_into("<I", state, 0, 9)
    hdr = struct.pack("<9I", 9, len(state), 36, len(serial), 0xC71A6146,
                      0, 0, 36 + len(serial), 0)
    out = struct.pack("<II", 0xFFFFFFFF, len(hdr) + len(serial)) + hdr + \
        serial + bytes(state) + priv(0)
    p3, p1 = frame_payload_path3(), frame_payload_path1()
    for i in range(4):
        out += b"\x00\x02" + struct.pack("<I", len(p3)) + p3
        out += b"\x00\x00" + struct.pack("<I", len(p1)) + p1
        out += b"\x03" + priv(0 if i % 2 == 0 else 0x50)
        out += b"\x01\x00"
    return out


def main():
    raw = build_dump()
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "TEST-00000_synthetic.gs.zst"
        path.write_bytes(zstd.compress(raw))
        info, gs = g.replay(g.load_dump(path))
    assert info["serial"] == "TEST-00000", info
    mode, frames = g.assemble(gs.intervals, 2)
    assert mode == "flip" and len(frames) == 2, (mode, len(frames))
    row = g.frame_row(frames[0])
    expect = {
        "vsyncs": 1, "prims": 5, "prims_tri": 3, "prims_sprite": 2,
        "vertices": 13, "draws": 5, "fill_px": 24096,
        "fill_blend_px": 10000, "fill_z_px": 19096 + 5000,
        "draws_blend": 2, "draws_blend_nonfixed": 1,
        "draws_blend_dst_alpha": 1, "draws_blend_src_alpha": 1,
        "draws_textured": 1, "uniq_tex_clut": 1,
        "tex_bytes_native": 512 * 512 * 4,
        "draws_tex_from_current_rt": 1, "uploads": 1, "upload_bytes": 256,
        "gif_bytes_path1": 16 * 4, "rt_count": 1}
    bad = {k: (row[k], v) for k, v in expect.items() if row[k] != v}
    assert not bad, f"mismatch (got, expected): {bad}"
    assert row["display"] == "640x448 CT32 int=1 field=0", row["display"]
    assert abs(row["overdraw"] - round(24096 / (640 * 448), 3)) < 1e-9
    assert row["render_targets"] == "fbp0/w640/CT32:500x300", \
        row["render_targets"]
    # without flips the same dump falls back to 2-vsync windows
    for iv in gs.intervals:
        iv.flip = False
    mode, frames = g.assemble(gs.intervals, 2)
    assert mode == "2-vsync window" and len(frames) == 2
    assert g.frame_row(frames[0])["prims"] == 10
    assert g.pct(list(range(1, 21)), 95) == 19
    print("gsdump_stats self-test: OK")


if __name__ == "__main__":
    main()
