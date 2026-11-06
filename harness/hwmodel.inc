// --- the "idle hardware" model -------------------------------------------
// Deterministic, so two code generators replaying the same state see the
// same hardware. What it leaves out (interrupts, other threads, the IOP) is
// listed in PHASE_3 Part C. Counts go to harness::report().

namespace harness
{
    namespace
    {
        std::array<uint8_t, 4096> g_vu0Data{}, g_vu0Code{}, g_vu0DataInit{}, g_vu0CodeInit{};
        std::array<uint8_t, 16384> g_vu1Data{}, g_vu1Code{};
        std::unordered_map<uint32_t, uint64_t> g_regs; // last value written, by aligned address
        uint32_t g_timerTicks = 0;
        int g_timerStores = 0;
        const int g_stopTimer = std::getenv("HARNESS_STOP_TIMER") ? std::atoi(std::getenv("HARNESS_STOP_TIMER")) : 0;
        std::map<std::string, std::array<uint64_t, 2>> g_mmio; // region -> loads, stores
        std::map<uint32_t, uint64_t> g_syscallsByNumber;
        uint64_t g_vu0Programs = 0;
        size_t g_vu0Cursor = 0; // HARNESS_VU0=2: next recorded program result
        std::map<std::string, uint64_t> g_stubs;
        std::map<int32_t, int32_t> g_semaCount; // counting semaphores, all start at 0

        uint32_t physical(uint32_t a) { return a >= 0x80000000u ? (a & 0x1FFFFFFFu) : a; }

        // DMA kicks: counted, and the two channels that write memory the game
        // reads back (scratchpad <-> RDRAM) copy at once. The others (VIF,
        // GIF, SIF, IPU) go to hardware the stand-in doesn't have.
        uint8_t *g_dmaRam = nullptr;
        struct DmaStat
        {
            uint64_t kicks = 0, qwords = 0, chains = 0;
        };
        std::map<uint32_t, DmaStat> g_dmaStats;
        const bool g_sprDma = !std::getenv("HARNESS_NO_SPR_DMA");

        uint64_t dmaReg(uint32_t p)
        {
            const auto it = g_regs.find(p);
            return it == g_regs.end() ? 0 : it->second;
        }

        void dmaKick(uint32_t base)
        {
            const uint32_t chcr = static_cast<uint32_t>(dmaReg(base)), madr = static_cast<uint32_t>(dmaReg(base + 0x10u));
            const uint32_t qwc = static_cast<uint32_t>(dmaReg(base + 0x20u)) & 0xFFFFu, sadr = static_cast<uint32_t>(dmaReg(base + 0x80u));
            DmaStat &stat = g_dmaStats[base];
            ++stat.kicks;
            stat.qwords += qwc;
            const uint32_t mod = (chcr >> 2) & 3u;
            if (mod != 0u)
                ++stat.chains;
            if (!g_sprDma || !g_dmaRam || mod != 0u || (base != 0x1000D000u && base != 0x1000D400u))
                return;
            uint8_t *spr = ps2GetScratchpadHostPtr();
            for (uint32_t q = 0; q < qwc; ++q)
            {
                uint8_t *ram = g_dmaRam + ((madr + q * 16u) & 0x01FFFFF0u);
                uint8_t *pad = spr + ((sadr + q * 16u) & 0x3FF0u);
                if (base == 0x1000D000u)
                    std::memcpy(ram, pad, 16); // fromSPR
                else
                    std::memcpy(pad, ram, 16); // toSPR
            }
            // as the hardware leaves them
            g_regs[base + 0x10u] = madr + qwc * 16u;
            g_regs[base + 0x20u] = 0u;
            g_regs[base + 0x80u] = sadr + qwc * 16u;
        }

