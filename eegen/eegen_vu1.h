// eegen: what the code from vu1gen.py (translated VU1 microprogram) runs on, beyond eegen_vu0.h.
//
// There's no giving up here, unlike VU0: by the time a value a host float can't hold appears, a packet may have
// gone to the GIF. Operands aren't normalized in the vector operations, which would cost on every one; vu1play
// --mode check shows whether a frame ever sees that. The rare scalar operations do normalize, as the interpreter.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// MAX and MINI compare the raw bits as sign and magnitude and hand on an operand untouched. Exact for anything,
// and the game moves integers through them.
static inline eg_vi4 eg_vu1_ordered(eg_v4 v)
{
    const eg_vi4 b = (eg_vi4)v;
    return b ^ ((b >> 31) & 0x7FFFFFFF);
}

static inline eg_v4 eg_vu1_max(eg_v4 s, eg_v4 t)
{
    const eg_vi4 m = eg_vu1_ordered(s) > eg_vu1_ordered(t);
    return (eg_v4)((((eg_vi4)s) & m) | (((eg_vi4)t) & ~m));
}

static inline eg_v4 eg_vu1_min(eg_v4 s, eg_v4 t)
{
    const eg_vi4 m = eg_vu1_ordered(s) < eg_vu1_ordered(t);
    return (eg_v4)((((eg_vi4)s) & m) | (((eg_vi4)t) & ~m));
}

static inline float eg_vu1_norm(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t exponent = bits >> 23 & 0xFFu;
    if (exponent == 0u)
        bits &= 0x80000000u;
    else if (exponent == 0xFFu)
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    std::memcpy(&value, &bits, 4);
    return value;
}

static inline float eg_vu1_div(float num, float den)
{
    num = eg_vu1_norm(num);
    den = eg_vu1_norm(den);
    if (den == 0.0f)
        return std::signbit(num) != std::signbit(den) ? -3.4028234663852886e+38f : 3.4028234663852886e+38f;
    return eg_vu1_norm(num / den);
}

static inline float eg_vu1_sqrt(float value) { return eg_vu1_norm(std::sqrt(std::fabs(eg_vu1_norm(value)))); }

static inline float eg_vu1_ercpr(float value)
{
    value = eg_vu1_norm(value);
    return eg_vu1_norm(value != 0.0f ? 1.0f / value : value);
}

static inline float eg_vu1_esum(eg_v4 v)
{
    float sum = 0.0f;
    for (int c = 0; c < 4; ++c)
        sum += eg_vu1_norm(v[c]);
    return eg_vu1_norm(sum);
}

static inline float eg_vu1_erleng(eg_v4 v)
{
    const float x = eg_vu1_norm(v[0]), y = eg_vu1_norm(v[1]), z = eg_vu1_norm(v[2]);
    const float len = std::sqrt(x * x + y * y + z * z);
    return eg_vu1_norm(len != 0.0f ? 1.0f / len : len);
}

// x86 gives 0x80000000 on overflow of either sign; the interpreter and aarch64 saturate.
static inline eg_vi4 eg_vu1_ftoi(eg_v4 v)
{
    eg_vi4 r = __builtin_convertvector(v, eg_vi4);
#if !defined(__aarch64__)
    const eg_vi4 over = (v >= 2147483648.0f);
    r = (r & ~over) | (over & 0x7FFFFFFF);
#endif
    return r;
}

// Z (bits 0-3) and S (bits 4-7) by lane, x highest. U and O aren't produced: this game's FMEQ masks don't read them
// (vu1gen --report lists the reads; vu1play --mode check compares the outcome).
static inline uint32_t eg_vu1_mac(eg_v4 v, uint32_t dest)
{
    const eg_vi4 b = (eg_vi4)v;
    uint32_t mac = 0;
    for (int c = 0; c < 4; ++c)
    {
        const uint32_t lane = 8u >> c;
        if (dest & lane)
        {
            if ((static_cast<uint32_t>(b[c]) & 0x7FFFFFFFu) == 0u)
                mac |= lane;
            if (b[c] < 0)
                mac |= lane << 4;
        }
    }
    return mac;
}

// Compares as signed integers of the raw bits, as the interpreter: |fs.xyz| against |ft.w| with a denormal w as the largest denormal
static inline uint32_t eg_vu1_clip(eg_v4 fs, eg_v4 ft)
{
    const uint32_t wBits = static_cast<uint32_t>(((eg_vi4)ft)[3]);
    const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
    const eg_vi4 s = (eg_vi4)fs;
    uint32_t flags = 0;
    for (int c = 0; c < 3; ++c)
    {
        if (s[c] > limit)
            flags |= 1u << (c * 2);
        if ((s[c] ^ static_cast<int32_t>(0x80000000u)) > limit)
            flags |= 2u << (c * 2);
    }
    return flags;
}

static inline int32_t eg_vu1_lane16(const uint8_t *qword, int lane)
{
    uint32_t v;
    std::memcpy(&v, qword + lane * 4, 4);
    return static_cast<int16_t>(v & 0xFFFFu);
}

static inline void eg_vu1_store(uint8_t *qword, eg_v4 value, uint32_t dest)
{
    for (int c = 0; c < 4; ++c)
    {
        if (dest & (8u >> c))
        {
            const float f = value[c];
            std::memcpy(qword + c * 4, &f, 4);
        }
    }
}

static inline void eg_vu1_store_int(uint8_t *qword, int32_t value, uint32_t dest)
{
    const uint32_t v = static_cast<uint32_t>(static_cast<uint16_t>(value));
    for (int c = 0; c < 4; ++c)
    {
        if (dest & (8u >> c))
            std::memcpy(qword + c * 4, &v, 4);
    }
}
