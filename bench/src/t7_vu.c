/* T7: a VU1-style vertex kernel on NEON. 4x4 transform, clip flags,
   perspective divide, viewport, and packing to GS PACKED qwords
   (ST, RGBAQ, XYZ2: 48 bytes per vertex, as VU1 sends them). Optionally
   with 3 directional lights. The scalar version is the reference; the NEON
   version keeps the same operation order (no fused multiply-add, IEEE
   division) and must match it bit for bit. */
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

typedef struct {
    float m[16];              /* column-major model-view-projection */
    float vs[3], vo[3];       /* viewport scale / offset (GS coordinates) */
    float amb[3];
    float ldir[3][3], lcol[3][3];
} vu_consts;

typedef struct {
    float *pos;    /* x y z 1 per vertex */
    float *st;     /* s t */
    uint32_t *col; /* RGBA8 */
    float *nrm;    /* nx ny nz 0 */
    int n;
} vu_in;

#define XMAX 4095.9375f
#define ZMAX 16777215.0f

static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }

static void xform_scalar(const vu_consts *k, const vu_in *in, int lit, int i0, int i1, uint32_t *out)
{
    const float *m = k->m;
    for (int i = i0; i < i1; i++) {
        const float *p = in->pos + 4 * i;
        float x = p[0], y = p[1], z = p[2];
        float cx = m[0] * x; cx = cx + m[4] * y; cx = cx + m[8] * z; cx = cx + m[12];
        float cy = m[1] * x; cy = cy + m[5] * y; cy = cy + m[9] * z; cy = cy + m[13];
        float cz = m[2] * x; cz = cz + m[6] * y; cz = cz + m[10] * z; cz = cz + m[14];
        float cw = m[3] * x; cw = cw + m[7] * y; cw = cw + m[11] * z; cw = cw + m[15];
        float ncw = -cw;
        uint32_t clip = (uint32_t)(cx > cw) | (uint32_t)(cx < ncw) << 1 | (uint32_t)(cy > cw) << 2 |
                        (uint32_t)(cy < ncw) << 3 | (uint32_t)(cz > cw) << 4 | (uint32_t)(cz < ncw) << 5;
        float q = 1.0f / cw;
        float sx = cx * q; sx = sx * k->vs[0]; sx = sx + k->vo[0];
        float sy = cy * q; sy = sy * k->vs[1]; sy = sy + k->vo[1];
        float sz = cz * q; sz = sz * k->vs[2]; sz = sz + k->vo[2];
        sx = minf(maxf(sx, 0.0f), XMAX);
        sy = minf(maxf(sy, 0.0f), XMAX);
        sz = minf(maxf(sz, 0.0f), ZMAX);
        float s = in->st[2 * i] * q, t = in->st[2 * i + 1] * q;
        uint32_t c = in->col[i];
        float r = (float)(c & 255), g = (float)(c >> 8 & 255), b = (float)(c >> 16 & 255);
        uint32_t a = c >> 24;
        if (lit) {
            const float *nv = in->nrm + 4 * i;
            float d[3];
            for (int l = 0; l < 3; l++) {
                float dl = nv[0] * k->ldir[l][0];
                dl = dl + nv[1] * k->ldir[l][1];
                dl = dl + nv[2] * k->ldir[l][2];
                d[l] = maxf(dl, 0.0f);
            }
            float ir = k->amb[0], ig = k->amb[1], ib = k->amb[2];
            for (int l = 0; l < 3; l++) {
                ir = ir + d[l] * k->lcol[l][0];
                ig = ig + d[l] * k->lcol[l][1];
                ib = ib + d[l] * k->lcol[l][2];
            }
            r = minf(r * ir, 255.0f);
            g = minf(g * ig, 255.0f);
            b = minf(b * ib, 255.0f);
        }
        uint32_t *o = out + 12 * i;
        memcpy(&o[0], &s, 4);
        memcpy(&o[1], &t, 4);
        memcpy(&o[2], &q, 4);
        o[3] = 0;
        o[4] = (uint32_t)r;
        o[5] = (uint32_t)g;
        o[6] = (uint32_t)b;
        o[7] = a;
        o[8] = (uint32_t)(int32_t)(sx * 16.0f) & 0xFFFF;
        o[9] = (uint32_t)(int32_t)(sy * 16.0f) & 0xFFFF;
        o[10] = (uint32_t)sz;
        o[11] = clip ? 0x8000u : 0; /* ADC: don't draw */
    }
}