        const char *region(uint32_t p)
        {
            if (p >= 0x70000000u && p < 0x70004000u) return "scratchpad";
            if (p >= 0x10000000u && p < 0x10002000u) return "timers";
            if (p >= 0x10002000u && p < 0x10003000u) return "IPU";
            if (p >= 0x10003000u && p < 0x10004000u) return "GIF/VIF regs";
            if (p >= 0x10004000u && p < 0x10008000u) return "VIF/GIF/IPU FIFOs";
            if (p >= 0x10008000u && p < 0x1000E000u) return "DMA channels";
            if (p >= 0x1000E000u && p < 0x1000F000u) return "DMA control";
            if (p >= 0x1000F000u && p < 0x10010000u) return "INTC/SIF/misc";
            if (p >= 0x11000000u && p < 0x11010000u) return "VU memory";
            if (p >= 0x12000000u && p < 0x12002000u) return "GS privileged";
            if (p >= 0x1FC00000u && p < 0x20000000u) return "BIOS";
            return "other";
        }

        // backing store for memories that live outside RDRAM
        uint8_t *backing(uint32_t p)
        {
            if (p >= 0x70000000u && p < 0x70004000u) return ps2GetScratchpadHostPtr() + (p - 0x70000000u);
            if (p >= 0x11000000u && p < 0x11001000u) return g_vu0Code.data() + (p - 0x11000000u);
            if (p >= 0x11004000u && p < 0x11005000u) return g_vu0Data.data() + (p - 0x11004000u);
            if (p >= 0x11008000u && p < 0x1100C000u) return g_vu1Code.data() + (p - 0x11008000u);
            if (p >= 0x1100C000u && p < 0x11010000u) return g_vu1Data.data() + (p - 0x1100C000u);
            return nullptr;
        }

        bool isDmaChcr(uint32_t p)
        {
            switch (p)
            {
            case 0x10008000u: case 0x10009000u: case 0x1000A000u: case 0x1000B000u: case 0x1000B400u:
            case 0x1000C000u: case 0x1000C400u: case 0x1000C800u: case 0x1000D000u: case 0x1000D400u:
                return true;
            default:
                return false;
            }
        }

        uint64_t loadReg(uint32_t p)
        {
            const auto it = g_regs.find(p & ~7u);
            const uint64_t stored = it != g_regs.end() ? it->second >> (8 * (p & 4u)) : 0u;
            if (isDmaChcr(p))
                return stored & ~0x100ull; // STR: every transfer has finished
            if (p == 0x1000E010u)
                return (stored & 0xFFFF0000ull) | 0x3FFu; // D_STAT: all channels complete
            if (p == 0x10003020u || p == 0x10003800u || p == 0x10003C00u)
                return 0; // GIF / VIF0 / VIF1 STAT: idle, FIFOs empty
            if ((p & ~0x1800u) == 0x10000000u)
                return (g_timerTicks += 64u) & 0xFFFFu; // Tn_COUNT: advances per read
            return stored;
        }
    }

    template <typename T>
    T mmioLoad(uint32_t addr)
    {
        const uint32_t p = physical(addr);
#ifdef HARNESS_COUNTING // a string-keyed map per access: 8 % of a timed frame
        ++g_mmio[region(p)][0];
#endif
        T value{};
        if (uint8_t *b = backing(p))
            std::memcpy(&value, b, sizeof(T));
        else
        {
            const uint64_t v = loadReg(p);
            std::memcpy(&value, &v, std::min(sizeof(T), sizeof(v)));
        }
        return value;
    }

    template <typename T>
    void mmioStore(uint32_t addr, T value)
    {
        const uint32_t p = physical(addr);
#ifdef HARNESS_COUNTING
        ++g_mmio[region(p)][1];
#endif
        if (uint8_t *b = backing(p))
        {
            std::memcpy(b, &value, sizeof(T));
            return;
        }
        uint64_t v = 0;
        std::memcpy(&v, &value, std::min(sizeof(T), sizeof(v)));
        uint64_t &slot = g_regs[p & ~7u];
        const unsigned shift = 8 * (p & 4u);
        const uint64_t mask = (sizeof(T) >= 8 ? ~0ull : ((1ull << (8 * sizeof(T))) - 1)) << shift;
        slot = (slot & ~mask) | ((v << shift) & mask);
        if (isDmaChcr(p) && (v & 0x100u))
            dmaKick(p);
        // HARNESS_STOP_TIMER=N: the run ends at the Nth store to T0_COUNT. The
        // game resets that timer right after its vsync wait, and every build
        // comes through here (eegen compiles the wait itself out), so a frame
        // resumed from the wait (first store) ends at its next wait (second).
        if (p == 0x10000000u && g_stopTimer > 0 && ++g_timerStores == g_stopTimer)
            std::longjmp(g_stopJmp, 1);
    }

