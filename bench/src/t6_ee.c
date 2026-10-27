/* T6: recompiled EE code on one core. Synthetic kernels (t6_kernels.inc)
   stand in for game code until Phase 3 supplies recompiled game functions.
   Reports guest instructions per host second, and host instructions /
   cycles per guest instruction when perf counters are available. */
#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#define RAM_BYTES (32u << 20)
#define SPR_BYTES (16u << 10)

typedef union {
    uint64_t d[2];
    uint32_t w[4];
} gpr128;

typedef struct {
    gpr128 gpr[32];
    float fpr[32];
    uint32_t fcr31;
    uint64_t icount;
    uint8_t *ram, *spr;
} ee_ctx;

#define SE32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))

static inline uint8_t *gptr(ee_ctx *c, uint32_t a)
{
    return (a >> 28) == 7 ? c->spr + (a & (SPR_BYTES - 1)) : c->ram + (a & (RAM_BYTES - 1));
}
static inline uint32_t LW(ee_ctx *c, uint32_t a)
{
    uint32_t v;
    memcpy(&v, gptr(c, a), 4);
    return v;
}
static inline void SW(ee_ctx *c, uint32_t a, uint32_t v)
{
    memcpy(gptr(c, a), &v, 4);
}
static inline float LWC1(ee_ctx *c, uint32_t a)
{
    float v;
    memcpy(&v, gptr(c, a), 4);
    return v;
}
static inline void SWC1(ee_ctx *c, uint32_t a, float v)
{
    memcpy(gptr(c, a), &v, 4);
}
static inline void LQ(ee_ctx *c, gpr128 *r, uint32_t a)
{
    memcpy(r, gptr(c, a & ~15u), 16);
}
static inline void SQ(ee_ctx *c, const gpr128 *r, uint32_t a)
{
    memcpy(gptr(c, a & ~15u), r, 16);
}

/* --- _ctx: registers in the context struct ---------------------------- */
#define KSUFFIX _ctx
#define REG(n) (c->gpr[n].d[0])
#define FR(n) (c->fpr[n])
#define QR(n) (c->gpr[n])
#define COUNT(n) ((void)0)
#define KDECL
#define KSYNC
#include "t6_kernels.inc"
#undef KSUFFIX
#undef COUNT

/* --- _cnt: as _ctx, counting executed guest instructions --------------- */
#define KSUFFIX _cnt
#define COUNT(n) (c->icount += (n))
#include "t6_kernels.inc"
#undef KSUFFIX
#undef REG
#undef FR
#undef QR
#undef COUNT
#undef KDECL
#undef KSYNC

/* --- _loc: registers as C locals, synced at entry and exit ------------- */
/* t0 / t1 are used both as 64-bit registers and by lq / sq: one guest
   register, one home (q8 / q9), the low dword aliased as r8 / r9. */
#define KSUFFIX _loc
#define REG(n) r##n
#define FR(n) f##n
#define QR(n) q##n
#define COUNT(n) ((void)0)
#define r8 q8.d[0]
#define r9 q9.d[0]
#define G(n) r##n = c->gpr[n].d[0]
#define F(n) f##n = c->fpr[n]
#define KDECL                                                                        \
    uint64_t G(4), G(5), G(6), G(10), G(11), G(12), G(13), G(14), G(15), G(16),      \
        G(17), G(18), G(24), G(25);                                                  \
    float F(0), F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8), F(9), F(10), F(11),  \
        F(12), F(13), F(20), F(21), F(22), F(23), F(24), F(25), F(26), F(27), F(28), \
        F(29), F(30), F(31);                                                         \
    gpr128 q8 = c->gpr[8], q9 = c->gpr[9];
#define WG(n) c->gpr[n].d[0] = r##n;
#define WF(n) c->fpr[n] = f##n;
#define KSYNC                                                                       \
    WG(4) WG(5) WG(6) WG(10) WG(11) WG(12) WG(13) WG(14) WG(15) WG(16) WG(17)       \
    WG(18) WG(24) WG(25)                                                            \
    WF(0) WF(1) WF(2) WF(3) WF(4) WF(5) WF(6) WF(7) WF(8) WF(9) WF(10) WF(11)       \
    WF(12) WF(13) WF(20) WF(21) WF(22) WF(23) WF(24) WF(25) WF(26) WF(27) WF(28)    \
    WF(29) WF(30) WF(31)                                                            \
    c->gpr[8] = q8;                                                                 \
    c->gpr[9] = q9;
