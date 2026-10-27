/* T5, CPU path: PS2 GS-swizzled PSMT8 / PSMT4 textures plus a CLUT
   (CSM1, 32-bit entries) converted to linear RGBA8.

   GS local memory is 4 MiB of 256-byte blocks. Pages are 32 blocks.
   PSMT8 pages are 128x64 texels in 16x16 blocks; PSMT4 pages are 128x128
   in 32x16 blocks. The layout tables are the ones PCSX2's GS uses
   (GSdx GSTables). The self-test checks that they are permutations and
   that every fast path matches per-texel address arithmetic. Byte-exact
   checks against a real GS dump's VRAM are left to Phase 3. */
#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#define HAVE_NEON 1
#else
#define HAVE_NEON 0
#endif

#define GS_BYTES (4u << 20)

static const uint8_t block8[4][8] = {
    { 0, 1, 4, 5, 16, 17, 20, 21 },
    { 2, 3, 6, 7, 18, 19, 22, 23 },
    { 8, 9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
};

static const uint8_t block4[8][4] = {
    { 0, 2, 8, 10 },   { 1, 3, 9, 11 },   { 4, 6, 12, 14 },  { 5, 7, 13, 15 },
    { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 },
};

/* rows 8..15 are rows 0..7 plus 128 (PSMT8) / 256 (PSMT4); filled in init */
static const uint8_t column8_lo[8][16] = {
    { 0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54 },
    { 8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62 },
    { 33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23 },
    { 41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31 },
    { 96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86 },
    { 104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94 },
    { 65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119 },
    { 73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127 },
};

static const uint16_t column4_lo[8][32] = {
    { 0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106,
      4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110 },
    { 16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122,
      20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126 },
    { 65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43,
      69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47 },
    { 81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59,
      85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63 },
    { 192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170,
      196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174 },
    { 208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186,
      212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190 },
    { 129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235,
      133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239 },
    { 145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251,
      149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255 },
};

static uint8_t column8[16][16];   /* byte offset within a block */
static uint16_t column4[16][32];  /* nibble offset within a block */
static uint8_t col8_rel[16][16];  /* offset within the row's 64-byte column */
static uint8_t col4_rel[16][32];  /* nibble offset within the row's column */

static void tables_init(void)
{
    static int done;
    if (done)
        return;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            column8[y][x] = (uint8_t)(column8_lo[y & 7][x] + (y >> 3) * 128);
            col8_rel[y][x] = (uint8_t)(column8[y][x] - (y >> 2) * 64);
        }
        for (int x = 0; x < 32; x++) {
            column4[y][x] = (uint16_t)(column4_lo[y & 7][x] + (y >> 3) * 256);
            col4_rel[y][x] = (uint8_t)(column4[y][x] - (y >> 2) * 128);
        }
    }
    done = 1;
}

typedef struct {
    uint32_t tbp, tbw, w, h;  /* base block, width in 64-texel units, size */
    int bpp;                  /* 8 or 4 */
    uint32_t pal[256];        /* CLUT already de-swizzled (CSM1) to index order */
} tex;

