// eegen: what the code from vu0gen.py (translated VU0 microprograms) runs on.
//
// A translated program runs in flush-to-zero and round-toward-zero mode. With
// those modes the host gives what PS2Recomp's VU interpreter computes for
// finite values:
// - denormal operands count as signed zeros, denormal results become signed
//   zeros (FPCR.FZ; on x86 MXCSR.DAZ + FTZ);
// - ADD, SUB, MUL and DIV truncate;
// - MADD / MSUB truncate twice, after the product and after the sum. The
//   interpreter also works out the exact sum (in long double, on aarch64
//   software quad precision) but uses it only to decide whether the result is
//   zero, too large or too small. Here a product below the smallest normal
//   float is already zero before the sum; there it is a denormal that can move
//   the sum by one unit (HARNESS_VU0=4 shows whether a frame ever sees that).
//
// A program gives up, changing nothing, and leaves the call to the interpreter
// when PS2 values a host float can't hold are around: an operand with exponent
// 255, a host overflow or invalid operation, a division by zero. Results are
// kept in EgVu0Out and written to the context and VU data memory only after
// the host's flags have been read.
#pragma once

#include <cstdint>
#include <cstring>
#if !defined(__aarch64__)
#include <xmmintrin.h>
#endif

typedef float eg_v4 __attribute__((vector_size(16)));
typedef int32_t eg_vi4 __attribute__((vector_size(16)));

static const eg_v4 eg_vf0 = {0.0f, 0.0f, 0.0f, 1.0f};

struct EgVu0Out
{
    eg_v4 vf[32];
    eg_v4 acc;
    eg_v4 sq[16];
    uint32_t sqa[16];
    float q;
    int16_t vi[16];
};
typedef bool (*EgVu0Fn)(R5900Context *ctx, uint8_t *data);

#define EG_VU_BC(v, c) __builtin_shuffle((v), eg_vi4{(c), (c), (c), (c)})
#define EG_VU_ALL(f) (eg_v4{(f), (f), (f), (f)})
#define EG_VU_YZX(v) __builtin_shuffle((v), eg_vi4{1, 2, 0, 3})
#define EG_VU_ZXY(v) __builtin_shuffle((v), eg_vi4{2, 0, 1, 3})
#define EG_VU_MR32(v) __builtin_shuffle((v), eg_vi4{1, 2, 3, 0})
#define EG_VU_MERGE(a, old, x, y, z, w)                     \
    ((eg_v4)((((eg_vi4)(a)) & (eg_vi4{(x), (y), (z), (w)})) | \
             (((eg_vi4)(old)) & ~(eg_vi4{(x), (y), (z), (w)}))))
// all ones in the lanes that are selected and hold exponent 255
#define EG_VU_EXP255(v, x, y, z, w)                                                            \
    ((((((eg_vi4)(v)) & 0x7F800000) == 0x7F800000)) & (eg_vi4{(x), (y), (z), (w)}))

static inline bool eg_vu_any(eg_vi4 v) { return (v[0] | v[1] | v[2] | v[3]) != 0; }

static inline bool eg_vu_exp255(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return (b & 0x7F800000u) == 0x7F800000u;
}

// MAX / MINI hand on a normalized operand: a denormal comes out as a signed
// zero (exponent 255 was tested on the way in)
static inline eg_v4 eg_vu_flush(eg_v4 v)
{
    const eg_vi4 denormal = (((eg_vi4)v) & 0x7F800000) == 0;
    return (eg_v4)(((eg_vi4)v) & (~denormal | (int32_t)0x80000000));
}

static inline eg_v4 eg_vu_max(eg_v4 s, eg_v4 t)
{
    s = eg_vu_flush(s);
    t = eg_vu_flush(t);
    const eg_vi4 m = s > t;
    return (eg_v4)((((eg_vi4)s) & m) | (((eg_vi4)t) & ~m));
}

static inline eg_v4 eg_vu_min(eg_v4 s, eg_v4 t)
{
    s = eg_vu_flush(s);
    t = eg_vu_flush(t);
    const eg_vi4 m = s < t;
    return (eg_v4)((((eg_vi4)s) & m) | (((eg_vi4)t) & ~m));
}

#if defined(__aarch64__)

static inline uint64_t eg_vu_enter()
{
    uint64_t c;
    asm volatile("mrs %0, fpcr" : "=r"(c));
    const uint64_t n = c | (3ull << 22) | (1ull << 24); // RMode = toward zero, FZ
    asm volatile("msr fpcr, %0" : : "r"(n));
    asm volatile("msr fpsr, xzr" : : : "memory");
    return c;
}

static inline bool eg_vu_bad()
{
    uint64_t s;
    asm volatile("mrs %0, fpsr" : "=r"(s) : : "memory");
    return (s & (0x1u | 0x4u)) != 0; // IOC, OFC
}

static inline void eg_vu_leave(uint64_t c) { asm volatile("msr fpcr, %0" : : "r"(c)); }

#else

static inline uint64_t eg_vu_enter()
{
    const unsigned c = _mm_getcsr();
    _mm_setcsr((c & ~0x3Fu) | 0x6000u | 0x8000u | 0x40u); // toward zero, FTZ, DAZ, flags clear
    return c;
}

static inline bool eg_vu_bad() { return (_mm_getcsr() & (0x1u | 0x8u)) != 0; } // invalid, overflow

static inline void eg_vu_leave(uint64_t c) { _mm_setcsr(static_cast<unsigned>(c)); }

#endif