#include "t6_kernels.inc"
#undef r8
#undef r9

/* ---- guest memory layout -------------------------------------------------- */

#define INT_BASE 0x00200000u
#define INT_NOBJ 4096u
#define FPU_BASE 0x00300000u
#define FPU_NOBJ 2048u
#define FPU_CONST 0x00380000u
#define LQ_SRC 0x00400000u
#define LQ_DST 0x00480000u
#define LQ_N32 8192u               /* 256 KiB per call */
#define MEM_DRAM_BASE 0x00800000u
#define MEM_DRAM_MASK ((4u << 20) - 8u)
#define MEM_SPR_BASE 0x70000000u
#define MEM_SPR_MASK (SPR_BYTES - 8u)
#define MEM_ITERS 65536u

enum { K_INT, K_FPU, K_LQSQ, K_MEM_DRAM, K_MEM_SPR, K_COUNT };
static const char *kname[K_COUNT] = { "int_branch", "fpu", "lq_sq", "mem_dram", "mem_spr" };
/* weights from the static instruction mix of the boot ELF (PHASE_1 A3) */
static const double kweight[K_COUNT] = { 0.73, 0.18, 0.05, 0.04, 0.0 };

static void ctx_init(ee_ctx *c)
{
    memset(c, 0, sizeof *c);
    c->ram = calloc(1, RAM_BYTES);
    c->spr = calloc(1, SPR_BYTES);
    if (!c->ram || !c->spr)
        abort();
}

static void ctx_free(ee_ctx *c)
{
    free(c->ram);
    free(c->spr);
}

static void data_init(ee_ctx *c, uint64_t seed)
{
    for (uint32_t i = 0; i < INT_NOBJ; i++) {
        uint32_t a = INT_BASE + i * 64;
        uint64_t r = rng_next(&seed);
        SW(c, a + 0, (r & 3) ? 1u : 0u);                          /* 75 % active */
        SW(c, a + 4, (uint32_t)(r >> 8) & 0xFFFF);                 /* x */
        SW(c, a + 8, (uint32_t)(r >> 24) & 0xFFFF);                /* y */
        SW(c, a + 12, (uint32_t)((int32_t)((r >> 40) & 0xFF) - 128)); /* vx */
        SW(c, a + 16, (uint32_t)((int32_t)((r >> 48) & 0xFF) - 128)); /* vy */
        SW(c, a + 20, (uint32_t)(r >> 56) % 60 + 1);               /* timer */
        SW(c, a + 24, 0);
        SW(c, a + 28, (uint32_t)r);
    }
    for (uint32_t i = 0; i < FPU_NOBJ; i++) {
        uint32_t a = FPU_BASE + i * 64;
        for (int k = 0; k < 6; k++) {
            uint64_t r = rng_next(&seed);
            float v = (float)((double)(r >> 11) / 9007199254740992.0 * 200.0 - 100.0);
            SWC1(c, a + 4 * k, k < 3 ? v : v * 0.1f);
        }
    }
    /* slightly scaled rotation about z, dt, R^2, R */
    const float kc[12] = { 0.9998f, -0.0175f, 0.0f, 0.0175f, 0.9998f, 0.0f, 0.0f, 0.0f, 1.0001f,
                           1.0f / 30.0f, 90.0f * 90.0f, 90.0f };
    for (int k = 0; k < 12; k++)
        SWC1(c, FPU_CONST + 4 * k, kc[k]);
    for (uint32_t i = 0; i < LQ_N32 * 32; i += 8) {
        uint64_t r = rng_next(&seed);
        memcpy(gptr(c, LQ_SRC + i), &r, 8);
    }
    for (uint32_t i = 0; i < (4u << 20); i += 8) {
        uint64_t r = rng_next(&seed);
        memcpy(gptr(c, MEM_DRAM_BASE + i), &r, 8);
    }
    for (uint32_t i = 0; i < SPR_BYTES; i += 8) {
        uint64_t r = rng_next(&seed);
        memcpy(c->spr + i, &r, 8);
    }
}

