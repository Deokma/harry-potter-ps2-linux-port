// What makes PS2Recomp's runtime run our code: eegen's functions over PS2Recomp's in the table, and translated VU0 and VU1
// programs in place of the interpreter (weak hooks in ps2_runtime.cpp; see docs/PROVENANCE.md).
//
// The translations were made from one snapshot of micro memory. A program runs translated only while micro memory equals that
// snapshot, tested when the runtime says the code has changed; otherwise the interpreter takes it, and registers move across.
// PS2PORT_NATIVE_STATS=1 reports how calls went at exit. PS2PORT_NO_EG, PS2PORT_NO_VU0 and PS2PORT_NO_VU1 turn each part off, to find
// which one a difference comes from. PS2PORT_EG_FROM and PS2PORT_EG_TO (hex) limit which functions enter eegen's code through the table;
// eegen's functions still call each other directly.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include "eegen_rt.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern const EgFunction eg_functions[];
uint32_t eg_resume_pc = 0; // eegen_rt.h: set by an EG_xxx_r entry for the function it calls (EE thread only)

#include "eegen_vu0.h"
#include "vu0_programs.inc"

// the snapshots: g_vu0Image, g_vu1Image
#include "micro_images.inc"
#include "vu1_reached.inc"

#include "../vu1/vu1_native.cpp"

namespace
{
    struct Stats
    {
        uint64_t vu0Native = 0, vu0GaveUp = 0, vu0Other = 0, vu1Native = 0, vu1Other = 0;
        ~Stats()
        {
            if (std::getenv("PS2PORT_NATIVE_STATS"))
                std::fprintf(stderr, "[native] VU0 translated %llu, gave up %llu, interpreter %llu; VU1 translated %llu, interpreter %llu\n", (unsigned long long)vu0Native,
                             (unsigned long long)vu0GaveUp, (unsigned long long)vu0Other, (unsigned long long)vu1Native, (unsigned long long)vu1Other);
        }
    } g_stats;

    // Pairs a program can't reach don't matter: the last few of VU1's hold what was there before the game's upload.
    bool sameCode(const char *unit, const uint8_t *code, const uint8_t *image, const uint8_t *reached, size_t size)
    {
        uint32_t differ = 0, first = 0xFFFFFFFFu, ignored = 0;
        for (size_t i = 0; i < size; i += 8)
            if (std::memcmp(code + i, image + i, 8) != 0)
            {
                if (reached && !reached[i / 8])
                {
                    ++ignored;
                    continue;
                }
                if (first == 0xFFFFFFFFu)
                    first = static_cast<uint32_t>(i / 8);
                ++differ;
            }
        if (differ == 0)
            std::fprintf(stderr, "[native] %s micro memory equals the snapshot (%u unreached pairs differ)\n", unit, ignored);
        else
            std::fprintf(stderr, "[native] %s micro memory differs from the snapshot in %u of %zu pairs, first %#x\n", unit, differ, size / 8, first);
        return differ == 0;
    }

    EgVu1State g_vu1;
    bool g_vu1Ours = false;
    PS2Memory *g_memory = nullptr;

    void toNative(const VU1State &from)
    {
        std::memcpy(g_vu1.vf, from.vf, sizeof(g_vu1.vf));
        std::memcpy(g_vu1.acc, from.acc, sizeof(g_vu1.acc));
        for (int r = 0; r < 16; ++r)
            g_vu1.vi[r] = static_cast<uint16_t>(from.vi[r]);
        g_vu1.q = from.q;
        g_vu1.p = from.p;
        g_vu1.i = from.i;
        g_vu1.clip = from.clip;
        g_vu1.mac = from.mac;
    }

    void toInterpreter(VU1State &to)
    {
        std::memcpy(to.vf, g_vu1.vf, sizeof(g_vu1.vf));
        std::memcpy(to.acc, g_vu1.acc, sizeof(g_vu1.acc));
        for (int r = 1; r < 16; ++r)
            to.vi[r] = static_cast<int16_t>(g_vu1.vi[r]);
        to.q = g_vu1.q;
        to.p = g_vu1.p;
        to.i = g_vu1.i;
        to.clip = g_vu1.clip;
    }
}

void ps2portNativeCounts(uint64_t out[5])
{
    out[0] = g_stats.vu0Native;
    out[1] = g_stats.vu0GaveUp;
    out[2] = g_stats.vu0Other;
    out[3] = g_stats.vu1Native;
    out[4] = g_stats.vu1Other;
}

