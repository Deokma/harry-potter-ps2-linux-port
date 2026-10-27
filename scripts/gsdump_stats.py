#!/usr/bin/env python3
"""GS workload statistics from PCSX2 GS dumps, per frame and per scene.

Replays the GIF stream recorded in a PCSX2 2.x GS dump (.gs, .gs.zst,
.gs.xz) without rendering anything, and measures per frame:
  - primitives by type, vertices, total GIF bytes (2.6.3 dumps don't
    record which GIF path a transfer came from)
  - estimated draw calls: runs of primitives with unchanged draw state
  - fill: scissor-clipped pixel area of every primitive, split by
    blending / texturing / depth test; overdraw relative to the main
    render target, and that fill scaled to a 640x480 panel
  - render targets and depth buffers (base, width, format, drawn extent,
    fill per target)
  - display mode from the privileged registers (interlace, frame/field)
  - texture uploads (host->local), local->local copies, readbacks
  - draws whose blend mode GLES fixed-function blending cannot express,
    and draws that texture from a render target (incl. the current one)
  - unique texture+CLUT combinations and their de-swizzled size
Frames are delimited by display flips (DISPFB base change) or, for games
with a single display buffer, by "presents" (drawing switches into the
displayed buffer, i.e. the copy of a finished frame). Without either,
windows of --vsyncs-per-frame vsyncs are used. Partial frames at the
start and end of a dump are dropped; vsyncs per frame give the pacing.

Format (PCSX2 v2.6.3 GSDump.cpp / GSState::Freeze): u32 0xFFFFFFFF,
u32 header size, GSDumpHeader (9 x u32), serial, screenshot, GS state,
8 KiB privileged registers, then packets: 0 transfer (u8 path, u32 size,
data), 1 vsync (u8 field), 2 FIFO read (u32 size), 3 privileged regs.

Usage:
    python scripts/gsdump_stats.py DUMP_OR_DIR... --scene ID [--out DIR]
                                   [--vsyncs-per-frame N]
Writes <out>/<scene>/frames.csv, summary.json and summary.md
(default out: D:/ps2port-data/reports).
"""

import argparse
import csv
import json
import lzma
import math
import struct
import sys
from collections import Counter
from pathlib import Path

try:
    from compression import zstd  # Python 3.14+
except ImportError:
    zstd = None

PRIV_SIZE = 0x2000
PANEL_PX = 640 * 480

PSM_NAMES = {0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S",
             0x13: "T8", 0x14: "T4", 0x1B: "T8H", 0x24: "T4HL",
             0x2C: "T4HH", 0x30: "Z32", 0x31: "Z24", 0x32: "Z16",
             0x3A: "Z16S"}
TEXEL_BITS = {0x00: 32, 0x01: 24, 0x02: 16, 0x0A: 16, 0x13: 8, 0x14: 4,
              0x1B: 8, 0x24: 4, 0x2C: 4, 0x30: 32, 0x31: 24, 0x32: 16,
              0x3A: 16}
# bits a pixel occupies in GS memory (24-bit and the H formats live in
# 32-bit pixels) and the page size in pixels for that storage width
STORE_BITS = {0x00: 32, 0x01: 32, 0x02: 16, 0x0A: 16, 0x13: 8, 0x14: 4,
              0x1B: 32, 0x24: 32, 0x2C: 32, 0x30: 32, 0x31: 32, 0x32: 16,
              0x3A: 16}
PAGE_DIM = {32: (64, 32), 16: (64, 64), 8: (128, 64), 4: (128, 128)}
INDEXED = {0x13, 0x14, 0x1B, 0x24, 0x2C}
PAGE_BYTES = 8192

# GS register addresses (A+D / REGLIST)
PRIM, RGBAQ, XYZF2, XYZ2, TEX0_1, TEX0_2 = 0x00, 0x01, 0x04, 0x05, 0x06, 0x07
CLAMP_1, XYZF3, XYZ3 = 0x08, 0x0C, 0x0D
TEX1_1, TEX2_1, XYOFFSET_1 = 0x14, 0x16, 0x18
PRMODECONT, PRMODE, TEXCLUT, TEXA, FOGCOL, TEXFLUSH = \
    0x1A, 0x1B, 0x1C, 0x3B, 0x3D, 0x3F
SCISSOR_1, ALPHA_1, DIMX, DTHE, COLCLAMP, TEST_1, PABE, FBA_1 = \
    0x40, 0x42, 0x44, 0x45, 0x46, 0x47, 0x49, 0x4A
FRAME_1, ZBUF_1, BITBLTBUF, TRXPOS, TRXREG, TRXDIR = \
    0x4C, 0x4E, 0x50, 0x51, 0x52, 0x53
CTX_REGS = {TEX0_1: "TEX0", CLAMP_1: "CLAMP", TEX1_1: "TEX1",
            TEX2_1: "TEX2", XYOFFSET_1: "XYOFFSET", SCISSOR_1: "SCISSOR",
            ALPHA_1: "ALPHA", TEST_1: "TEST", FBA_1: "FBA",
            FRAME_1: "FRAME", ZBUF_1: "ZBUF", 0x34: "MIPTBP1",
            0x36: "MIPTBP2"}