static uint32_t csm1_index(uint32_t i)
{
    return (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
}

/* PSMT8: byte address of texel (x, y) */
static uint32_t addr8(const tex *t, uint32_t x, uint32_t y)
{
    uint32_t page = (y >> 6) * (t->tbw >> 1) + (x >> 7);
    uint32_t blk = t->tbp + page * 32 + block8[(y >> 4) & 3][(x >> 4) & 7];
    return (blk * 256u + column8[y & 15][x & 15]) & (GS_BYTES - 1);
}

/* PSMT4: nibble address of texel (x, y) */
static uint32_t addr4(const tex *t, uint32_t x, uint32_t y)
{
    uint32_t page = (y >> 7) * (t->tbw >> 1) + (x >> 7);
    uint32_t blk = t->tbp + page * 32 + block4[(y >> 4) & 7][(x >> 5) & 3];
    return (blk * 512u + column4[y & 15][x & 31]) & (GS_BYTES * 2 - 1);
}

static const uint8_t *block_ptr(const uint8_t *gs, const tex *t, uint32_t bx, uint32_t by)
{
    uint32_t blk;
    if (t->bpp == 8) {
        uint32_t page = (by >> 6) * (t->tbw >> 1) + (bx >> 7);
        blk = t->tbp + page * 32 + block8[(by >> 4) & 3][(bx >> 4) & 7];
    } else {
        uint32_t page = (by >> 7) * (t->tbw >> 1) + (bx >> 7);
        blk = t->tbp + page * 32 + block4[(by >> 4) & 7][(bx >> 5) & 3];
    }
    return gs + ((blk * 256u) & (GS_BYTES - 1));
}

/* reference: per-texel address arithmetic */
static void conv_ref(const uint8_t *gs, const tex *t, uint32_t *out)
{
    for (uint32_t y = 0; y < t->h; y++)
        for (uint32_t x = 0; x < t->w; x++) {
            uint32_t idx;
            if (t->bpp == 8) {
                idx = gs[addr8(t, x, y)];
            } else {
                uint32_t n = addr4(t, x, y);
                idx = n & 1 ? gs[n >> 1] >> 4 : gs[n >> 1] & 15;
            }
            out[y * t->w + x] = t->pal[idx];
        }
}

static void conv_scalar(const uint8_t *gs, const tex *t, uint32_t *out)
{
    const uint32_t *pal = t->pal;
    if (t->bpp == 8) {
        for (uint32_t by = 0; by < t->h; by += 16)
            for (uint32_t bx = 0; bx < t->w; bx += 16) {
                const uint8_t *blk = block_ptr(gs, t, bx, by);
                uint32_t *o = out + by * t->w + bx;
                for (int y = 0; y < 16; y++, o += t->w) {
                    const uint8_t *cr = column8[y];
                    for (int x = 0; x < 16; x++)
                        o[x] = pal[blk[cr[x]]];
                }
            }
    } else {
        for (uint32_t by = 0; by < t->h; by += 16)
            for (uint32_t bx = 0; bx < t->w; bx += 32) {
                const uint8_t *blk = block_ptr(gs, t, bx, by);
                uint32_t *o = out + by * t->w + bx;
                for (int y = 0; y < 16; y++, o += t->w) {
                    const uint16_t *cr = column4[y];
                    for (int x = 0; x < 32; x++) {
                        uint32_t n = cr[x], b = blk[n >> 1];
                        o[x] = pal[n & 1 ? b >> 4 : b & 15];
                    }
                }
            }
    }
}

#if HAVE_NEON
/* PSMT8: TBL de-swizzle (one vqtbl4q per row of 16), then the CLUT either
   as four byte planes through TBL (neon_tbl) or as scalar loads (neon_sclut). */
static void conv8_neon(const uint8_t *gs, const tex *t, uint32_t *out, int tbl_clut)
{
    uint8_t planes[4][256] __attribute__((aligned(16)));
    uint8_t idx[256] __attribute__((aligned(16)));
    uint8_t pout[4][256] __attribute__((aligned(16)));
    for (int i = 0; i < 256; i++)
        for (int c = 0; c < 4; c++)
            planes[c][i] = (uint8_t)(t->pal[i] >> (8 * c));
    uint8x16_t k64 = vdupq_n_u8(64);
    for (uint32_t by = 0; by < t->h; by += 16)
        for (uint32_t bx = 0; bx < t->w; bx += 16) {
            const uint8_t *blk = block_ptr(gs, t, bx, by);
            uint32_t *o = out + by * t->w + bx;
            for (int c = 0; c < 4; c++) {
                uint8x16x4_t col = { { vld1q_u8(blk + c * 64), vld1q_u8(blk + c * 64 + 16),
                                       vld1q_u8(blk + c * 64 + 32), vld1q_u8(blk + c * 64 + 48) } };
                for (int r = 0; r < 4; r++) {
                    int row = c * 4 + r;
                    vst1q_u8(idx + row * 16, vqtbl4q_u8(col, vld1q_u8(col8_rel[row])));
                }
            }
            if (!tbl_clut) {
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++)
                        o[y * t->w + x] = t->pal[idx[y * 16 + x]];
                continue;
            }
            for (int p = 0; p < 4; p++) {
                const uint8_t *pl = planes[p];
                uint8x16x4_t q0 = { { vld1q_u8(pl), vld1q_u8(pl + 16), vld1q_u8(pl + 32), vld1q_u8(pl + 48) } };
                uint8x16x4_t q1 = { { vld1q_u8(pl + 64), vld1q_u8(pl + 80), vld1q_u8(pl + 96), vld1q_u8(pl + 112) } };
                uint8x16x4_t q2 = { { vld1q_u8(pl + 128), vld1q_u8(pl + 144), vld1q_u8(pl + 160), vld1q_u8(pl + 176) } };
                uint8x16x4_t q3 = { { vld1q_u8(pl + 192), vld1q_u8(pl + 208), vld1q_u8(pl + 224), vld1q_u8(pl + 240) } };
                for (int row = 0; row < 16; row++) {
                    uint8x16_t i0 = vld1q_u8(idx + row * 16);
                    uint8x16_t i1 = vsubq_u8(i0, k64), i2 = vsubq_u8(i1, k64), i3 = vsubq_u8(i2, k64);
                    uint8x16_t v = vqtbl4q_u8(q0, i0);
                    v = vqtbx4q_u8(v, q1, i1);
                    v = vqtbx4q_u8(v, q2, i2);
                    v = vqtbx4q_u8(v, q3, i3);
                    vst1q_u8(pout[p] + row * 16, v);
                }
            }
            for (int row = 0; row < 16; row++) {
                uint8x16x4_t px = { { vld1q_u8(pout[0] + row * 16), vld1q_u8(pout[1] + row * 16),
                                      vld1q_u8(pout[2] + row * 16), vld1q_u8(pout[3] + row * 16) } };
                vst4q_u8((uint8_t *)(o + row * t->w), px);
            }
        }
}