void ps2portInstallNative()
{
    if (std::getenv("PS2PORT_NO_EG"))
    {
        std::fprintf(stderr, "[native] eegen's functions left out\n");
        return;
    }
    const char *from = std::getenv("PS2PORT_EG_FROM"), *to = std::getenv("PS2PORT_EG_TO");
    const uint32_t lowest = from ? static_cast<uint32_t>(std::strtoul(from, nullptr, 16)) : 0u;
    const uint32_t highest = to ? static_cast<uint32_t>(std::strtoul(to, nullptr, 16)) : 0xFFFFFFFFu;
    unsigned count = 0;
    for (const EgFunction *f = eg_functions; f->function; ++f)
    {
        if (f->address < lowest || f->address > highest)
            continue;
        g_ps2RecompiledFunctionTable[(f->address - g_ps2RecompiledFunctionTableBase) >> 2] = f->function;
        ++count;
    }
    std::fprintf(stderr, "[native] %u eegen functions in the table\n", count);
    // Resumes at loop heads, and at call and syscall return points (runner/gen.sh), go to eegen's own entries, not PS2Recomp's
    // re-entry code (PS2PORT_NO_EG_RESUME=1 keeps PS2Recomp's)
    if (std::getenv("PS2PORT_NO_EG_RESUME"))
        return;
    unsigned resumes = 0;
    for (const EgFunction *f = eg_resumes; f->function; ++f)
    {
        if (f->address < lowest || f->address > highest)
            continue;
        g_ps2RecompiledFunctionTable[(f->address - g_ps2RecompiledFunctionTableBase) >> 2] = f->function;
        ++resumes;
    }
    std::fprintf(stderr, "[native] %u loop heads and return points resume in eegen's code\n", resumes);
}

bool ps2portVu0Native(PS2Memory &memory, R5900Context *ctx, uint32_t startPC)
{
    static const bool off = std::getenv("PS2PORT_NO_VU0") != nullptr;
    if (off)
        return false;
    static uint64_t generation = ~0ull;
    static bool same = false;
    if (memory.getVU0CodeGeneration() != generation)
    {
        generation = memory.getVU0CodeGeneration();
        same = sameCode("VU0", memory.getVU0Code(), g_vu0Image, nullptr, sizeof(g_vu0Image));
    }
    if (!same)
    {
        ++g_stats.vu0Other;
        return false;
    }
    const EgVu0Fn fn = eg_vu0_lookup(startPC / 8u);
    if (!fn)
    {
        ++g_stats.vu0Other;
        return false;
    }
    if (!fn(ctx, memory.getVU0Data()))
    {
        ++g_stats.vu0GaveUp;
        return false;
    }
    ++g_stats.vu0Native;
    return true;
}

bool ps2portVu1Native(PS2Memory &memory, VU1State &interpreter, uint32_t startPC, uint32_t top, uint32_t itop)
{
    static const bool off = std::getenv("PS2PORT_NO_VU1") != nullptr;
    static uint64_t generation = ~0ull;
    static bool same = false;
    if (!off && memory.getVU1CodeGeneration() != generation)
    {
        generation = memory.getVU1CodeGeneration();
        same = sameCode("VU1", memory.getVU1Code(), g_vu1Image, g_vu1Reached, sizeof(g_vu1Image));
    }
    // PS2PORT_VU1_LOG=1: once a second, "[vu1e]" lines with each start address (in pairs) and its calls and XGKICKs then
    static const bool log = std::getenv("PS2PORT_VU1_LOG") != nullptr;
    static uint32_t calls[0x800], kicks[0x800];
    static uint32_t current = 0;
    if (log)
    {
        static auto last = std::chrono::steady_clock::now();
        static const auto t0 = last;
        current = (startPC / 8u) & 0x7FFu;
        ++calls[current];
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1))
        {
            last = now;
            std::fprintf(stderr, "[vu1e] t=%.0f", std::chrono::duration<double>(now - t0).count());
            for (uint32_t e = 0; e < 0x800u; ++e)
                if (calls[e])
                    std::fprintf(stderr, " %x:%u/%u", e, calls[e], kicks[e]);
            std::fputc('\n', stderr);
            std::memset(calls, 0, sizeof(calls));
            std::memset(kicks, 0, sizeof(kicks));
        }
    }
    if (off)
        return false;
    if (same && !g_memory)
    {
        g_memory = &memory;
        egVu1SetKickSink([](const uint8_t *data, uint32_t size)
                         {
                             ++kicks[current];
                             g_memory->submitGifPacket(GifPathId::Path1, data, size); });
    }
    if (same)
    {
        if (!g_vu1Ours)
        {
            toNative(interpreter);
            g_vu1Ours = true;
        }
        if (egVu1Run(g_vu1, memory.getVU1Data(), startPC / 8u, top, itop))
        {
            ++g_stats.vu1Native;
            return true;
        }
    }
    if (g_vu1Ours)
    {
        toInterpreter(interpreter);
        g_vu1Ours = false;
    }
    ++g_stats.vu1Other;
    return false;
}