    void loadVu0(const std::string &base)
    {
        const std::pair<const char *, std::array<uint8_t, 4096> *> files[] = {
            {".vu0data", &g_vu0DataInit}, {".vu0code", &g_vu0CodeInit}};
        for (const auto &[suffix, mem] : files)
            if (std::FILE *f = std::fopen((base + suffix).c_str(), "rb"))
            {
                (void)std::fread(mem->data(), 1, mem->size(), f);
                std::fclose(f);
            }
    }

    void resetHardware()
    {
        g_vu0Data = g_vu0DataInit;
        g_vu0Code = g_vu0CodeInit;
        g_vu0Cursor = 0;
        g_vu1Data.fill(0);
        g_vu1Code.fill(0);
        g_regs.clear();
        g_timerTicks = 0;
        g_timerStores = 0;
        g_semaCount.clear();
    }

    void stubCalled(const char *name) { ++g_stubs[name]; }

    void report()
    {
        if (g_calls)
            if (std::FILE *f = std::fopen(std::getenv("HARNESS_CALLS"), "w"))
            {
                for (const auto &[pc, n] : *g_calls)
                    std::fprintf(f, "0x%08x %llu%c", pc, static_cast<unsigned long long>(n), 10);
                std::fclose(f);
            }
        std::printf("hardware (loads/stores, all iterations):");
        for (const auto &[name, n] : g_mmio)
            std::printf(" %s %llu/%llu", name.c_str(), static_cast<unsigned long long>(n[0]),
                        static_cast<unsigned long long>(n[1]));
        std::printf("\nsyscalls (all iterations):");
        for (const auto &[id, n] : g_syscallsByNumber)
            std::printf(" 0x%02x x%llu", id, static_cast<unsigned long long>(n));
        std::printf("; VU0 microprograms skipped: %llu\ndefault stubs (all iterations):",
                    static_cast<unsigned long long>(g_vu0Programs));
        for (const auto &[name, n] : g_stubs)
            std::printf(" %s x%llu", name.c_str(), static_cast<unsigned long long>(n));
        std::printf("\n");
    }
}

// Every syscall answers as an idle kernel would and never blocks (the replay
// has one thread, so locks are free and waits are already satisfied).
static void harnessSyscall(R5900Context *ctx, uint32_t id)
{
    ++g_counters.syscalls;
    ++harness::g_syscallsByNumber[id];
    int32_t result = 0;
    switch (id)
    {
    case 0x2Fu: // GetThreadId
        result = 1;
        break;
    case 0x42u: case 0x43u: // (i)SignalSema
        ++harness::g_semaCount[static_cast<int32_t>(GPR_U32(ctx, 4))];
        result = static_cast<int32_t>(GPR_U32(ctx, 4));
        break;
    case 0x44u: // WaitSema: never blocks (a wait that would block is taken as satisfied)
    {
        int32_t &count = harness::g_semaCount[static_cast<int32_t>(GPR_U32(ctx, 4))];
        if (count > 0)
            --count;
        result = static_cast<int32_t>(GPR_U32(ctx, 4));
        break;
    }
    case 0x45u: case 0x46u: // (i)PollSema: -1 when the count is 0, as the EE kernel does
    {
        int32_t &count = harness::g_semaCount[static_cast<int32_t>(GPR_U32(ctx, 4))];
        result = count > 0 ? (--count, static_cast<int32_t>(GPR_U32(ctx, 4))) : -1;
        break;
    }
    case 0x47u: case 0x48u: // (i)ReferSemaStatus: count 1, max 1, nobody waiting
    {
        if (uint8_t *s = getMemPtr(ps2portRdram, GPR_U32(ctx, 5)))
        {
            const int32_t fields[4] = {harness::g_semaCount[static_cast<int32_t>(GPR_U32(ctx, 4))], 1, 0, 0};
            std::memcpy(s, fields, sizeof(fields));
        }
        result = static_cast<int32_t>(GPR_U32(ctx, 4));
        break;
    }
    default: // anything else succeeds with 0 (counted by number)
        break;
    }
    SET_GPR_S32(ctx, 2, result);
}

