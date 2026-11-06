// ps2port trial, step 3, code-generation experiment V3 (with V1 + V2):
// guest loads and stores go straight to RDRAM (address & 32 MB mask) unless
// a cheap test says the address may be special. The test mirrors
// PS2Recomp's Ps2IsSpecialAddress (ps2_address.h): physical 0x1xxxxxxx
// (I/O, GS, VU memory, BIOS), 0x7xxxxxxx (scratchpad) and kseg2/3. Special
// addresses go to the runtime's Load / Store, as before. FAST_* accesses
// (addresses the recompiler proved to be RAM) stay direct.
#pragma once

#include <cstdint>
#include <cstring>

template <typename T>
static inline T ps2portLoad(const uint8_t *rdram, uint32_t addr)
{
    T value;
    std::memcpy(&value, rdram + (addr & PS2_RAM_MASK), sizeof(T));
    return value;
}

template <typename T>
static inline void ps2portStore(uint8_t *rdram, uint32_t addr, T value)
{
    std::memcpy(rdram + (addr & PS2_RAM_MASK), &value, sizeof(T));
}

static inline bool ps2portSlowAddr(uint32_t addr)
{
    const uint32_t phys = addr >= 0x80000000u ? (addr & 0x1FFFFFFFu) : addr;
    return (phys >> 28) == 1u || (phys >> 28) == 7u || addr >= 0xC0000000u;
}

#undef FAST_READ8
#undef FAST_READ16
#undef FAST_READ32
#undef FAST_READ64
#undef FAST_READ128
#undef FAST_WRITE8
#undef FAST_WRITE16
#undef FAST_WRITE32
#undef FAST_WRITE64
#undef FAST_WRITE128
#undef READ8
#undef READ16
#undef READ32
#undef READ64
#undef READ128
#undef WRITE8
#undef WRITE16
#undef WRITE32
#undef WRITE64
#undef WRITE128

#define FAST_READ8(addr) ps2portLoad<uint8_t>(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) ps2portLoad<uint16_t>(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) ps2portLoad<uint32_t>(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) ps2portLoad<uint64_t>(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) ps2portLoad<__m128i>(rdram, (uint32_t)(addr))
#define FAST_WRITE8(addr, val) ps2portStore<uint8_t>(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) ps2portStore<uint16_t>(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) ps2portStore<uint32_t>(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) ps2portStore<uint64_t>(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) ps2portStore<__m128i>(rdram, (uint32_t)(addr), (val))

#define PS2PORT_READ(T, L, addr) ([&]() -> T {                        \
    const uint32_t _a = (uint32_t)(addr);                             \
    return __builtin_expect(ps2portSlowAddr(_a), 0)                   \
               ? runtime->L(rdram, ctx, _a)                           \
               : ps2portLoad<T>(rdram, _a); }())
#define PS2PORT_WRITE(T, S, addr, val)                                \
    do                                                                \
    {                                                                 \
        const uint32_t _a = (uint32_t)(addr);                         \
        if (__builtin_expect(ps2portSlowAddr(_a), 0))                 \
            runtime->S(rdram, ctx, _a, (T)(val));                     \
        else                                                          \
            ps2portStore<T>(rdram, _a, (T)(val));                     \
    } while (0)

#define READ8(addr) PS2PORT_READ(uint8_t, Load8, addr)
#define READ16(addr) PS2PORT_READ(uint16_t, Load16, addr)
#define READ32(addr) PS2PORT_READ(uint32_t, Load32, addr)
#define READ64(addr) PS2PORT_READ(uint64_t, Load64, addr)
#define READ128(addr) PS2PORT_READ(__m128i, Load128, addr)
#define WRITE8(addr, val) PS2PORT_WRITE(uint8_t, Store8, addr, val)
#define WRITE16(addr, val) PS2PORT_WRITE(uint16_t, Store16, addr, val)
#define WRITE32(addr, val) PS2PORT_WRITE(uint32_t, Store32, addr, val)
#define WRITE64(addr, val) PS2PORT_WRITE(uint64_t, Store64, addr, val)
#define WRITE128(addr, val) PS2PORT_WRITE(__m128i, Store128, addr, val)