ENV_REGS = {PRMODECONT: "PRMODECONT", PRMODE: "PRMODE",
            TEXCLUT: "TEXCLUT", TEXA: "TEXA", FOGCOL: "FOGCOL",
            DIMX: "DIMX", DTHE: "DTHE", COLCLAMP: "COLCLAMP",
            PABE: "PABE", BITBLTBUF: "BITBLTBUF", TRXPOS: "TRXPOS",
            TRXREG: "TRXREG", 0x22: "SCANMSK"}
VERTS_NEEDED = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2}
PRIM_CLASS = {0: "point", 1: "line", 2: "line", 3: "tri", 4: "tri",
              5: "tri", 6: "sprite"}


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def mem_range(base_bytes, width_px, height_px, psm):
    """Byte range a width x height buffer occupies in GS memory (by page)."""
    pw, ph = PAGE_DIM[STORE_BITS.get(psm, 32)]
    pages = -(-max(width_px, 1) // pw) * -(-max(height_px, 1) // ph)
    return base_bytes, base_bytes + pages * PAGE_BYTES


def clip_area(pts, x0, y0, x1, y1):
    """Area of a polygon clipped to the rectangle [x0,x1) x [y0,y1)."""
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    if min(xs) >= x0 and max(xs) <= x1 and min(ys) >= y0 and max(ys) <= y1:
        poly = pts
    else:
        poly = pts
        for axis, bound, keep_ge in ((0, x0, True), (0, x1, False),
                                     (1, y0, True), (1, y1, False)):
            out = []
            n = len(poly)
            for i in range(n):
                a, b = poly[i], poly[(i + 1) % n]
                ina = (a[axis] >= bound) if keep_ge else (a[axis] <= bound)
                inb = (b[axis] >= bound) if keep_ge else (b[axis] <= bound)
                if ina:
                    out.append(a)
                if ina != inb:
                    t = (bound - a[axis]) / (b[axis] - a[axis])
                    out.append((a[0] + t * (b[0] - a[0]),
                                a[1] + t * (b[1] - a[1])))
            poly = out
            if len(poly) < 3:
                return 0.0
    s = 0.0
    n = len(poly)
    for i in range(n):
        s += poly[i][0] * poly[(i + 1) % n][1] - \
            poly[(i + 1) % n][0] * poly[i][1]
    return abs(s) / 2.0


def blend_flags(alpha, colclamp, pabe):
    """Why a PS2 blend equation does not map to GLES fixed-function.

    PS2: ((A - B) * C >> 7) + D, A/B/D in {Cs, Cd, 0}, C in {As, Ad, FIX},
    alpha 0x80 = 1.0. GLES: src*Fs (+|-) dst*Fd with factors in [0, 1].
    """
    a, b, c, d = bits(alpha, 0, 2), bits(alpha, 2, 2), bits(alpha, 4, 2), \
        bits(alpha, 6, 2)
    fix = bits(alpha, 32, 8)
    flags = set()
    if a == b:
        return flags  # result is just D
    # (A-B)*C + D leaves a (1 + C) factor on Cs or Cd in these forms
    if (a, b, d) in ((0, 1, 0), (1, 0, 1), (0, 2, 0), (1, 2, 1)):
        flags.add("factor_gt_1")
    if c == 1:
        flags.add("dst_alpha")  # Ad is 0..0x80 in memory: needs x2 scaling
    if c == 2 and fix > 0x80:
        flags.add("fix_gt_1")
    if c == 0:
        flags.add("src_alpha")  # exact only while As <= 0x80
    if not colclamp & 1:
        flags.add("wrap")  # COLCLAMP=0: results wrap modulo 256
    if pabe & 1:
        flags.add("pabe")  # per-pixel blend enable from As MSB
    return flags


NONFIXED = {"factor_gt_1", "dst_alpha", "fix_gt_1", "wrap", "pabe"}


class Chunk:
    """Counters between two events: a vsync and/or a frame boundary.

    Chunks are merged into frames later. A chunk ends either at a vsync
    (vsync=True) or right before a "present" draw (see GSReplay.emit);
    boundary says whether a frame ends with it ("flip" or "present").
    """

    def __init__(self):
        self.c = Counter()
        self.tex = {}       # texture+CLUT key -> (native bytes, rgba8 bytes)
        self.rts = {}       # (fbp, fbw, psm) -> [x0, y0, x1, y1]
        self.rtfill = Counter()  # (fbp, fbw, psm) -> clipped pixels
        self.zbufs = set()  # (zbp, psm)
        self.vsync = False
        self.boundary = None
        self.disp = None

    def merge(self, other):
        self.c.update(other.c)
        self.tex.update(other.tex)
        for k, bb in other.rts.items():
            cur = self.rts.setdefault(k, list(bb))
            cur[0], cur[1] = min(cur[0], bb[0]), min(cur[1], bb[1])
            cur[2], cur[3] = max(cur[2], bb[2]), max(cur[3], bb[3])
        self.rtfill.update(other.rtfill)
        self.zbufs |= other.zbufs
        self.disp = other.disp or self.disp


class GifPath:
    __slots__ = ("nloop", "flg", "regs", "cur", "eop")

    def __init__(self):
        self.nloop = 0
        self.flg = 0
        self.regs = [0xE]
        self.cur = 0
        self.eop = False


class GSReplay:
    def __init__(self, state, priv):
        self.ctx = [dict.fromkeys(CTX_REGS.values(), 0) for _ in range(2)]
        self.env = dict.fromkeys(ENV_REGS.values(), 0)
        self.env["PRMODECONT"] = 1
        self.env["COLCLAMP"] = 1
        self.prim = 0
        self._load_state(state)
        self.priv = priv
        self.vq = []
        self.rgba_a = 0x80
        self.cbp0 = self.cbp1 = -1
        self.cur = Chunk()
        self.chunks = []
        d = self.display()
        self.disp_fbp = d.get("fbp") if d else None
        self.batch_key = None
        self.batch_flags = set()
        self.batch_z = False
        self.last_rt = None
        self.last_disp_key = None
        # GS memory ranges of every render target drawn so far in this dump
        # (kept across frames: effects often sample the previous frame);
        # a texture upload over a range retires it
        self.rt_ranges = {}
        self.warnings = Counter()

    # -- initial state ------------------------------------------------------
    def _load_state(self, st):
        self.paths = [GifPath() for _ in range(4)]
        if len(st) < 4 + 39 * 8:
            return
        env_order = ["PRIM", "PRMODECONT", "TEXCLUT", "SCANMSK", "TEXA",
                     "FOGCOL", "DIMX", "DTHE", "COLCLAMP", "PABE",
                     "BITBLTBUF", "TRXDIR", "TRXPOS", "TRXREG", "TRXREG"]
        vals = struct.unpack_from("<15Q", st, 4)
        for name, v in zip(env_order, vals):
            if name == "PRIM":
                self.prim = v & 0x7FF
            elif name in self.env:
                self.env[name] = v
        ctx_order = ["XYOFFSET", "TEX0", "TEX1", "CLAMP", "MIPTBP1",
                     "MIPTBP2", "SCISSOR", "ALPHA", "TEST", "FBA", "FRAME",
                     "ZBUF"]
        for i in range(2):
            vals = struct.unpack_from("<12Q", st, 124 + i * 96)
            for name, v in zip(ctx_order, vals):
                self.ctx[i][name] = v
        # GIF path states sit at the end: 4 x (GIFTag 16 B + u32 reg), f32 q
        off = len(st) - 4 - 4 * 20
        for i in range(4):
            lo, hi, reg = struct.unpack_from("<QQI", st, off + 20 * i)
            nloop, flg = bits(lo, 0, 15), bits(lo, 58, 2)
            nreg = bits(lo, 60, 4) or 16
            if nloop and reg < nreg:
                p = self.paths[i]
                p.nloop, p.flg, p.cur = nloop, flg, reg
                p.regs = [bits(hi, 4 * k, 4) for k in range(nreg)]
                p.eop = bool(bits(lo, 15, 1))

    # -- helpers ------------------------------------------------------------
    def attrs(self):
        if self.env["PRMODECONT"] & 1:
            return self.prim & 0x7F8
        return self.env["PRMODE"] & 0x7F8

    def write_reg(self, addr, v):
        if addr == PRIM:
            self.prim = v & 0x7FF
            self.vq.clear()
        elif addr == RGBAQ:
            self.rgba_a = bits(v, 24, 8)
        elif addr in (XYZ2, XYZF2, XYZ3, XYZF3):
            self.vertex(bits(v, 0, 16), bits(v, 16, 16),
                        addr in (XYZ2, XYZF2))
        elif addr in (TEX0_1, TEX0_2):
            self.set_tex0(addr - TEX0_1, v)
        elif addr in (TEX2_1, TEX2_1 + 1):
            c = addr - TEX2_1
            keep = self.ctx[c]["TEX0"] & ~((0x3F << 20) | (0x7FFFFFF << 37))
            self.set_tex0(c, keep | (v & ((0x3F << 20) | (0x7FFFFFF << 37))))
        elif addr in CTX_REGS or addr - 1 in CTX_REGS:
            base = addr if addr in CTX_REGS else addr - 1
            self.ctx[addr - base][CTX_REGS[base]] = v
        elif addr in ENV_REGS:
            self.env[ENV_REGS[addr]] = v
        elif addr == TRXDIR:
            self.transfer_start(v & 3)
        elif addr == TEXFLUSH:
            self.cur.c["texflush"] += 1

    def set_tex0(self, c, v):
        self.ctx[c]["TEX0"] = v
        if bits(v, 20, 6) not in INDEXED:
            return
        cld, cbp = bits(v, 61, 3), bits(v, 37, 14)
        load = cld in (1, 2, 3) or (cld == 4 and cbp != self.cbp0) or \
            (cld == 5 and cbp != self.cbp1)
        if cld in (2, 4):
            self.cbp0 = cbp
        if cld in (3, 5):
            self.cbp1 = cbp
        if load:
            self.cur.c["clut_loads"] += 1

    def transfer_start(self, xdir):
        blt, reg = self.env["BITBLTBUF"], self.env["TRXREG"]
        w, h = bits(reg, 0, 12), bits(reg, 32, 12)
        c = self.cur.c
        self.batch_key = None
        if xdir == 0:
            c["uploads"] += 1
            d0, d1 = mem_range(bits(blt, 32, 14) * 256,
                               bits(blt, 48, 6) * 64 or w, h,
                               bits(blt, 56, 6))
            for k in [k for k, (s, e) in self.rt_ranges.items()
                      if s < d1 and d0 < e]:
                del self.rt_ranges[k]
        elif xdir == 1:
            c["readbacks"] += 1
            c["readback_bytes"] += w * h * \
                TEXEL_BITS.get(bits(blt, 24, 6), 32) // 8
        elif xdir == 2:
            c["copies"] += 1
            c["copy_bytes"] += w * h * \
                TEXEL_BITS.get(bits(blt, 24, 6), 32) // 8

    # -- primitives ---------------------------------------------------------
    def vertex(self, x, y, kick):
        attrs = self.attrs()
        ctx = self.ctx[bits(attrs, 9, 1)]
        off = ctx["XYOFFSET"]
        vx = (x - bits(off, 0, 16)) / 16.0
        vy = (y - bits(off, 32, 16)) / 16.0
        self.cur.c["vertices"] += 1
        q = self.vq
        q.append((vx, vy, self.rgba_a))
        t = self.prim & 7
        need = VERTS_NEEDED.get(t)
        if need is None:
            return
        if not kick or len(q) < need:
            if len(q) > need:
                del q[1 if t == 5 else 0]
            return
        if t in (0, 1, 3, 6):
            self.emit(t, q[-need:], attrs)
            q.clear()
        elif t == 2:
            self.emit(t, q[-2:], attrs)
            del q[:-1]
        elif t == 4:
            self.emit(t, q[-3:], attrs)
            del q[:-2]
        else:  # fan
            self.emit(t, [q[0], q[-2], q[-1]], attrs)
            del q[1:-1]

    def draw_key(self, t, attrs, c):
        ctx, env = self.ctx[c], self.env
        tme, fge, abe = attrs & 0x10, attrs & 0x20, attrs & 0x40
        return (PRIM_CLASS[t], attrs, ctx["FRAME"], ctx["ZBUF"],
                ctx["TEST"], ctx["SCISSOR"], ctx["FBA"],
                (ctx["TEX0"] & ~(7 << 61)) if tme else 0,
                ctx["TEX1"] if tme else 0, ctx["CLAMP"] if tme else 0,
                ctx["ALPHA"] if abe else 0, env["PABE"] if abe else 0,
                env["COLCLAMP"] if abe else 0, env["DTHE"] & 1,
                env["TEXA"] if tme else 0, env["FOGCOL"] if fge else 0)

    def new_draw(self, t, attrs, c):
        ctx, env, cnt = self.ctx[c], self.env, self.cur.c
        cnt["draws"] += 1
        frame = ctx["FRAME"]
        rt = (bits(frame, 0, 9), bits(frame, 16, 6), bits(frame, 24, 6))
        if rt != self.last_rt:
            cnt["rt_switches"] += 1
            self.last_rt = rt
        test, zbuf = ctx["TEST"], ctx["ZBUF"]
        zte, ztst = bits(test, 16, 1), bits(test, 17, 2)
        zmsk = bits(zbuf, 32, 1)
        self.batch_z = bool(zte and (ztst != 1 or not zmsk))
        if self.batch_z:
            self.cur.zbufs.add((bits(zbuf, 0, 9), 0x30 | bits(zbuf, 24, 4)))
        if bits(test, 0, 1) and bits(test, 1, 3) != 1 and bits(test, 12, 2):
            cnt["draws_afail_partial"] += 1
        if bits(test, 14, 1):
            cnt["draws_date"] += 1
        self.batch_flags = set()
        if attrs & 0x40 or attrs & 0x80:
            cnt["draws_blend"] += 1
            flags = blend_flags(ctx["ALPHA"], env["COLCLAMP"], env["PABE"])
            if attrs & 0x80:
                flags.add("aa1")
            for f in flags:
                cnt[f"draws_blend_{f}"] += 1
            if flags & NONFIXED or "aa1" in flags:
                cnt["draws_blend_nonfixed"] += 1
            self.batch_flags = flags
        if attrs & 0x10:
            self.texture_draw(ctx, rt, cnt)

    def texture_draw(self, ctx, rt, cnt):
        tex0 = ctx["TEX0"]
        psm = bits(tex0, 20, 6)
        tw, th = bits(tex0, 26, 4), bits(tex0, 30, 4)
        tbw = bits(tex0, 14, 6)
        key = (bits(tex0, 0, 14), tbw, psm, tw, th)
        if psm in INDEXED:
            key += (bits(tex0, 37, 14), bits(tex0, 51, 4), bits(tex0, 56, 5))
        texels = (1 << min(tw, 11)) * (1 << min(th, 11))
        if key not in self.cur.tex:
            self.cur.tex[key] = (texels * TEXEL_BITS.get(psm, 32) // 8,
                                 texels * 4)
        cnt["draws_textured"] += 1
        tex1 = ctx["TEX1"]
        if bits(tex1, 2, 3) and bits(tex1, 6, 3) >= 2:
            cnt["draws_mipmapped"] += 1
        t0, t1 = mem_range(bits(tex0, 0, 14) * 256,
                           tbw * 64 or (1 << tw), 1 << th, psm)
        cur = self.rt_ranges.get(rt)
        if cur and t0 < cur[1] and cur[0] < t1:
            cnt["draws_tex_from_current_rt"] += 1
        elif any(t0 < e and s < t1 for k, (s, e) in self.rt_ranges.items()
                 if k != rt):
            cnt["draws_tex_from_rt"] += 1

    def emit(self, t, verts, attrs):
        c = bits(attrs, 9, 1)
        key = self.draw_key(t, attrs, c)
        if key != self.batch_key:
            # "present": drawing switches into the displayed buffer, e.g. the
            # copy of a finished frame into a single display buffer
            fbp = bits(self.ctx[c]["FRAME"], 0, 9)
            if fbp == self.disp_fbp and \
                    (self.last_rt is None or self.last_rt[0] != fbp):
                self._close(vsync=False, boundary="present")
            self.new_draw(t, attrs, c)
            self.batch_key = key
        ctx, cnt = self.ctx[c], self.cur.c
        cls = PRIM_CLASS[t]
        cnt[f"prims_{cls}"] += 1
        cnt["prims"] += 1
        sc = ctx["SCISSOR"]
        x0, x1 = bits(sc, 0, 11), bits(sc, 16, 11) + 1
        y0, y1 = bits(sc, 32, 11), bits(sc, 48, 11) + 1
        pts = [(v[0], v[1]) for v in verts]
        if cls == "tri":
            area = clip_area(pts, x0, y0, x1, y1)
        elif cls == "sprite":
            ax0, ax1 = sorted((pts[0][0], pts[1][0]))
            ay0, ay1 = sorted((pts[0][1], pts[1][1]))
            area = max(0.0, min(ax1, x1) - max(ax0, x0)) * \
                max(0.0, min(ay1, y1) - max(ay0, y0))
        elif cls == "line":
            area = max(abs(pts[1][0] - pts[0][0]),
                       abs(pts[1][1] - pts[0][1])) + 1
        else:
            area = 1.0
        cnt["fill_px"] += area
        if attrs & 0x40:
            cnt["fill_blend_px"] += area
            if "src_alpha" in self.batch_flags and not attrs & 0x10 and \
                    any(v[2] > 0x80 for v in verts):
                cnt["prims_src_alpha_gt_1"] += 1
        if attrs & 0x10:
            cnt["fill_tex_px"] += area
        if self.batch_z:
            cnt["fill_z_px"] += area
        if area > 0:
            frame = ctx["FRAME"]
            rt = (bits(frame, 0, 9), bits(frame, 16, 6), bits(frame, 24, 6))
            self.cur.rtfill[rt] += area
            bx0 = max(min(p[0] for p in pts), x0)
            by0 = max(min(p[1] for p in pts), y0)
            bx1 = min(max(p[0] for p in pts), x1)
            by1 = min(max(p[1] for p in pts), y1)
            bb = self.cur.rts.setdefault(rt, [bx0, by0, bx1, by1])
            bb[0], bb[1] = min(bb[0], bx0), min(bb[1], by0)
            bb[2], bb[3] = max(bb[2], bx1), max(bb[3], by1)
            s, e = mem_range(rt[0] * PAGE_BYTES, rt[1] * 64, int(by1) + 1,
                             rt[2])
            old = self.rt_ranges.get(rt)
            self.rt_ranges[rt] = (s, max(e, old[1])) if old else (s, e)

    # -- GIF ----------------------------------------------------------------
    def transfer(self, index, data):
        """One recorded GIF transfer.

        Index 0 = old-style PATH1 (rest of VU1 memory follows the packet),
        1 = PATH2, 2 = PATH3, 3 = any path via PCSX2's GIF unit. 2.6.3 dumps
        record everything as 3, so the source path is not known.
        """
        n = len(data) // 16
        if n == 0:
            return
        words = struct.unpack_from(f"<{2 * n}Q", data)
        used = self._gif(self.paths[index & 3], words, n, index == 0)
        self.cur.c["gif_bytes"] += 16 * used

    def _gif(self, p, words, n, stop_at_eop):
        """Run GIF tags over n qwords; returns how many were consumed."""
        q = 0
        while q < n:
            if p.nloop == 0:
                if stop_at_eop and p.eop:
                    p.eop = False
                    return q
                lo, hi = words[2 * q], words[2 * q + 1]
                q += 1
                p.nloop = bits(lo, 0, 15)
                p.eop = bool(bits(lo, 15, 1))
                p.flg = bits(lo, 58, 2)
                nreg = bits(lo, 60, 4) or 16
                p.regs = [bits(hi, 4 * k, 4) for k in range(nreg)]
                p.cur = 0
                if p.flg == 0 and bits(lo, 46, 1):
                    self.write_reg(PRIM, bits(lo, 47, 11))
                if p.nloop == 0 and stop_at_eop and p.eop:
                    p.eop = False
                    return q
                continue
            if p.flg == 0:
                q = self._packed(p, words, q, n)
            elif p.flg == 1:
                q = self._reglist(p, words, q, n)
            else:
                k = min(p.nloop, n - q)
                self.cur.c["upload_bytes"] += 16 * k
                p.nloop -= k
                q += k
        if stop_at_eop and p.nloop == 0:
            p.eop = False
        return q

    def _packed(self, p, words, q, n):
        regs, nreg = p.regs, len(p.regs)
        while q < n and p.nloop:
            lo, hi = words[2 * q], words[2 * q + 1]
            q += 1
            r = regs[p.cur]
            if r == 0xE:
                self.write_reg(hi & 0xFF, lo)
            elif r == 0x1:
                self.rgba_a = bits(hi, 32, 8)
            elif r in (0x4, 0x5, 0xC, 0xD):
                kick = r in (0x4, 0x5) and not bits(hi, 47, 1)
                self.vertex(bits(lo, 0, 16), bits(lo, 32, 16), kick)
            elif r == 0x0:
                self.write_reg(PRIM, lo & 0x7FF)
            elif r in (0x6, 0x7):
                self.set_tex0(r - 0x6, lo)
            elif r in (0x8, 0x9):
                self.ctx[r - 0x8]["CLAMP"] = lo
            p.cur += 1
            if p.cur == nreg:
                p.cur = 0
                p.nloop -= 1
        return q

    def _reglist(self, p, words, q, n):
        regs, nreg = p.regs, len(p.regs)
        while q < n and p.nloop:
            for half in (0, 1):
                r = regs[p.cur]
                if r < 0xE:
                    self.write_reg(r, words[2 * q + half])
                p.cur += 1
                if p.cur == nreg:
                    p.cur = 0
                    p.nloop -= 1
                    if p.nloop == 0:
                        break  # an odd count leaves the high half as padding
            q += 1
        return q

    # -- frames -------------------------------------------------------------
    def privileged(self, regs):
        self.priv = regs
        d = self.display()
        self.disp_fbp = d.get("fbp") if d else None

    def display(self):
        r = self.priv
        if len(r) < 0xB0:
            return None
        pmode, smode2 = struct.unpack_from("<Q", r, 0x00)[0], \
            struct.unpack_from("<Q", r, 0x20)[0]
        circuit = 2 if bits(pmode, 1, 1) else 1 if bits(pmode, 0, 1) else 0
        if not circuit:
            return {"circuit": 0}
        fb, = struct.unpack_from("<Q", r, 0x70 if circuit == 1 else 0x90)
        disp, = struct.unpack_from("<Q", r, 0x80 if circuit == 1 else 0xA0)
        magh, magv = bits(disp, 23, 4), bits(disp, 27, 2)
        dw, dh = bits(disp, 32, 12), bits(disp, 44, 11)
        return {"circuit": circuit, "fbp": bits(fb, 0, 9),
                "fbw": bits(fb, 9, 6), "psm": PSM_NAMES.get(bits(fb, 15, 5),
                                                            hex(bits(fb, 15, 5))),
                "dbx": bits(fb, 32, 11), "dby": bits(fb, 43, 11),
                "w": (dw + 1) // (magh + 1), "h": (dh + 1) // (magv + 1),
                "magh": magh, "magv": magv,
                "interlaced": bits(smode2, 0, 1),
                "field_mode": bits(smode2, 1, 1)}

    def _close(self, vsync, boundary):
        self.cur.vsync = vsync
        self.cur.boundary = boundary
        self.cur.disp = self.display()
        self.chunks.append(self.cur)
        self.cur = Chunk()

    def vsync(self, field):
        disp = self.display()
        key = disp and (disp.get("fbp"), disp.get("dbx"), disp.get("dby"))
        flip = self.last_disp_key is not None and key != self.last_disp_key
        self.last_disp_key = key
        self._close(vsync=True, boundary="flip" if flip else None)
        self.batch_key = None

    def fifo_read(self, size):
        self.cur.c["fifo_read_bytes"] += 16 * size


def load_dump(path):
    raw = Path(path).read_bytes()
    if raw[:4] == b"\x28\xb5\x2f\xfd":
        if zstd is None:
            sys.exit("zstd GS dumps need Python 3.14+ (compression.zstd)")
        raw = zstd.decompress(raw)
    elif raw[:6] == b"\xfd7zXZ\x00":
        raw = lzma.decompress(raw)
    return raw


def replay(raw):
    first, = struct.unpack_from("<I", raw, 0)
    info = {}
    if first == 0xFFFFFFFF:
        hsize, = struct.unpack_from("<I", raw, 4)
        (ver, ssize, soff, slen, crc, sw, sh, _, _) = \
            struct.unpack_from("<9I", raw, 8)
        info = {"state_version": ver, "crc": f"{crc:08X}",
                "serial": raw[8 + soff:8 + soff + slen].decode("ascii",
                                                               "replace"),
                "screenshot": f"{sw}x{sh}"}
        pos = 8 + hsize
    else:
        ssize, = struct.unpack_from("<I", raw, 4)
        info = {"crc": f"{first:08X}", "format": "old"}
        pos = 8
    state = raw[pos:pos + ssize]
    pos += ssize
    gs = GSReplay(state, raw[pos:pos + PRIV_SIZE])
    pos += PRIV_SIZE
    end = len(raw)
    while pos < end:
        pid = raw[pos]
        pos += 1
        if pid == 0:
            index = raw[pos]
            size, = struct.unpack_from("<I", raw, pos + 1)
            pos += 5
            gs.transfer(index, raw[pos:pos + size])
            pos += size
        elif pid == 1:
            gs.vsync(raw[pos])
            pos += 1
        elif pid == 2:
            gs.fifo_read(struct.unpack_from("<I", raw, pos)[0])
            pos += 4
        elif pid == 3:
            gs.privileged(raw[pos:pos + PRIV_SIZE])
            pos += PRIV_SIZE
        else:
            gs.warnings[f"unknown packet id {pid} at {pos - 1}"] += 1
            break
    return info, gs


def assemble(chunks, vsyncs_per_frame):
    """Group chunks into complete frames.

    Frames run from one boundary (display flip or present) to the next;
    the partial frames before the first and after the last are dropped.
    Without boundaries, frames are windows of vsyncs_per_frame vsyncs.
    """
    bounds = [i for i, ch in enumerate(chunks) if ch.boundary]
    groups = []
    if len(bounds) >= 2:
        mode = "+".join(sorted({chunks[i].boundary for i in bounds}))
        groups = [chunks[a + 1:b + 1] for a, b in zip(bounds, bounds[1:])]
    else:
        mode = f"{vsyncs_per_frame}-vsync window"
        group, seen = [], 0
        for ch in chunks:
            group.append(ch)
            seen += ch.vsync
            if seen == vsyncs_per_frame:
                groups.append(group)
                group, seen = [], 0
    frames = []
    for grp in groups:
        f = Chunk()
        for ch in grp:
            f.merge(ch)
        f.c["vsyncs"] = sum(ch.vsync for ch in grp)
        frames.append(f)
    return mode, frames


METRICS = [
    "vsyncs", "draws", "prims", "prims_tri", "prims_sprite", "prims_line",
    "prims_point", "vertices", "fill_px", "main_rt_px", "overdraw",
    "fill_px_panel", "fill_blend_px", "fill_tex_px", "fill_z_px",
    "gif_bytes", "uploads", "upload_bytes", "clut_loads", "copies",
    "copy_bytes", "readbacks", "readback_bytes", "fifo_read_bytes",
    "rt_count", "rt_switches", "draws_textured", "draws_mipmapped",
    "uniq_tex_clut", "tex_bytes_native", "tex_bytes_rgba8", "draws_blend",
    "draws_blend_nonfixed", "draws_blend_factor_gt_1",
    "draws_blend_dst_alpha", "draws_blend_fix_gt_1", "draws_blend_wrap",
    "draws_blend_pabe", "draws_blend_aa1", "draws_blend_src_alpha",
    "prims_src_alpha_gt_1", "draws_date", "draws_afail_partial",
    "draws_tex_from_current_rt", "draws_tex_from_rt", "texflush"]


def rt_name(k):
    return f"fbp{k[0]}/w{k[1] * 64}/{PSM_NAMES.get(k[2], k[2])}"


def frame_row(f):
    """Flatten a frame. Overdraw is relative to the main render target (the
    largest drawn extent), which is also what the panel scaling uses: the
    device renders that target at native size and scales it to 640x480."""
    row = {k: f.c.get(k, 0) for k in METRICS}
    main = max(f.rts.items(), default=(None, [0, 0, 640, 448]),
               key=lambda kv: (kv[1][2] - kv[1][0]) * (kv[1][3] - kv[1][1]))
    bb = main[1]
    main_px = max(1, round((bb[2] - bb[0]) * (bb[3] - bb[1])))
    row["main_rt_px"] = main_px
    row["overdraw"] = round(f.c["fill_px"] / main_px, 3)
    row["fill_px_panel"] = round(f.c["fill_px"] * PANEL_PX / main_px)
    row["fill_px"] = round(f.c["fill_px"])
    for k in ("fill_blend_px", "fill_tex_px", "fill_z_px"):
        row[k] = round(f.c[k])
    row["rt_count"] = len(f.rts)
    row["uniq_tex_clut"] = len(f.tex)
    row["tex_bytes_native"] = sum(v[0] for v in f.tex.values())
    row["tex_bytes_rgba8"] = sum(v[1] for v in f.tex.values())
    d = f.disp or {}
    row["display"] = (f"{d.get('w')}x{d.get('h')} {d.get('psm')} "
                      f"fbp={d.get('fbp')} int={d.get('interlaced')} "
                      f"field={d.get('field_mode')}") if d.get("w") else "-"
    row["main_rt"] = rt_name(main[0]) if main[0] else "-"
    row["render_targets"] = ";".join(
        f"{rt_name(k)}:{int(b[2] - b[0])}x{int(b[3] - b[1])}"
        f" fill={round(f.rtfill[k])}"
        for k, b in sorted(f.rts.items()))
    return row


def pct(values, p):
    """Nearest-rank percentile."""
    s = sorted(values)
    if not s:
        return 0
    return s[max(0, math.ceil(p / 100 * len(s)) - 1)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("inputs", nargs="+", type=Path)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--out", type=Path,
                    default=Path("D:/ps2port-data/reports"))
    ap.add_argument("--vsyncs-per-frame", type=int, default=2)
    args = ap.parse_args(argv)
    files = []
    for p in args.inputs:
        files += sorted(f for f in p.iterdir() if ".gs" in f.name) \
            if p.is_dir() else [p]
    if not files:
        sys.exit("no GS dumps found")
    out = args.out / args.scene
    out.mkdir(parents=True, exist_ok=True)
    rows, dumps, rts, displays, zbufs, psms = [], [], {}, Counter(), \
        Counter(), Counter()
    for f in files:
        info, gs = replay(load_dump(f))
        mode, frames = assemble(gs.chunks, args.vsyncs_per_frame)
        vsyncs = sum(ch.vsync for ch in gs.chunks)
        pacing = Counter(fr.c["vsyncs"] for fr in frames)
        if pacing.get(0):
            gs.warnings["frames with 0 vsyncs: boundary detection may "
                        "split frames"] += pacing[0]
        dumps.append({"file": f.name, **info, "vsyncs": vsyncs,
                      "frames": len(frames), "frame_mode": mode,
                      "vsyncs_per_frame": dict(sorted(pacing.items())),
                      "warnings": dict(gs.warnings)})
        for i, fr in enumerate(frames):
            row = frame_row(fr)
            rows.append({"scene": args.scene, "dump": f.name, "frame": i,
                         **row})
            displays[row["display"]] += 1
            for k, b in fr.rts.items():
                cur = rts.setdefault(k, [0, 0, 0, 0.0])
                cur[0] = max(cur[0], int(b[2] - b[0]))
                cur[1] = max(cur[1], int(b[3] - b[1]))
                cur[2] += 1
                cur[3] += fr.rtfill[k]
            for z in fr.zbufs:
                zbufs[f"zbp{z[0]}/{PSM_NAMES.get(z[1], z[1])}"] += 1
            for key in fr.tex:
                psms[PSM_NAMES.get(key[2], key[2])] += 1
        print(f"{f.name}: {vsyncs} vsyncs, {len(frames)} frames ({mode}); "
              f"vsyncs/frame {dict(sorted(pacing.items()))}"
              + (f"; warnings {dict(gs.warnings)}" if gs.warnings else ""))
    cols = ["scene", "dump", "frame"] + METRICS + ["display", "main_rt",
                                                    "render_targets"]
    with open(out / "frames.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)
    stats = {}
    for k in METRICS:
        vals = [r[k] for r in rows]
        stats[k] = {"max": max(vals, default=0), "p95": pct(vals, 95),
                    "mean": round(sum(vals) / len(vals), 3) if vals else 0}
    summary = {
        "scene": args.scene, "frames": len(rows), "dumps": dumps,
        "metrics": stats, "displays": dict(displays),
        "render_targets": {
            rt_name(k): {"max_w": v[0], "max_h": v[1], "frames": v[2],
                         "mean_fill_px": round(v[3] / max(1, v[2]))}
            for k, v in sorted(rts.items())},
        "depth_buffers": dict(zbufs),
        "texture_formats_per_frame_uses": dict(psms)}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                      newline="\n")
    key_metrics = ["vsyncs", "draws", "vertices", "prims", "fill_px",
                   "overdraw", "fill_px_panel", "fill_blend_px",
                   "upload_bytes", "uploads", "uniq_tex_clut",
                   "tex_bytes_rgba8", "draws_blend_nonfixed",
                   "draws_tex_from_current_rt", "draws_tex_from_rt",
                   "readbacks", "copies", "gif_bytes"]
    lines = [f"# GS stats: {args.scene}", "",
             f"{len(rows)} complete frames from {len(files)} dump(s).", "",
             "| metric | max | p95 | mean |", "|---|---|---|---|"]
    for k in key_metrics:
        s = stats[k]
        lines.append(f"| {k} | {s['max']} | {s['p95']} | {s['mean']} |")
    lines += ["", "Displays: " + ", ".join(f"{k} ({v})" for k, v in
                                           displays.items()), "",
              "Render targets (max drawn extent, mean fill per frame):"]
    lines += [f"- {k}: {v['max_w']}x{v['max_h']} in {v['frames']} frames, "
              f"{v['mean_fill_px']} px"
              for k, v in summary["render_targets"].items()]
    lines += ["", "Frame boundaries: " + ", ".join(
        f"{d['file']}: {d['frame_mode']} {d['vsyncs_per_frame']}"
        for d in dumps)]
    (out / "summary.md").write_text("\n".join(lines) + "\n", newline="\n")
    print(f"{len(rows)} frames -> {out}")
    for k in ("vsyncs", "draws", "vertices", "fill_px", "overdraw",
              "upload_bytes"):
        print(f"  {k:14s} max {stats[k]['max']:>12} p95 {stats[k]['p95']:>12}"
              f" mean {stats[k]['mean']:>12}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