void PS2Runtime::handleSyscall(uint8_t *, R5900Context *ctx)
{
    harnessSyscall(ctx, GPR_U32(ctx, 3));
}

void PS2Runtime::handleSyscall(uint8_t *, R5900Context *ctx, uint32_t encodedSyscallId)
{
    harnessSyscall(ctx, encodedSyscallId ? encodedSyscallId : GPR_U32(ctx, 3));
}

void PS2Runtime::handleBreak(uint8_t *, R5900Context *ctx) { unexpected("break", ctx->pc); }
void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception) { unexpected("exception", ctx->pc); }
// VU0 microprograms (vcallms): counted and skipped, or with HARNESS_VU0=1
// run in PS2Recomp's VU interpreter on the replay's VU0 memories (the same
// context copies as ps2_runtime.cpp), timed per call and counted in VU
// cycles. Running them changes what the frame computes afterwards (the
// reference skips them), so this costs them; it does not check them.
// HARNESS_VU0=2: an extra first pass runs the interpreter and records each
// program's result; the timed iterations apply the recorded results. That
// times the EE side of a frame that gets its VU0 results, without the
// interpreter in the caches.
// HARNESS_VU0=3: the programs translated by eegen/vu0gen.py run as native
// code; the interpreter takes the rest and any call a program gives up on.
// HARNESS_VU0=4: differential check. Every call runs in both, and the
// translated program's registers and VU data must equal the interpreter's.
#if __has_include("gen_vu0/vu0_programs.inc")
#include "eegen_vu0.h"
#include "gen_vu0/vu0_programs.inc"
#else
typedef bool (*EgVu0Fn)(R5900Context *, uint8_t *);
static EgVu0Fn eg_vu0_lookup(uint32_t) { return nullptr; }
#endif
namespace harness
{
    namespace
    {
        VU1Interpreter g_vu0(VU1Interpreter::Unit::VU0);
        const int g_vu0Mode = std::getenv("HARNESS_VU0") ? std::atoi(std::getenv("HARNESS_VU0")) : 0;
        uint64_t g_vu0Cycles = 0, g_vu0Nanos = 0, g_vu0Longest = 0;
        uint64_t g_vu0EntryCalls[512]{}, g_vu0EntryCycles[512]{}, g_vu0EntryMax[512]{};
        uint64_t g_vu0Compiled = 0, g_vu0GaveUp = 0, g_vu0NotTranslated = 0, g_vu0Checked = 0, g_vu0Mismatches = 0;

        // HARNESS_VU0=4: bits, not float comparisons
        void compareVu0(uint32_t entry, const R5900Context &a, const R5900Context &b, const uint8_t *dataA, const uint8_t *dataB)
        {
            ++g_vu0Checked;
            bool bad = false;
            const auto report = [&](const char *what, uint32_t n, const void *x, const void *y, size_t size)
            {
                bad = true;
                if (g_vu0Mismatches >= 12)
                    return;
                uint32_t p[4]{}, q[4]{};
                std::memcpy(p, x, size);
                std::memcpy(q, y, size);
                std::printf("VU0 mismatch, entry 0x%03x, %s%u: translated %08x %08x %08x %08x, interpreter %08x %08x %08x %08x%c",
                            entry, what, n, p[0], p[1], p[2], p[3], q[0], q[1], q[2], q[3], 10);
            };
            for (uint32_t i = 1; i < 32u; ++i)
                if (std::memcmp(&a.vu0_vf[i], &b.vu0_vf[i], 16) != 0)
                    report("vf", i, &a.vu0_vf[i], &b.vu0_vf[i], 16);
            for (uint32_t i = 1; i < 16u; ++i)
                if (a.vi[i] != b.vi[i])
                    report("vi", i, &a.vi[i], &b.vi[i], 2);
            if (std::memcmp(&a.vu0_acc, &b.vu0_acc, 16) != 0)
                report("acc", 0, &a.vu0_acc, &b.vu0_acc, 16);
            if (std::memcmp(&a.vu0_q, &b.vu0_q, 4) != 0)
                report("q", 0, &a.vu0_q, &b.vu0_q, 4);
            for (uint32_t off = 0; off < 4096u; off += 16u)
                if (std::memcmp(dataA + off, dataB + off, 16) != 0)
                    report("data+", off, dataA + off, dataB + off, 16);
            g_vu0Mismatches += bad;
        }