#if HAVE_NEON
static inline void store_t4(uint32_t *o, uint32x4_t a, uint32x4_t b, uint32x4_t c, uint32x4_t d)
{
    uint64x2_t ab0 = vreinterpretq_u64_u32(vzip1q_u32(a, b)), ab1 = vreinterpretq_u64_u32(vzip2q_u32(a, b));
    uint64x2_t cd0 = vreinterpretq_u64_u32(vzip1q_u32(c, d)), cd1 = vreinterpretq_u64_u32(vzip2q_u32(c, d));
    vst1q_u32(o + 0, vreinterpretq_u32_u64(vzip1q_u64(ab0, cd0)));
    vst1q_u32(o + 12, vreinterpretq_u32_u64(vzip2q_u64(ab0, cd0)));
    vst1q_u32(o + 24, vreinterpretq_u32_u64(vzip1q_u64(ab1, cd1)));
    vst1q_u32(o + 36, vreinterpretq_u32_u64(vzip2q_u64(ab1, cd1)));
}

static inline float32x4_t row4(const float *m, int r, float32x4_t x, float32x4_t y, float32x4_t z)
{
    float32x4_t v = vmulq_n_f32(x, m[r]);
    v = vaddq_f32(v, vmulq_n_f32(y, m[4 + r]));
    v = vaddq_f32(v, vmulq_n_f32(z, m[8 + r]));
    return vaddq_f32(v, vdupq_n_f32(m[12 + r]));
}

static void xform_neon(const vu_consts *k, const vu_in *in, int lit, int i0, int i1, uint32_t *out)
{
    const float *m = k->m;
    const float32x4_t zero = vdupq_n_f32(0.0f), xmax = vdupq_n_f32(XMAX), zmax = vdupq_n_f32(ZMAX);
    const float32x4_t c255 = vdupq_n_f32(255.0f), one = vdupq_n_f32(1.0f);
    const uint32x4_t m255 = vdupq_n_u32(255), mffff = vdupq_n_u32(0xFFFF);
    int i = i0;
    for (; i + 4 <= i1; i += 4) {
        float32x4x4_t P = vld4q_f32(in->pos + 4 * i);
        float32x4_t cx = row4(m, 0, P.val[0], P.val[1], P.val[2]);
        float32x4_t cy = row4(m, 1, P.val[0], P.val[1], P.val[2]);
        float32x4_t cz = row4(m, 2, P.val[0], P.val[1], P.val[2]);
        float32x4_t cw = row4(m, 3, P.val[0], P.val[1], P.val[2]);
        float32x4_t ncw = vnegq_f32(cw);
        uint32x4_t clip = vorrq_u32(vcgtq_f32(cx, cw), vcltq_f32(cx, ncw));
        clip = vorrq_u32(clip, vorrq_u32(vcgtq_f32(cy, cw), vcltq_f32(cy, ncw)));
        clip = vorrq_u32(clip, vorrq_u32(vcgtq_f32(cz, cw), vcltq_f32(cz, ncw)));
        float32x4_t q = vdivq_f32(one, cw);
        float32x4_t sx = vaddq_f32(vmulq_n_f32(vmulq_f32(cx, q), k->vs[0]), vdupq_n_f32(k->vo[0]));
        float32x4_t sy = vaddq_f32(vmulq_n_f32(vmulq_f32(cy, q), k->vs[1]), vdupq_n_f32(k->vo[1]));
        float32x4_t sz = vaddq_f32(vmulq_n_f32(vmulq_f32(cz, q), k->vs[2]), vdupq_n_f32(k->vo[2]));
        sx = vminq_f32(vmaxq_f32(sx, zero), xmax);
        sy = vminq_f32(vmaxq_f32(sy, zero), xmax);
        sz = vminq_f32(vmaxq_f32(sz, zero), zmax);
        float32x4x2_t ST = vld2q_f32(in->st + 2 * i);
        float32x4_t s = vmulq_f32(ST.val[0], q), t = vmulq_f32(ST.val[1], q);
        uint32x4_t c = vld1q_u32(in->col + i);
        float32x4_t r = vcvtq_f32_u32(vandq_u32(c, m255));
        float32x4_t g = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(c, 8), m255));
        float32x4_t b = vcvtq_f32_u32(vandq_u32(vshrq_n_u32(c, 16), m255));
        uint32x4_t a = vshrq_n_u32(c, 24);
        if (lit) {
            float32x4x4_t N = vld4q_f32(in->nrm + 4 * i);
            float32x4_t ir = vdupq_n_f32(k->amb[0]), ig = vdupq_n_f32(k->amb[1]), ib = vdupq_n_f32(k->amb[2]);
            float32x4_t d[3];
            for (int l = 0; l < 3; l++) {
                float32x4_t dl = vmulq_n_f32(N.val[0], k->ldir[l][0]);
                dl = vaddq_f32(dl, vmulq_n_f32(N.val[1], k->ldir[l][1]));
                dl = vaddq_f32(dl, vmulq_n_f32(N.val[2], k->ldir[l][2]));
                d[l] = vmaxq_f32(dl, zero);
            }
            for (int l = 0; l < 3; l++) {
                ir = vaddq_f32(ir, vmulq_n_f32(d[l], k->lcol[l][0]));
                ig = vaddq_f32(ig, vmulq_n_f32(d[l], k->lcol[l][1]));
                ib = vaddq_f32(ib, vmulq_n_f32(d[l], k->lcol[l][2]));
            }
            r = vminq_f32(vmulq_f32(r, ir), c255);
            g = vminq_f32(vmulq_f32(g, ig), c255);
            b = vminq_f32(vmulq_f32(b, ib), c255);
        }
        uint32_t *o = out + 12 * i;
        store_t4(o + 0, vreinterpretq_u32_f32(s), vreinterpretq_u32_f32(t), vreinterpretq_u32_f32(q),
                 vdupq_n_u32(0));
        store_t4(o + 4, vcvtq_u32_f32(r), vcvtq_u32_f32(g), vcvtq_u32_f32(b), a);
        uint32x4_t X = vandq_u32(vreinterpretq_u32_s32(vcvtq_s32_f32(vmulq_n_f32(sx, 16.0f))), mffff);
        uint32x4_t Y = vandq_u32(vreinterpretq_u32_s32(vcvtq_s32_f32(vmulq_n_f32(sy, 16.0f))), mffff);
        uint32x4_t adc = vandq_u32(clip, vdupq_n_u32(0x8000));
        store_t4(o + 8, X, Y, vcvtq_u32_f32(sz), adc);
    }
    if (i < i1)
        xform_scalar(k, in, lit, i, i1, out);
}
#endif