/* PS2 FPU: no denormals, round toward zero. The switches are noinline
   asm with memory clobbers so the compiler can't move guest-memory loads,
   stores, or the FP work between them, across a mode change. */
typedef struct {
    uint64_t saved;
} fpmode;

static __attribute__((noinline)) void fpmode_ps2(fpmode *m)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v) : : "memory");
    m->saved = v;
    v = (v & ~(3ull << 22)) | (3ull << 22) | (1ull << 24); /* RMode = RZ, FZ */
    __asm__ volatile("msr fpcr, %0" : : "r"(v) : "memory");
#elif defined(__x86_64__)
    unsigned v;
    __asm__ volatile("stmxcsr %0" : "=m"(v) : : "memory");
    m->saved = v;
    v = (v & ~0x6000u) | 0x6000u | 0x8040u;                /* RC = RZ, FTZ, DAZ */
    __asm__ volatile("ldmxcsr %0" : : "m"(v) : "memory");
#else
    m->saved = 0;
#endif
}

static __attribute__((noinline)) void fpmode_restore(const fpmode *m)
{
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr, %0" : : "r"(m->saved) : "memory");
#elif defined(__x86_64__)
    unsigned v = (unsigned)m->saved;
    __asm__ volatile("ldmxcsr %0" : : "m"(v) : "memory");
#else
    (void)m;
#endif
}

static void run_kernel(int k, int variant, ee_ctx *c)
{
    /* variant: 0 = ctx, 1 = loc, 2 = cnt */
    switch (k) {
    case K_INT:
        (variant == 0 ? k_int_ctx : variant == 1 ? k_int_loc : k_int_cnt)(c, INT_BASE, INT_NOBJ);
        break;
    case K_FPU:
        (variant == 0 ? k_fpu_ctx : variant == 1 ? k_fpu_loc : k_fpu_cnt)(c, FPU_BASE, FPU_NOBJ, FPU_CONST);
        break;
    case K_LQSQ:
        (variant == 0 ? k_lqsq_ctx : variant == 1 ? k_lqsq_loc : k_lqsq_cnt)(c, LQ_SRC, LQ_DST, LQ_N32);
        break;
    case K_MEM_DRAM:
        (variant == 0 ? k_mem_ctx : variant == 1 ? k_mem_loc : k_mem_cnt)(c, MEM_DRAM_BASE, MEM_DRAM_MASK, MEM_ITERS);
        break;
    case K_MEM_SPR:
        (variant == 0 ? k_mem_ctx : variant == 1 ? k_mem_loc : k_mem_cnt)(c, MEM_SPR_BASE, MEM_SPR_MASK, MEM_ITERS);
        break;
    }
}

/* guest memory a kernel touches, for snapshot / compare */
static void kernel_region(int k, ee_ctx *c, uint8_t **p, size_t *n)
{
    switch (k) {
    case K_INT:
        *p = gptr(c, INT_BASE), *n = INT_NOBJ * 64;
        break;
    case K_FPU:
        *p = gptr(c, FPU_BASE), *n = FPU_NOBJ * 64;
        break;
    case K_LQSQ:
        *p = gptr(c, LQ_DST), *n = LQ_N32 * 32;
        break;
    case K_MEM_DRAM:
        *p = gptr(c, MEM_DRAM_BASE), *n = 4u << 20;
        break;
    default:
        *p = c->spr, *n = SPR_BYTES;
        break;
    }
}

static uint64_t regs_hash(const ee_ctx *c)
{
    return fnv1a(c->gpr, sizeof c->gpr) ^ fnv1a(c->fpr, sizeof c->fpr) * 31;
}

/* Run `calls` calls of kernel k from a fresh state; returns final-state hash. */
static uint64_t run_calls(int k, int variant, ee_ctx *c, const uint8_t *snap, int calls)
{
    uint8_t *p;
    size_t n;
    kernel_region(k, c, &p, &n);
    memcpy(p, snap, n);
    memset(c->gpr, 0, sizeof c->gpr);
    memset(c->fpr, 0, sizeof c->fpr);
    c->fcr31 = 0;
    fpmode m = { 0 };
    if (k == K_FPU)
        fpmode_ps2(&m);
    for (int i = 0; i < calls; i++)
        run_kernel(k, variant, c);
    if (k == K_FPU)
        fpmode_restore(&m);
    return fnv1a(p, n) ^ regs_hash(c);
}