        struct Vu0Write
        {
            uint16_t offset;
            uint8_t bytes[16];
        };
        struct Vu0Result
        {
            uint32_t address, firstWrite, writes;
            VU1State state;
        };
        std::vector<Vu0Result> g_vu0Log;
        std::vector<Vu0Write> g_vu0LogWrites;
        bool g_vu0Replaying = false;

        uint64_t nanosNow()
        {
            timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + static_cast<uint64_t>(t.tv_nsec);
        }

        void vu0FromContext(const R5900Context *ctx, VU1State &s)
        {
            std::memset(&s, 0, sizeof(s));
            for (uint32_t i = 0; i < 32u; ++i)
                _mm_storeu_ps(s.vf[i], ctx->vu0_vf[i]);
            for (uint32_t i = 0; i < 16u; ++i)
                s.vi[i] = static_cast<int16_t>(ctx->vi[i]);
            _mm_storeu_ps(s.acc, ctx->vu0_acc);
            s.q = ctx->vu0_q;
            s.p = ctx->vu0_p;
            s.i = ctx->vu0_i;
            alignas(16) uint32_t r[4]{};
            _mm_storeu_si128(reinterpret_cast<__m128i *>(r), _mm_castps_si128(ctx->vu0_r));
            s.r = 0x3F800000u | (r[0] & 0x007FFFFFu);
            s.pc = ctx->vu0_pc;
            s.mac = ctx->vu0_mac_flags;
            s.clip = ctx->vu0_clip_flags;
            s.status = ctx->vu0_status;
            s.itop = ctx->vu0_itop;
            s.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
            s.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;
            s.vf[0][0] = s.vf[0][1] = s.vf[0][2] = 0.0f;
            s.vf[0][3] = 1.0f;
            s.vi[0] = 0;
        }

