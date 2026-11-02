// eegen: helpers the generated code uses (see eegen.py).
//
// Built against PS2Recomp's runtime (the fork we port under GPL-3.0): its
// R5900Context, PS2Runtime (MMIO Load* / Store*, syscalls, dispatch) and its
// PS2 float model in ps2_runtime_macros.h. The float fast paths below give
// the same results as PS2Recomp's ps2_fadd / ps2_fsub / ps2_fmul (they keep
// its exact model and only handle the zero case inline instead of calling
// the out-of-line path); see docs/PROVENANCE.md.
#pragma once

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#define EG_INLINE inline __attribute__((always_inline))
#define EG_NOINLINE __attribute__((noinline))
#define EG_LIKELY(x) __builtin_expect(!!(x), 1)
#define EG_UNLIKELY(x) __builtin_expect(!!(x), 0)

#ifndef HARNESS_COUNT_K
#define HARNESS_COUNT_K(k) ((void)0)
#endif

// --- guest registers in the context ---------------------------------------
// The generated code keeps the low 64 bits of the GPRs and the FPU registers
// in locals; the context is written back before anything that can read it
// (calls, syscalls, returns) and read again after. The upper 64 bits of the
// GPRs (only lq / sq / movn / movz touch them here) always live in the
// context.

EG_INLINE uint64_t eg_get(const R5900Context *ctx, int n)
{
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const uint8_t *>(&ctx->r[n]), 8);
    return v;
}

EG_INLINE void eg_put(R5900Context *ctx, int n, uint64_t v)
{
    std::memcpy(reinterpret_cast<uint8_t *>(&ctx->r[n]), &v, 8);
}

EG_INLINE uint64_t eg_get_hi(const R5900Context *ctx, int n)
{
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const uint8_t *>(&ctx->r[n]) + 8, 8);
    return v;
}

EG_INLINE void eg_put_hi(R5900Context *ctx, int n, uint64_t v)
{
    std::memcpy(reinterpret_cast<uint8_t *>(&ctx->r[n]) + 8, &v, 8);
}

// --- guest memory ------------------------------------------------------------
// RDRAM is the first 32 MB of the address space: one test (a single
// tst + branch on the A53) for the common case. The runtime allocates RDRAM
// with 16 bytes of slack after it, so an unaligned access at the very end
// stays inside the allocation. Anything else takes the out-of-line path:
// RAM mirrors (kseg0/1, uncached) go to RDRAM, I/O, scratchpad and kseg2/3
// to the runtime's Load* / Store*, as in PS2Recomp's READ* / WRITE* macros.

EG_INLINE bool eg_ram(uint32_t a) { return (a & 0xFE000000u) == 0u; }
EG_INLINE bool eg_ram16(uint32_t a) { return (a & 0xFE00000Fu) == 0u; } // aligned 128-bit

EG_INLINE bool eg_special(uint32_t a)
{
    const uint32_t phys = a >= 0x80000000u ? (a & 0x1FFFFFFFu) : a;
    return (phys >> 28) == 1u || (phys >> 28) == 7u || a >= 0xC0000000u;
}

template <size_t N> struct EgBits;
template <> struct EgBits<1> { using type = uint8_t; };
template <> struct EgBits<2> { using type = uint16_t; };
template <> struct EgBits<4> { using type = uint32_t; };
template <> struct EgBits<8> { using type = uint64_t; };

// T may be float (lwc1 / swc1 load straight into an FP register)
template <typename T>
EG_NOINLINE T eg_ld_slow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a)
{
    using U = typename EgBits<sizeof(T)>::type;
    U u;
    if (eg_special(a))
    {
        if constexpr (sizeof(T) == 1)
            u = rt->Load8(rdram, ctx, a);
        else if constexpr (sizeof(T) == 2)
            u = rt->Load16(rdram, ctx, a);
        else if constexpr (sizeof(T) == 4)
            u = rt->Load32(rdram, ctx, a);
        else
            u = rt->Load64(rdram, ctx, a);
    }
    else
        std::memcpy(&u, rdram + (a & PS2_RAM_MASK), sizeof(U));
    T v;
    std::memcpy(&v, &u, sizeof(T));
    return v;
}