static int fpmode_selftest(void)
{
    fpmode m;
    volatile float one = 1.0f, three = 3.0f, tiny = 1e-38f, hundredth = 0.01f;
    volatile float q, d;
    fpmode_ps2(&m);
    q = one / three;
    d = tiny * hundredth;
    fpmode_restore(&m);
    uint32_t qb;
    float qv = q;
    memcpy(&qb, &qv, 4);
    int fails = 0;
    if (qb != 0x3EAAAAAAu) {
        printf("  FAIL T6 round-toward-zero not active: 1/3 = %08x\n", qb);
        fails++;
    }
    if (d != 0.0f) {
        printf("  FAIL T6 flush-to-zero not active: 1e-38 * 0.01 = %g\n", (double)d);
        fails++;
    }
    return fails;
}

int t6_selftest(void)
{
    int fails = fpmode_selftest();
    ee_ctx c;
    ctx_init(&c);
    data_init(&c, 0xEE5EED);
    for (int k = 0; k < K_COUNT; k++) {
        uint8_t *p;
        size_t n;
        kernel_region(k, &c, &p, &n);
        uint8_t *snap = malloc(n);
        memcpy(snap, p, n);
        uint64_t h[3];
        for (int v = 0; v < 3; v++)
            h[v] = run_calls(k, v, &c, snap, 3);
        if (h[0] != h[1] || h[0] != h[2]) {
            printf("  FAIL T6 %s: ctx/loc/cnt results differ (%016llx %016llx %016llx)\n", kname[k],
                   (unsigned long long)h[0], (unsigned long long)h[1], (unsigned long long)h[2]);
            fails++;
        }
        printf("  T6 %-10s checksum %016llx guest instr/call %llu (must match across builds)\n",
               kname[k], (unsigned long long)h[0], (unsigned long long)(c.icount / 3));
        c.icount = 0;
        memcpy(p, snap, n);
        free(snap);
    }
    ctx_free(&c);
    return fails;
}

/* ---- benchmark ------------------------------------------------------------ */

typedef struct {
    int k, variant, calls;
    ee_ctx *c;
    const uint8_t *snap;
    double secs;
    uint64_t perf_ins, perf_cyc, hash;
    int perf_ok;
    double mhz;
} t6_job;

static void t6_thread(void *arg, int index, int count)
{
    (void)index;
    (void)count;
    t6_job *j = arg;
    perf_ctr pc;
    j->perf_ok = perf_open(&pc) == 0;
    uint8_t *p;
    size_t n;
    kernel_region(j->k, j->c, &p, &n);
    memcpy(p, j->snap, n);
    memset(j->c->gpr, 0, sizeof j->c->gpr);
    memset(j->c->fpr, 0, sizeof j->c->fpr);
    j->c->fcr31 = 0;
    fpmode m = { 0 };
    if (j->k == K_FPU)
        fpmode_ps2(&m);
    if (j->perf_ok)
        perf_start(&pc);
    double t0 = now_s();
    for (int i = 0; i < j->calls; i++)
        run_kernel(j->k, j->variant, j->c);
    j->secs = now_s() - t0;
    if (j->perf_ok) {
        perf_stop(&pc, &j->perf_ins, &j->perf_cyc);
        perf_close(&pc);
    }
    if (j->k == K_FPU)
        fpmode_restore(&m);
    j->mhz = cpu_cur_freq_mhz(0);
    j->hash = fnv1a(p, n) ^ regs_hash(j->c);
}

