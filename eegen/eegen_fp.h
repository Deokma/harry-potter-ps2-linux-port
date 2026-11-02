// eegen: the FPU modes and checks behind float groups (see eegen.py).
//
// eegen mode: flush-to-zero on (A53 FPCR.FZ, x86 MXCSR.FTZ). In a group's
// fast version, any operation that PS2Recomp's per-operation float model
// would not take straight from the host leaves a trace: an invalid,
// overflow or underflow flag (underflow is raised whenever a tiny result is
// flushed), a denormal-input flag (A53: IDC when FZ flushes an input; x86:
// DE, with DAZ off), or an inf / NaN in the values the group hands on
// (operands with exponent 255 are host inf / NaN and spread to the
// results). eg_fp_ok() tests that once per group.
//
// PS2Recomp mode: flush-to-zero off, the mode PS2Recomp's own code, the
// native stubs and a group's exact version run in. The runtime switches
// modes where control passes between eegen's functions and the rest
// (eg_mode_eegen / eg_mode_ps2recomp).
#pragma once

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef EG_FLOAT_STATS
inline uint64_t eg_group_stats[2]; // groups committed fast, groups run again exactly
#define EG_GROUP_FAST() (++eg_group_stats[0])
#define EG_GROUP_EXACT() (++eg_group_stats[1])
#else
#define EG_GROUP_FAST() ((void)0)
#define EG_GROUP_EXACT() ((void)0)
#endif

#define EG_FADD_FAST(a, b) ((a) + (b))
#define EG_FSUB_FAST(a, b) ((a) - (b))
#define EG_FMUL_FAST(a, b) ((a) * (b))

// a load in a group's fast version: RDRAM only, anything else runs the
// group's exact version instead (an MMIO read must happen once)
#define EG_LD_OR(exact, T, a)                    \
    ({                                           \
        const uint32_t a_ = (a);                 \
        if (EG_UNLIKELY(!eg_ram(a_)))            \
            goto exact;                          \
        T v_;                                    \
        std::memcpy(&v_, rdram + a_, sizeof(T)); \
        v_;                                      \
    })

#if defined(__aarch64__)

constexpr uint64_t EG_FP_BAD = 0x1u | 0x4u | 0x8u | 0x80u; // IOC OFC UFC IDC
constexpr uint64_t EG_FZ = 1ull << 24;

// k depends on every value the group hands on, so the read cannot move
// above the arithmetic it has to see
EG_INLINE uint64_t eg_fp_flags(float k)
{
    uint64_t v;
    asm volatile("mrs %0, fpsr" : "=r"(v) : "w"(k) : "memory");
    return v;
}

EG_INLINE void eg_fp_clear() { asm volatile("msr fpsr, xzr" ::: "memory"); }

EG_INLINE void eg_fz(bool on)
{
    uint64_t c;
    asm volatile("mrs %0, fpcr" : "=r"(c));
    c = on ? (c | EG_FZ) : (c & ~EG_FZ);
    asm volatile("msr fpcr, %0" ::"r"(c) : "memory");
}

EG_INLINE bool eg_fz_on()
{
    uint64_t c;
    asm volatile("mrs %0, fpcr" : "=r"(c));
    return (c & EG_FZ) != 0;
}

#else

constexpr uint32_t EG_FP_BAD = 0x1u | 0x2u | 0x8u | 0x10u; // IE DE OE UE
constexpr uint32_t EG_FZ = 0x8000u;                        // FTZ (DAZ stays off)

EG_INLINE uint32_t eg_fp_flags(float k)
{
    uint32_t v;
    asm volatile("stmxcsr %0" : "=m"(v) : "x"(k) : "memory");
    return v;
}

EG_INLINE void eg_fp_clear()
{
    uint32_t v;
    asm volatile("stmxcsr %0" : "=m"(v)::"memory");
    v &= ~0x3Fu;
    asm volatile("ldmxcsr %0" ::"m"(v) : "memory");
}

EG_INLINE void eg_fz(bool on)
{
    uint32_t v;
    asm volatile("stmxcsr %0" : "=m"(v)::"memory");
    v = (on ? (v | EG_FZ) : (v & ~EG_FZ)) & ~0x3Fu;
    asm volatile("ldmxcsr %0" ::"m"(v) : "memory");
}

EG_INLINE bool eg_fz_on()
{
    uint32_t v;
    asm volatile("stmxcsr %0" : "=m"(v)::"memory");
    return (v & EG_FZ) != 0;
}

#endif

EG_INLINE bool eg_fp_ok(float k)
{
    return k <= FLT_MAX && (eg_fp_flags(k) & EG_FP_BAD) == 0;
}

EG_INLINE void eg_mode_eegen()
{
    eg_fz(true);
    eg_fp_clear();
}

EG_INLINE void eg_mode_ps2recomp() { eg_fz(false); }

// a group's exact version: PS2Recomp's per-operation code in its own mode
EG_INLINE void eg_exact_enter() { eg_fz(false); }
EG_INLINE void eg_exact_leave() { eg_mode_eegen(); }