        void vu0ToContext(const VU1State &s, R5900Context *ctx)
        {
            for (uint32_t i = 0; i < 32u; ++i)
                ctx->vu0_vf[i] = _mm_loadu_ps(s.vf[i]);
            for (uint32_t i = 0; i < 16u; ++i)
                ctx->vi[i] = static_cast<uint16_t>(s.vi[i]);
            ctx->vu0_acc = _mm_loadu_ps(s.acc);
            ctx->vu0_q = s.q;
            ctx->vu0_p = s.p;
            ctx->vu0_i = s.i;
            ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(s.r)));
            ctx->vu0_mac_flags = s.mac;
            ctx->vu0_clip_flags = ctx->vu0_clip_flags2 = s.clip;
            ctx->vu0_status = static_cast<uint16_t>(s.status);
            ctx->vu0_itop = s.itop;
            ctx->vu0_pc = ctx->vu0_tpc = s.pc;
            ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (s.stoppedByD ? 2u : 0u) | (s.stoppedByT ? 4u : 0u);
            ctx->vu0_vpu_stat2 = 0;
            ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
            ctx->vi[0] = 0;
        }

        void applyVu0Result(const Vu0Result &r, R5900Context *ctx)
        {
            vu0ToContext(r.state, ctx);
            for (uint32_t w = 0; w < r.writes; ++w)
            {
                const Vu0Write &write = g_vu0LogWrites[r.firstWrite + w];
                std::memcpy(g_vu0Data.data() + write.offset, write.bytes, 16);
            }
        }

        void runVu0(R5900Context *ctx, uint32_t address)
        {
            ++g_vu0Programs;
            if (g_vu0Mode == 0)
                return;
            if (g_vu0Replaying)
            {
                if (g_vu0Cursor >= g_vu0Log.size() || g_vu0Log[g_vu0Cursor].address != address)
                    unexpected("VU0 replay out of step", address);
                else
                    applyVu0Result(g_vu0Log[g_vu0Cursor++], ctx);
                return;
            }
            const uint32_t entry = (address >> 3) & 511u;
            const EgVu0Fn translated = g_vu0Mode >= 3 ? eg_vu0_lookup(entry) : nullptr;
            if (g_vu0Mode == 3)
            {
                if (translated && translated(ctx, g_vu0Data.data()))
                {
                    ++g_vu0Compiled;
                    return;
                }
                ++(translated ? g_vu0GaveUp : g_vu0NotTranslated);
            }
            static R5900Context shadow;
            static std::array<uint8_t, 4096> shadowData;
            bool haveShadow = false;
            if (g_vu0Mode == 4 && translated)
            {
                shadow = *ctx;
                shadowData = g_vu0Data;
                haveShadow = translated(&shadow, shadowData.data());
                ++(haveShadow ? g_vu0Compiled : g_vu0GaveUp);
            }
            else if (g_vu0Mode == 4)
                ++g_vu0NotTranslated;
            std::array<uint8_t, 4096> before;
            if (g_vu0Mode == 2)
                before = g_vu0Data;
            const uint64_t t0 = nanosNow();
            g_vu0.reset();
            vu0FromContext(ctx, g_vu0.state());
            alignas(64) static unsigned char gsStorage[64]; // VU0 has no XGKICK: never used
            g_vu0.execute(g_vu0Code.data(), 4096u, g_vu0Data.data(), 4096u, *reinterpret_cast<GS *>(gsStorage),
                          nullptr, address & ~0x7u, 0u, ctx->vu0_itop, 4096u);
            vu0ToContext(g_vu0.state(), ctx);
            const uint64_t dt = nanosNow() - t0;
            if (haveShadow)
                compareVu0(entry, shadow, *ctx, shadowData.data(), g_vu0Data.data());
            g_vu0Nanos += dt;
            g_vu0Longest = std::max(g_vu0Longest, dt);
            g_vu0Cycles += g_vu0.state().cycles;
            const uint32_t e = (address >> 3) & 511u;
            ++g_vu0EntryCalls[e];
            g_vu0EntryCycles[e] += g_vu0.state().cycles;
            g_vu0EntryMax[e] = std::max<uint64_t>(g_vu0EntryMax[e], g_vu0.state().cycles);
            if (g_vu0Mode == 2)
            {
                Vu0Result r;
                r.address = address;
                r.firstWrite = static_cast<uint32_t>(g_vu0LogWrites.size());
                r.state = g_vu0.state();
                for (uint32_t off = 0; off < 4096u; off += 16u)
                    if (std::memcmp(before.data() + off, g_vu0Data.data() + off, 16) != 0)
                    {
                        Vu0Write w;
                        w.offset = static_cast<uint16_t>(off);
                        std::memcpy(w.bytes, g_vu0Data.data() + off, 16);
                        g_vu0LogWrites.push_back(w);
                    }
                r.writes = static_cast<uint32_t>(g_vu0LogWrites.size()) - r.firstWrite;
                g_vu0Log.push_back(r);
            }
        }
    }

    void setDmaRam(uint8_t *ram) { g_dmaRam = ram; }
    void reportDma()
    {
        if (!std::getenv("HARNESS_DMA_LOG"))
            return;
        std::printf("DMA kicks (all iterations):");
        for (const auto &[base, stat] : g_dmaStats)
            std::printf(" %08x: %llu kicks, %llu qwords, %llu chain", base, static_cast<unsigned long long>(stat.kicks),
                        static_cast<unsigned long long>(stat.qwords), static_cast<unsigned long long>(stat.chains));
        std::printf("%c", 10);
    }

    bool vu0Running() { return g_vu0Mode != 0; }
    bool vu0NeedsRecording() { return g_vu0Mode == 2; }
    void vu0FinishRecording() { g_vu0Replaying = true; }

    void reportVu0(int iterations)
    {
        if (g_vu0Mode == 0 || iterations <= 0)
            return;
        // the interpreter ran once (HARNESS_VU0=2, the recording pass) or every iteration
        const double runs = g_vu0Mode == 2 ? 1.0 : iterations;
        if (g_vu0Mode >= 3)
            std::printf("VU0 translated programs, per frame: ran %.0f, gave up %.0f, not translated %.0f; "
                        "differential check: %llu calls, %llu mismatching%c",
                        double(g_vu0Compiled) / iterations, double(g_vu0GaveUp) / iterations,
                        double(g_vu0NotTranslated) / iterations, static_cast<unsigned long long>(g_vu0Checked),
                        static_cast<unsigned long long>(g_vu0Mismatches), 10);
        uint64_t programs = 0;
        for (uint64_t n : g_vu0EntryCalls)
            programs += n;
        std::printf("VU0 microprograms run in the interpreter, per call of the frame: %.0f programs, %.0f VU cycles, "
                    "%.3f ms (longest program %.1f us)%c",
                    double(programs) / runs, double(g_vu0Cycles) / runs, double(g_vu0Nanos) / runs * 1e-6,
                    double(g_vu0Longest) * 1e-3, 10);
        std::printf("VU0 by program entry (calls per frame, VU cycles per frame, longest in VU cycles):");
        for (uint32_t e = 0; e < 512u; ++e)
            if (g_vu0EntryCalls[e])
                std::printf(" 0x%03x:%.0f,%.0f,%llu", e, double(g_vu0EntryCalls[e]) / runs, double(g_vu0EntryCycles[e]) / runs,
                            static_cast<unsigned long long>(g_vu0EntryMax[e]));
        std::printf("%c", 10);
        if (g_vu0Mode == 2 && !g_vu0Log.empty())
        {
            // what applying the recorded results itself takes, to subtract
            static R5900Context dummy{};
            const uint64_t t0 = nanosNow();
            for (int pass = 0; pass < 20; ++pass)
                for (const Vu0Result &r : g_vu0Log)
                    applyVu0Result(r, &dummy);
            std::printf("VU0 replay: %zu results, %zu data writes, %zu bytes per result; applying them takes %.3f ms per frame%c",
                        g_vu0Log.size(), g_vu0LogWrites.size(), sizeof(VU1State), double(nanosNow() - t0) / 20.0 * 1e-6, 10);
        }
    }
}