/* PSMT4: nibbles expanded to bytes (and + shift + zip), TBL de-swizzle over
   the 128-nibble column, 16-entry CLUT planes through vqtbl1q. */
static void conv4_neon(const uint8_t *gs, const tex *t, uint32_t *out)
{
    uint8_t planes[4][16] __attribute__((aligned(16)));
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++)
            planes[c][i] = (uint8_t)(t->pal[i] >> (8 * c));
    uint8x16_t p0 = vld1q_u8(planes[0]), p1 = vld1q_u8(planes[1]);
    uint8x16_t p2 = vld1q_u8(planes[2]), p3 = vld1q_u8(planes[3]);
    uint8x16_t lo4 = vdupq_n_u8(15), k64 = vdupq_n_u8(64);
    for (uint32_t by = 0; by < t->h; by += 16)
        for (uint32_t bx = 0; bx < t->w; bx += 32) {
            const uint8_t *blk = block_ptr(gs, t, bx, by);
            uint32_t *o = out + by * t->w + bx;
            for (int c = 0; c < 4; c++) {
                uint8x16_t e[8];
                for (int i = 0; i < 4; i++) {
                    uint8x16_t b = vld1q_u8(blk + c * 64 + i * 16);
                    uint8x16_t lo = vandq_u8(b, lo4), hi = vshrq_n_u8(b, 4);
                    e[2 * i] = vzip1q_u8(lo, hi);
                    e[2 * i + 1] = vzip2q_u8(lo, hi);
                }
                uint8x16x4_t ta = { { e[0], e[1], e[2], e[3] } };
                uint8x16x4_t tb = { { e[4], e[5], e[6], e[7] } };
                for (int r = 0; r < 4; r++) {
                    int row = c * 4 + r;
                    for (int h = 0; h < 2; h++) {
                        uint8x16_t i0 = vld1q_u8(col4_rel[row] + h * 16);
                        uint8x16_t v = vqtbl4q_u8(ta, i0);
                        v = vqtbx4q_u8(v, tb, vsubq_u8(i0, k64));
                        uint8x16x4_t px = { { vqtbl1q_u8(p0, v), vqtbl1q_u8(p1, v),
                                              vqtbl1q_u8(p2, v), vqtbl1q_u8(p3, v) } };
                        vst4q_u8((uint8_t *)(o + row * t->w + h * 16), px);
                    }
                }
            }
        }
}
#endif