void t6_run(void)
{
    printf("T6: recompiled-EE-style kernels, one core\n");
    int fails = fpmode_selftest();
    ee_ctx c;
    ctx_init(&c);
    data_init(&c, 0xEE5EED);
    double rate_med[2][K_COUNT] = { { 0 } };
    static const char *varname[2] = { "ctx", "loc" };
    for (int k = 0; k < K_COUNT; k++) {
        uint8_t *p;
        size_t n;
        kernel_region(k, &c, &p, &n);
        uint8_t *snap = malloc(n);
        memcpy(snap, p, n);
        for (int v = 0; v < 2; v++) {
            t6_job j = { k, v, 1, &c, snap, 0, 0, 0, 0, 0, 0 };
            run_threads(1, t6_thread, &j, 1);
            double target = g_opts.quick ? 0.02 : 0.3;
            j.calls = j.secs > 0 ? (int)(target / j.secs) + 1 : 1;
            /* exact guest instruction count for the same sequence of calls */
            c.icount = 0;
            uint64_t hcnt = run_calls(k, 2, &c, snap, j.calls);
            uint64_t guest = c.icount;
            c.icount = 0;
            samples rate = { { 0 }, 0 }, hipg = { { 0 }, 0 }, cpg = { { 0 }, 0 }, fcpg = { { 0 }, 0 };
            for (int r = 0; r < g_opts.reps; r++) {
                run_threads(1, t6_thread, &j, 1);
                if (j.hash != hcnt) {
                    printf("  FAIL T6 %s_%s: timed run differs from counted run\n", kname[k], varname[v]);
                    fails++;
                }
                double gps = (double)guest / j.secs;
                samples_add(&rate, gps / 1e6);
                if (j.perf_ok) {
                    samples_add(&hipg, (double)j.perf_ins / (double)guest);
                    samples_add(&cpg, (double)j.perf_cyc / (double)guest);
                }
                if (j.mhz > 0)
                    samples_add(&fcpg, j.mhz * 1e6 / gps);
            }
            rate_med[v][k] = samples_median(&rate);
            char var[64], par[200];
            snprintf(var, sizeof var, "%s_%s", kname[k], varname[v]);
            snprintf(par, sizeof par, "\"kernel\":\"%s\",\"regs\":\"%s\",\"calls\":%d,\"guest_instr\":%llu",
                     kname[k], varname[v], j.calls, (unsigned long long)guest);
            result_add("T6", var, "mguest_instr_per_s", "M/s", &rate, par);
            if (hipg.n) {
                result_add("T6", var, "host_instr_per_guest", "ratio", &hipg, par);
                result_add("T6", var, "host_cycles_per_guest", "cycles", &cpg, par);
            }
            if (fcpg.n)
                result_add("T6", var, "cycles_per_guest_from_freq", "cycles", &fcpg, par);
        }
        free(snap);
    }
    /* Part B composite: weighted by the static mix, against 3.3-5.9 M guest
       instructions per gameplay frame at 30 fps (PHASE_1 C3.1). */
    for (int v = 0; v < 2; v++) {
        double inv = 0;
        for (int k = 0; k < K_COUNT; k++)
            if (kweight[k] > 0 && rate_med[v][k] > 0)
                inv += kweight[k] / rate_med[v][k];
        double mix = inv > 0 ? 1.0 / inv : NAN;
        char var[32], par[160];
        snprintf(var, sizeof var, "partb_mix_%s", varname[v]);
        snprintf(par, sizeof par, "\"weights\":\"int .73 fpu .18 lqsq .05 mem .04\",\"threshold_mps\":295");
        result_add1("T6", var, "mguest_instr_per_s", "M/s", mix, par);
        result_add1("T6", var, "core_fraction_5.9M_30fps", "cores", 5.9e6 * 30 / (mix * 1e6), par);
    }
    note_num("t6.selftest_failures", fails);
    ctx_free(&c);
}

/* ---- background load ------------------------------------------------------ */

static atomic_int load_stop_flag;
static pthread_t load_threads[16];
static int load_nthreads;

static void *load_thread(void *arg)
{
    int cpu = (int)(intptr_t)arg;
    pin_to_cpu(cpu % cpu_count());
    ee_ctx c;
    ctx_init(&c);
    data_init(&c, 0x10AD + (uint64_t)cpu);
    while (!atomic_load(&load_stop_flag))
        k_int_loc(&c, INT_BASE, INT_NOBJ);
    ctx_free(&c);
    return NULL;
}

void load_start(int threads)
{
    atomic_store(&load_stop_flag, 0);
    load_nthreads = threads > 16 ? 16 : threads;
    for (int i = 0; i < load_nthreads; i++)
        pthread_create(&load_threads[i], NULL, load_thread, (void *)(intptr_t)i);
}

void load_stop(void)
{
    atomic_store(&load_stop_flag, 1);
    for (int i = 0; i < load_nthreads; i++)
        pthread_join(load_threads[i], NULL);
    load_nthreads = 0;
}