template <typename T>
EG_NOINLINE void eg_st_slow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a, T v)
{
    using U = typename EgBits<sizeof(T)>::type;
    U u;
    std::memcpy(&u, &v, sizeof(U));
    if (eg_special(a))
    {
        if constexpr (sizeof(T) == 1)
            rt->Store8(rdram, ctx, a, u);
        else if constexpr (sizeof(T) == 2)
            rt->Store16(rdram, ctx, a, u);
        else if constexpr (sizeof(T) == 4)
            rt->Store32(rdram, ctx, a, u);
        else
            rt->Store64(rdram, ctx, a, u);
        return;
    }
    std::memcpy(rdram + (a & PS2_RAM_MASK), &u, sizeof(U));
}

template <typename T>
EG_INLINE T eg_ld(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a)
{
    if (EG_LIKELY(eg_ram(a)))
    {
        T v;
        std::memcpy(&v, rdram + a, sizeof(T));
        return v;
    }
    return eg_ld_slow<T>(rdram, ctx, rt, a);
}

template <typename T>
EG_INLINE void eg_st(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a, T v)
{
    if (EG_LIKELY(eg_ram(a)))
        std::memcpy(rdram + a, &v, sizeof(T));
    else
        eg_st_slow<T>(rdram, ctx, rt, a, v);
}

// 128-bit: the low half to a local, the high half to / from the context
EG_NOINLINE inline uint64_t eg_lq_slow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a, int n)
{
    __m128i v;
    if (eg_special(a))
        v = rt->Load128(rdram, ctx, a);
    else
        std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 16);
    uint64_t q[2];
    std::memcpy(q, &v, 16);
    if (n != 0)
        eg_put_hi(ctx, n, q[1]);
    return q[0];
}

EG_INLINE uint64_t eg_lq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a, int n)
{
    if (EG_LIKELY(eg_ram16(a)))
    {
        uint64_t q[2];
        std::memcpy(q, rdram + a, 16);
        if (n != 0)
            eg_put_hi(ctx, n, q[1]);
        return q[0];
    }
    return eg_lq_slow(rdram, ctx, rt, a, n);
}

EG_NOINLINE inline void eg_sq_slow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a, uint64_t lo, uint64_t hi)
{
    const uint64_t q[2] = {lo, hi};
    __m128i v;
    std::memcpy(&v, q, 16);
    if (eg_special(a))
        rt->Store128(rdram, ctx, a, v);
    else
        std::memcpy(rdram + (a & PS2_RAM_MASK), &v, 16);
}

EG_INLINE void eg_sq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a, uint64_t lo, uint64_t hi)
{
    if (EG_LIKELY(eg_ram16(a)))
    {
        const uint64_t q[2] = {lo, hi};
        std::memcpy(rdram + a, q, 16);
    }
    else
        eg_sq_slow(rdram, ctx, rt, a, lo, hi);
}

EG_INLINE __m128 eg_lqc2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a)
{
    __m128i v;
    if (EG_LIKELY(eg_ram16(a)))
        std::memcpy(&v, rdram + a, 16);
    else if (eg_special(a))
        v = rt->Load128(rdram, ctx, a);
    else
        std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 16);
    return _mm_castsi128_ps(v);
}

EG_INLINE void eg_sqc2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t a, __m128 f)
{
    const __m128i v = _mm_castps_si128(f);
    if (EG_LIKELY(eg_ram16(a)))
        std::memcpy(rdram + a, &v, 16);
    else if (eg_special(a))
        rt->Store128(rdram, ctx, a, v);
    else
        std::memcpy(rdram + (a & PS2_RAM_MASK), &v, 16);
}

// --- PS2 floats ----------------------------------------------------------------
// PS2Recomp's model (ps2_runtime_macros.h): the host result stands when it is
// a normal number, or when it is zero and neither operand is special;
// everything else goes to its out-of-line double-precision path.

