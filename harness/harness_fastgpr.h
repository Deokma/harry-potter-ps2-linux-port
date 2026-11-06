// ps2port trial, step 3, code-generation experiment V1: scalar access to the
// low 64 bits of each guest GPR instead of PS2Recomp's __m128i lane
// extract / insert (which sse2neon turns into 128-bit loads, inserts and
// stores on aarch64). Same semantics as ps2_runtime_macros.h: 32-bit results
// are sign- (or zero-) extended into the low doubleword and the upper
// doubleword is left alone; 128-bit access (GPR_VEC) is unchanged.
// make_harness.py --variant v1 includes this after the generated file's own
// includes.
#pragma once

#include <cstdint>
#include <cstring>

static inline uint64_t ps2portLo64(const R5900Context *ctx, int reg)
{
    uint64_t value;
    std::memcpy(&value, &ctx->r[reg], sizeof(value));
    return value;
}

static inline void ps2portSetLo64(R5900Context *ctx, int reg, uint64_t value)
{
    std::memcpy(&ctx->r[reg], &value, sizeof(value));
}

#undef GPR_U32
#undef GPR_S32
#undef GPR_U64
#undef GPR_S64
#undef SET_GPR_U32
#undef SET_GPR_ZE32
#undef SET_GPR_S32
#undef SET_GPR_U64
#undef SET_GPR_S64

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx) == 0 ? 0U : static_cast<uint32_t>(ps2portLo64((ctx_ptr), (reg_idx))))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx) == 0 ? 0 : static_cast<int32_t>(ps2portLo64((ctx_ptr), (reg_idx))))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx) == 0 ? 0ULL : ps2portLo64((ctx_ptr), (reg_idx)))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx) == 0 ? 0LL : static_cast<int64_t>(ps2portLo64((ctx_ptr), (reg_idx))))

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                                       \
    do                                                                                           \
    {                                                                                            \
        if ((reg_idx) != 0)                                                                      \
            ps2portSetLo64((ctx_ptr), (reg_idx), static_cast<uint64_t>(static_cast<int64_t>(    \
                                                     static_cast<int32_t>(val))));              \
    } while (0)
#define SET_GPR_S32(ctx_ptr, reg_idx, val) SET_GPR_U32(ctx_ptr, reg_idx, val)
#define SET_GPR_ZE32(ctx_ptr, reg_idx, val)                                                      \
    do                                                                                           \
    {                                                                                            \
        if ((reg_idx) != 0)                                                                      \
            ps2portSetLo64((ctx_ptr), (reg_idx), static_cast<uint64_t>(static_cast<uint32_t>(val))); \
    } while (0)
#define SET_GPR_U64(ctx_ptr, reg_idx, val)                                                       \
    do                                                                                           \
    {                                                                                            \
        if ((reg_idx) != 0)                                                                      \
            ps2portSetLo64((ctx_ptr), (reg_idx), static_cast<uint64_t>(val));                    \
    } while (0)
#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)