static void consts_init(vu_consts *k)
{
    /* perspective: fov ~60 deg, aspect 4:3, near 1, far 1000, camera at origin */
    memset(k, 0, sizeof *k);
    float f = 1.732f, n = 1.0f, fa = 1000.0f;
    k->m[0] = f / 1.333f;
    k->m[5] = f;
    k->m[10] = (fa + n) / (n - fa);
    k->m[11] = -1.0f;
    k->m[14] = 2.0f * fa * n / (n - fa);
    k->vs[0] = 320.0f, k->vs[1] = -224.0f, k->vs[2] = 8388607.0f;
    k->vo[0] = 2048.0f, k->vo[1] = 2048.0f, k->vo[2] = 8388608.0f;
    k->amb[0] = 0.3f, k->amb[1] = 0.3f, k->amb[2] = 0.35f;
    const float ld[3][3] = { { 0.577f, 0.577f, 0.577f }, { -0.707f, 0.0f, 0.707f }, { 0.0f, -1.0f, 0.0f } };
    const float lc[3][3] = { { 0.8f, 0.75f, 0.7f }, { 0.2f, 0.25f, 0.4f }, { 0.1f, 0.1f, 0.1f } };
    memcpy(k->ldir, ld, sizeof ld);
    memcpy(k->lcol, lc, sizeof lc);
}

static float frand(uint64_t *s, float lo, float hi)
{
    return lo + (hi - lo) * (float)((double)(rng_next(s) >> 11) / 9007199254740992.0);
}

static void *alloc16(size_t bytes)
{
    void *p = aligned_alloc(16, (bytes + 64 + 15) & ~(size_t)15);
    if (!p)
        abort();
    return p;
}

static void input_init(vu_in *in, int n, uint64_t seed)
{
    in->n = n;
    in->pos = alloc16((size_t)n * 16);
    in->st = alloc16((size_t)n * 8);
    in->col = alloc16((size_t)n * 4);
    in->nrm = alloc16((size_t)n * 16);
    for (int i = 0; i < n; i++) {
        /* in front of the camera; ~10 % outside the frustum */
        float z = frand(&seed, -200.0f, -2.0f);
        in->pos[4 * i + 0] = frand(&seed, -1.0f, 1.0f) * -z * 0.85f;
        in->pos[4 * i + 1] = frand(&seed, -1.0f, 1.0f) * -z * 0.65f;
        in->pos[4 * i + 2] = z;
        in->pos[4 * i + 3] = 1.0f;
        in->st[2 * i] = frand(&seed, 0.0f, 1.0f);
        in->st[2 * i + 1] = frand(&seed, 0.0f, 1.0f);
        in->col[i] = (uint32_t)rng_next(&seed) | 0x80000000u;
        float nx = frand(&seed, -1, 1), ny = frand(&seed, -1, 1), nz = frand(&seed, -1, 1);
        in->nrm[4 * i] = nx, in->nrm[4 * i + 1] = ny, in->nrm[4 * i + 2] = nz, in->nrm[4 * i + 3] = 0;
    }
}