enum { V_SCALAR, V_NEON_TBL, V_NEON_SCLUT, V_COUNT };
static const char *vname[V_COUNT] = { "scalar", "neon_tbl", "neon_sclut" };

static int variant_ok(int v, int bpp)
{
    if (v == V_SCALAR)
        return 1;
    if (!HAVE_NEON)
        return 0;
    return bpp == 8 || v == V_NEON_TBL;
}

static void convert(int v, const uint8_t *gs, const tex *t, uint32_t *out)
{
#if HAVE_NEON
    if (v == V_NEON_TBL) {
        if (t->bpp == 8)
            conv8_neon(gs, t, out, 1);
        else
            conv4_neon(gs, t, out);
        return;
    }
    if (v == V_NEON_SCLUT) {
        conv8_neon(gs, t, out, 0);
        return;
    }
#else
    (void)v;
#endif
    conv_scalar(gs, t, out);
}

static uint8_t *gs_alloc(uint64_t seed)
{
    uint8_t *gs = aligned_alloc(64, GS_BYTES);
    if (!gs)
        abort();
    for (uint32_t i = 0; i < GS_BYTES; i += 8) {
        uint64_t r = rng_next(&seed);
        memcpy(gs + i, &r, 8);
    }
    return gs;
}

static void tex_init(tex *t, int bpp, uint32_t w, uint32_t h, uint32_t tbp, uint64_t *seed)
{
    uint32_t raw[256];
    t->bpp = bpp;
    t->w = w;
    t->h = h;
    t->tbw = ((w + 127) / 128) * 2;  /* both formats have 128-texel pages */
    t->tbp = tbp;
    for (int i = 0; i < 256; i++)
        raw[i] = (uint32_t)rng_next(seed);
    for (uint32_t i = 0; i < 256; i++)
        t->pal[i] = bpp == 8 ? raw[csm1_index(i)] : raw[i & 15];
}

/* ---- self-test ---------------------------------------------------------- */

static int check_perm(const char *name, const uint32_t *vals, int n)
{
    uint8_t seen[512] = { 0 };
    for (int i = 0; i < n; i++) {
        if (vals[i] >= (uint32_t)n || seen[vals[i]]) {
            printf("  FAIL %s is not a permutation (value %u)\n", name, vals[i]);
            return 1;
        }
        seen[vals[i]] = 1;
    }
    return 0;
}

int t5_selftest(void)
{
    tables_init();
    int fails = 0;
    uint32_t v[512];
    for (int i = 0; i < 256; i++)
        v[i] = column8[i / 16][i % 16];
    fails += check_perm("column8", v, 256);
    for (int i = 0; i < 512; i++)
        v[i] = column4[i / 32][i % 32];
    fails += check_perm("column4", v, 512);
    for (int i = 0; i < 32; i++)
        v[i] = block8[i / 8][i % 8];
    fails += check_perm("block8", v, 32);
    for (int i = 0; i < 32; i++)
        v[i] = block4[i / 4][i % 4];
    fails += check_perm("block4", v, 32);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++)
            fails += col8_rel[y][x] >= 64;
        for (int x = 0; x < 32; x++)
            fails += col4_rel[y][x] >= 128;
    }

    uint8_t *gs = gs_alloc(0x5EED);
    uint64_t seed = 42;
    uint32_t *ref = malloc(256 * 256 * 4), *out = malloc(256 * 256 * 4);
    static const uint32_t sizes[] = { 32, 64, 128, 256 };
    uint64_t sum = 0xcbf29ce484222325ULL;
    for (int bpp = 8; bpp >= 4; bpp -= 4)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            tex t;
            uint32_t w = sizes[s], h = sizes[s] / 2 < 16 ? 16 : sizes[s] / 2;
            if (bpp == 8 && w < 16)
                continue;
            tex_init(&t, bpp, w, h, (uint32_t)(rng_next(&seed) % 16000), &seed);
            conv_ref(gs, &t, ref);
            sum ^= fnv1a(ref, (size_t)w * h * 4) + (uint64_t)bpp * 31 + w;
            for (int vi = 0; vi < V_COUNT; vi++) {
                if (!variant_ok(vi, bpp))
                    continue;
                memset(out, 0, (size_t)w * h * 4);
                convert(vi, gs, &t, out);
                if (memcmp(ref, out, (size_t)w * h * 4)) {
                    printf("  FAIL T5 %s psmt%d %ux%u differs from reference\n", vname[vi], bpp, w, h);
                    fails++;
                }
            }
        }
    printf("  T5 checksum %016llx (must match across builds)\n", (unsigned long long)sum);
    free(ref);
    free(out);
    free(gs);
    return fails;
}

