// The translated microprogram (private/vu1/vu1_gen.inc, from eegen/vu1gen.py) and what it needs.
#include "vu1_rt.h"

struct R5900Context;
#include "eegen_vu0.h"
#include "eegen_vu1.h"

uint64_t g_egVu1Notes[2048];
uint64_t g_egVu1Counts[2048];
static inline void eg_vu1_note(int pc, eg_v4 v, uint32_t dest)
{
    const eg_vi4 b = (eg_vi4)v;
    for (int c = 0; c < 4; ++c)
        if ((dest & (8u >> c)) && (b[c] & 0x7F800000) == 0x7F800000)
            ++g_egVu1Notes[pc];
}

namespace
{
    EgVu1KickSink g_sink = nullptr;
    uint8_t g_wrap[16384 + 16];

    // XGKICK takes the whole packet at once, where the hardware (and the interpreter) copy one qword every two
    // cycles while the program carries on. A program that writes to the packet after kicking it would differ:
    // vu1play --mode check compares packets.
    void eg_vu1_kick(const uint8_t *data, uint32_t qword)
    {
        const uint32_t start = (qword * 16u) & 16383u;
        uint32_t pos = 0;
        for (;;)
        {
            uint64_t lo;
            std::memcpy(&lo, data + ((start + pos) & 16383u), 8);
            const uint32_t nloop = lo & 0x7FFFu;
            const uint32_t flg = (lo >> 58) & 3u;
            uint32_t nreg = (lo >> 60) & 15u;
            if (nreg == 0u)
                nreg = 16u;
            pos += 16u;
            if (flg == 0u)
                pos += nloop * nreg * 16u;
            else if (flg == 1u)
                pos += ((nloop * nreg + 1u) & ~1u) * 8u;
            else
                pos += nloop * 16u;
            if ((lo >> 15) & 1u)
                break;
            if (pos >= 16384u)
                break;
        }
        if (pos > 16384u)
            pos = 16384u;
        if (start + pos <= 16384u)
        {
            g_sink(data + start, pos);
            return;
        }
        const uint32_t first = 16384u - start;
        std::memcpy(g_wrap, data + start, first);
        std::memcpy(g_wrap + first, data, pos - first);
        g_sink(g_wrap, pos);
    }
}

void egVu1SetKickSink(EgVu1KickSink sink) { g_sink = sink; }

#include "vu1_gen.inc"