static void input_free(vu_in *in)
{
    free(in->pos);
    free(in->st);
    free(in->col);
    free(in->nrm);
}

int t7_selftest(void)
{
    int fails = 0;
    vu_consts k;
    consts_init(&k);
    vu_in in;
    const int n = 1027; /* not a multiple of 4: exercises the scalar tail */
    input_init(&in, n, 0x7E57);
    uint32_t *a = malloc((size_t)n * 48), *b = malloc((size_t)n * 48);
    for (int lit = 0; lit < 2; lit++) {
        xform_scalar(&k, &in, lit, 0, n, a);
        printf("  T7 %-5s checksum %016llx (must match across builds)\n", lit ? "lit" : "unlit",
               (unsigned long long)fnv1a(a, (size_t)n * 48));
#if HAVE_NEON
        memset(b, 0, (size_t)n * 48);
        xform_neon(&k, &in, lit, 0, n, b);
        if (memcmp(a, b, (size_t)n * 48)) {
            int first = 0;
            while (a[first] == b[first])
                first++;
            printf("  FAIL T7 neon %s differs from scalar at vertex %d word %d (%08x vs %08x)\n",
                   lit ? "lit" : "unlit", first / 12, first % 12, a[first], b[first]);
            fails++;
        }
#endif
    }
    free(a);
    free(b);
    input_free(&in);
    return fails;
}

typedef struct {
    const vu_consts *k;
    const vu_in *in;
    uint32_t *out;
    int lit, neon, frames;
} t7_job;

static void t7_thread(void *arg, int index, int count)
{
    t7_job *j = arg;
    int n = j->in->n, i0 = (int)((long)n * index / count) & ~3, i1 = index == count - 1 ? n : (int)((long)n * (index + 1) / count) & ~3;
    for (int f = 0; f < j->frames; f++) {
#if HAVE_NEON
        if (j->neon) {
            xform_neon(j->k, j->in, j->lit, i0, i1, j->out);
            continue;
        }
#endif
        xform_scalar(j->k, j->in, j->lit, i0, i1, j->out);
    }
}

void t7_run(void)
{
    printf("T7: VU1-style transform kernel\n");
    /* Part B: 132.5k vertices per frame at p95 (PHASE_1 C2) */
    const int n = 132500;
    vu_consts k;
    consts_init(&k);
    vu_in in;
    input_init(&in, n, 0xB0B);
    uint32_t *out = alloc16((size_t)n * 48);
    int maxthreads = cpu_count() >= 2 ? 2 : 1;
    for (int neon = 0; neon <= HAVE_NEON; neon++)
        for (int lit = 0; lit < 2; lit++)
            for (int th = 1; th <= maxthreads; th++) {
                t7_job j = { &k, &in, out, lit, neon, 1 };
                run_threads(th, t7_thread, &j, 1); /* untimed: page faults, caches */
                j.frames = g_opts.quick ? 1 : 20;
                samples ns = { { 0 }, 0 }, cyc = { { 0 }, 0 }, core = { { 0 }, 0 };
                for (int r = 0; r < g_opts.reps; r++) {
                    double t0 = now_s();
                    run_threads(th, t7_thread, &j, 1);
                    double dt = now_s() - t0;
                    double nsv = dt * 1e9 / ((double)n * j.frames);
                    samples_add(&ns, nsv);
                    double mhz = cpu_cur_freq_mhz(0);
                    if (mhz > 0)
                        samples_add(&cyc, nsv * mhz / 1000.0 * th);
                    samples_add(&core, nsv * 1e-9 * n * 30.0 * th);
                }
                char var[48], par[160];
                snprintf(var, sizeof var, "%s_%s_t%d", neon ? "neon" : "scalar", lit ? "lit3" : "unlit", th);
                snprintf(par, sizeof par,
                         "\"impl\":\"%s\",\"lights\":%d,\"threads\":%d,\"vertices\":%d,\"threshold_ns\":126",
                         neon ? "neon" : "scalar", lit ? 3 : 0, th, n);
                result_add("T7", var, "ns_per_vertex", "ns", &ns, par);
                if (cyc.n)
                    result_add("T7", var, "cycles_per_vertex_core", "cycles", &cyc, par);
                result_add("T7", var, "core_fraction_132k_30fps", "cores", &core, par);
            }
    free(out);
    input_free(&in);
}