/* ---- benchmark ---------------------------------------------------------- */

typedef struct {
    const uint8_t *gs;
    tex *texs;
    int ntex, variant, reps;
    uint32_t *out[2];
} job;

static void conv_job(void *arg, int index, int count)
{
    job *j = arg;
    for (int r = 0; r < j->reps; r++)
        for (int i = index; i < j->ntex; i += count)
            convert(j->variant, j->gs, &j->texs[i], j->out[index]);
}

void t5_cpu_run(void)
{
    tables_init();
    printf("T5 CPU: PS2 swizzled PSMT8/PSMT4 + CLUT -> RGBA8\n");
    uint8_t *gs = gs_alloc(0xC0FFEE);
    uint64_t seed = 7;
    /* Part B: up to 1.63 MB of native texture data per frame (PHASE_1 C2) */
    const double partb_bytes = 1.63e6;
    static const uint32_t sizes[] = { 64, 128, 256 };
    const int ntex = 16;
    tex *texs = malloc(sizeof(tex) * ntex);
    uint32_t *out[2] = { malloc(256 * 256 * 4), malloc(256 * 256 * 4) };
    int maxthreads = cpu_count() >= 2 ? 2 : 1;
    for (int bpp = 8; bpp >= 4; bpp -= 4)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            uint32_t w = sizes[s];
            for (int i = 0; i < ntex; i++)
                tex_init(&texs[i], bpp, w, w, (uint32_t)(rng_next(&seed) % 12000), &seed);
            double texels = (double)w * w * ntex;
            for (int vi = 0; vi < V_COUNT; vi++) {
                if (!variant_ok(vi, bpp))
                    continue;
                for (int th = 1; th <= maxthreads; th++) {
                    job j = { gs, texs, ntex, vi, 1, { out[0], out[1] } };
                    /* size the repeat count to ~0.25 s (0.02 s quick) */
                    double t0 = now_s();
                    conv_job(&j, 0, 1);
                    double one = now_s() - t0;
                    double target = g_opts.quick ? 0.02 : 0.25;
                    j.reps = one > 0 ? (int)(target / one) + 1 : 1;
                    samples mtex = { { 0 }, 0 }, ms = { { 0 }, 0 };
                    for (int r = 0; r < g_opts.reps; r++) {
                        t0 = now_s();
                        run_threads(th, conv_job, &j, 1);
                        double dt = now_s() - t0;
                        double rate = texels * j.reps / dt;   /* texels/s */
                        samples_add(&mtex, rate / 1e6);
                        samples_add(&ms, partb_bytes * (8.0 / bpp) / rate * 1e3);
                    }
                    char var[64], par[160];
                    snprintf(var, sizeof var, "psmt%d_%u_%s_t%d", bpp, w, vname[vi], th);
                    snprintf(par, sizeof par,
                             "\"bpp\":%d,\"size\":%u,\"impl\":\"%s\",\"threads\":%d,\"partb_native_bytes\":%.0f",
                             bpp, w, vname[vi], th, partb_bytes);
                    result_add("T5", var, "mtexel_per_s", "Mtexel/s", &mtex, par);
                    result_add("T5", var, "ms_per_partb_frame", "ms", &ms, par);
                }
            }
        }
    free(texs);
    free(out[0]);
    free(out[1]);
    free(gs);
}