void PS2Runtime::executeVU0Microprogram(uint8_t *, R5900Context *ctx, uint32_t a) { harness::runVu0(ctx, a); }
void PS2Runtime::vu0StartMicroProgram(uint8_t *, R5900Context *ctx, uint32_t a) { harness::runVu0(ctx, a); }

// the GIF paths the VU interpreter links against (VU1 XGKICK only)
void GS::processGIFPacket(const uint8_t *, uint32_t) { harness::unexpected("GS::processGIFPacket", 0); }
void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool)
{
    harness::unexpected("PS2Memory::submitGifPacket", 0);
}
void PS2Runtime::clearLLBit(R5900Context *ctx) { ctx->llbit = 0u; }

uint8_t PS2Runtime::Load8(uint8_t *, R5900Context *, uint32_t a) { return harness::mmioLoad<uint8_t>(a); }
uint16_t PS2Runtime::Load16(uint8_t *, R5900Context *, uint32_t a) { return harness::mmioLoad<uint16_t>(a); }
uint32_t PS2Runtime::Load32(uint8_t *, R5900Context *, uint32_t a) { return harness::mmioLoad<uint32_t>(a); }
uint64_t PS2Runtime::Load64(uint8_t *, R5900Context *, uint32_t a) { return harness::mmioLoad<uint64_t>(a); }
__m128i PS2Runtime::Load128(uint8_t *, R5900Context *, uint32_t a) { return harness::mmioLoad<__m128i>(a); }
void PS2Runtime::Store8(uint8_t *, R5900Context *, uint32_t a, uint8_t v) { harness::mmioStore(a, v); }
void PS2Runtime::Store16(uint8_t *, R5900Context *, uint32_t a, uint16_t v) { harness::mmioStore(a, v); }
void PS2Runtime::Store32(uint8_t *, R5900Context *, uint32_t a, uint32_t v) { harness::mmioStore(a, v); }
void PS2Runtime::Store64(uint8_t *, R5900Context *, uint32_t a, uint64_t v) { harness::mmioStore(a, v); }
void PS2Runtime::Store128(uint8_t *, R5900Context *, uint32_t a, __m128i v) { harness::mmioStore(a, v); }
