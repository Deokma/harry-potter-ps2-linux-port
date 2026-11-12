// Mixer unit test (worker IOP, 2026-11-12): drives the SPU2 model in ps2xIOP's iop_memory.cpp alone through a fixed, pseudo-random
// script (sample DMA, key-ons and key-offs, pitch, ADSR, volumes, status reads, irregular clock steps) and writes everything the
// mixer produced, plus every status register read, to a file. Build it once against the old iop_memory.cpp and once against
// the new one (scripts/iop_mixer_test.sh) and compare the files: a refactor of the mixer must give identical bytes.
#include "emulator/core/iop_memory.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace ps2x::iop::detail
{
    extern uint64_t ps2portIopCycle;
}
size_t ps2portSpuPull(int16_t *out, size_t count);

using ps2x::iop::detail::IopMemory;

namespace
{
    uint32_t g_rng = 12345u;
    uint32_t rnd()
    {
        g_rng = g_rng * 1103515245u + 12345u;
        return g_rng >> 8u;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    std::FILE *out = std::fopen(argv[1], "wb");
    static IopMemory m;
    constexpr uint32_t kSpu = IopMemory::Spu2Base;
    // 64 KB of ADPCM blocks: groups of 64 with a loop start, a loop end, now and then a plain end
    std::vector<uint8_t> data(65536);
    for (uint32_t b = 0; b < 4096u; ++b)
    {
        uint8_t *blk = data.data() + b * 16u;
        blk[0] = static_cast<uint8_t>(((rnd() % 5u) << 4u) | (rnd() % 13u));
        blk[1] = (b % 64u == 0u) ? 4u : (b % 64u == 63u) ? 3u : (b % 997u == 500u) ? 1u : 0u;
        for (int k = 2; k < 16; ++k)
            blk[k] = static_cast<uint8_t>(rnd());
    }
    (void)m.writeRam(0x100000u, data.data(), data.size());
    for (int core = 0; core < 2; ++core)
    {
        const uint32_t base = kSpu + static_cast<uint32_t>(core) * 0x400u;
        m.write16(base + 0x1A8u, 0u);
        m.write16(base + 0x1AAu, static_cast<uint16_t>(0x2800u >> 1u));
    }
    m.write32(0x1F8010C0u, 0x100000u);
    m.write32(0x1F8010C4u, (1024u << 16u) | 16u);
    m.write32(0x1F8010C8u, 0x01000201u);
    // master volumes; core 1's input from core 0
    m.write16(kSpu + 0x760u, 0x3FFFu);
    m.write16(kSpu + 0x762u, 0x3000u);
    m.write16(kSpu + 0x788u, 0x3FFFu);
    m.write16(kSpu + 0x78Au, 0x3FFFu);
    m.write16(kSpu + 0x790u, 0x7FFFu);
    m.write16(kSpu + 0x792u, 0x6000u);
    auto setupVoice = [&](int core, int v)
    {
        const uint32_t base = kSpu + static_cast<uint32_t>(core) * 0x400u;
        const uint32_t vr = base + static_cast<uint32_t>(v) * 0x10u;
        const uint32_t start = 0x2800u + (rnd() % 64u) * 1024u;
        m.write16(vr + 0u, static_cast<uint16_t>(rnd() % 4u == 0u ? 0x8000u | (rnd() & 0x7Fu) : rnd() & 0x3FFFu));
        m.write16(vr + 2u, static_cast<uint16_t>(rnd() & 0x3FFFu));
        m.write16(vr + 4u, static_cast<uint16_t>(0x400u + rnd() % 0x2000u));
        m.write16(vr + 6u, static_cast<uint16_t>(rnd()));
        m.write16(vr + 8u, static_cast<uint16_t>(rnd()));
        const uint32_t ar = base + 0x1C0u + static_cast<uint32_t>(v) * 0xCu;
        m.write16(ar + 0u, static_cast<uint16_t>((start >> 1u) >> 16u));
        m.write16(ar + 2u, static_cast<uint16_t>(start >> 1u));
        m.write16(ar + 4u, static_cast<uint16_t>((start >> 1u) >> 16u));
        m.write16(ar + 6u, static_cast<uint16_t>(start >> 1u));
    };
    std::vector<int16_t> pulled(65536);
    uint64_t total = 0;
    for (int step = 0; step < 200000; ++step)
    {
        ps2x::iop::detail::ps2portIopCycle += 1000u + rnd() % 60000u;
        const uint32_t r = rnd() % 1000u;
        const int core = static_cast<int>(rnd() % 2u), v = static_cast<int>(rnd() % 24u);
        const uint32_t base = kSpu + static_cast<uint32_t>(core) * 0x400u;
        if (r < 6u)
        {
            setupVoice(core, v);
            m.write16(base + (v < 16 ? 0x1A0u : 0x1A2u), static_cast<uint16_t>(1u << (v % 16)));
        }
        else if (r < 9u)
            m.write16(base + (v < 16 ? 0x1A4u : 0x1A6u), static_cast<uint16_t>(1u << (v % 16)));
        else if (r < 12u)
            m.write16(base + static_cast<uint32_t>(v) * 0x10u + 4u, static_cast<uint16_t>(0x400u + rnd() % 0x2000u));
        else if (r < 14u)
        {
            // a DMA refill somewhere in the sample area (the starved-stream rule watches these)
            m.write16(base + 0x1A8u, 0u);
            m.write16(base + 0x1AAu, static_cast<uint16_t>((0x2800u + (rnd() % 64u) * 1024u) >> 1u));
            m.write32(core ? 0x1F801500u : 0x1F8010C0u, 0x100000u + (rnd() % 60u) * 1024u);
            m.write32(core ? 0x1F801504u : 0x1F8010C4u, (16u << 16u) | 16u);
            m.write32(core ? 0x1F801508u : 0x1F8010C8u, 0x01000201u);
        }
        if (r % 3u == 0u)
        {
            // what a sound driver reads: ENVX, NAX, ENDX
            uint16_t s[3];
            s[0] = m.read16(base + static_cast<uint32_t>(v) * 0x10u + 0xAu);
            s[1] = m.read16(base + 0x1C0u + static_cast<uint32_t>(v) * 0xCu + 10u);
            s[2] = m.read16(base + 0x340u);
            std::fwrite(s, sizeof s, 1, out);
        }
        m.ps2portSpuTick();
        const size_t got = ps2portSpuPull(pulled.data(), pulled.size() / 2u);
        std::fwrite(pulled.data(), sizeof(int16_t), got * 2u, out);
        total += got;
    }
    std::fclose(out);
    std::printf("frames %llu\n", static_cast<unsigned long long>(total));
    return 0;
}