EG_INLINE bool eg_fnormal(float r)
{
    return (std::bit_cast<uint32_t>(r) * 2u + 0x01000000u) >= 0x02000000u;
}

EG_INLINE bool eg_fzero_ok(float a, float b, float r)
{
    return (std::bit_cast<uint32_t>(r) << 1) == 0u && !ps2_fspecial(a) && !ps2_fspecial(b);
}

#ifdef EG_FLOAT_STATS
// [0] calls, [1] an operand with exponent 255, [2] an operand with exponent 0
// and a mantissa, [3] result exponent 255, [4] result exponent 0 (non-zero),
// [5] zero result with a special operand
inline uint64_t eg_float_stats[6];
EG_NOINLINE inline void eg_float_stat(float a, float b, float r)
{
    auto e = [](float x) { return (std::bit_cast<uint32_t>(x) >> 23) & 0xFFu; };
    auto m = [](float x) { return std::bit_cast<uint32_t>(x) & 0x7FFFFFu; };
    ++eg_float_stats[0];
    eg_float_stats[1] += e(a) == 255 || e(b) == 255;
    eg_float_stats[2] += (e(a) == 0 && m(a)) || (e(b) == 0 && m(b));
    eg_float_stats[3] += e(r) == 255;
    eg_float_stats[4] += e(r) == 0 && m(r);
    eg_float_stats[5] += (std::bit_cast<uint32_t>(r) << 1) == 0u;
}
#define EG_FSTAT(a, b, r) eg_float_stat(a, b, r)
#else
#define EG_FSTAT(a, b, r) ((void)0)
#endif

#ifdef EG_NO_FLOAT_CHECK
// timing experiment only (wrong results for PS2-special values): plain host
// arithmetic, to measure what the per-operation checks cost
EG_INLINE float eg_fadd(float a, float b) { return a + b; }
EG_INLINE float eg_fsub(float a, float b) { return a - b; }
EG_INLINE float eg_fmul(float a, float b) { return a * b; }
#else
EG_INLINE float eg_fadd(float a, float b)
{
    const float r = a + b;
    if (EG_LIKELY(eg_fnormal(r) || eg_fzero_ok(a, b, r)))
        return r;
    EG_FSTAT(a, b, r);
    return ps2_fadd_slow(a, b);
}

EG_INLINE float eg_fsub(float a, float b)
{
    const float r = a - b;
    if (EG_LIKELY(eg_fnormal(r) || eg_fzero_ok(a, b, r)))
        return r;
    EG_FSTAT(a, b, r);
    return ps2_fsub_slow(a, b);
}

EG_INLINE float eg_fmul(float a, float b)
{
    const float r = a * b;
    if (EG_LIKELY(eg_fnormal(r) || eg_fzero_ok(a, b, r)))
        return r;
    EG_FSTAT(a, b, r);
    return ps2_fmul_slow(a, b);
}

#endif

EG_INLINE float eg_fbits(uint32_t v) { return std::bit_cast<float>(v); }
EG_INLINE uint32_t eg_bits(float f) { return std::bit_cast<uint32_t>(f); }

// CVT.W.S as the hardware does it (truncation, saturating beyond 2^31);
// PS2Recomp rounds to nearest instead (eegen --ps2recomp-quirks)
EG_INLINE uint32_t eg_cvt_w_s(float f)
{
    const uint32_t b = std::bit_cast<uint32_t>(f);
    if ((b & 0x7F800000u) <= 0x4E800000u)
        return static_cast<uint32_t>(static_cast<int32_t>(f));
    return (b & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
}

// a function the generated code makes known to the runtime
struct EgFunction
{
    uint32_t address;
    void (*function)(uint8_t *, R5900Context *, PS2Runtime *);
};
extern const EgFunction eg_functions[];
extern const unsigned eg_function_count;

#define EG_LD(T, a) eg_ld<T>(rdram, ctx, runtime, (a))
#define EG_ST(T, a, v) eg_st<T>(rdram, ctx, runtime, (a), static_cast<T>(v))
