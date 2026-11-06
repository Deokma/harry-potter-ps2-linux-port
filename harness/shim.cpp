// ps2port trial, step 3: a stand-in for PS2Runtime, so a recorded guest call
// can be replayed from PS2Recomp's generated code without the rest of the
// runtime (GS, IOP, kernel, host window).
//
// The per-call work matches the real runtime (ps2_runtime.cpp
// dispatchGuestBranch / lookupFunction, EeScheduler::checkpointDue +
// accountCycles): checkpoint charge, atomic flag loads, deadline compare,
// IOP cycle carry (the IOP interpreter itself is not run), dispatch history
// push, two table lookups, indirect call. Anything the recorded call should
// not need (MMIO, syscalls, breaks, VU0 microprograms, missing functions)
// is counted and makes the replay invalid.
//
// Lives under private/: it is built against PS2Recomp's headers and the
// game-derived generated code.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <ctime>

#include "runtime/ps2_vu1.h"
#include "runtime/gs/gs_frontend.h"

#include "harness.h"
#include "eegen_rt.h"
#include "eegen_fp.h"

uint8_t *ps2portRdram = nullptr; // set by main.cpp (syscalls that write guest memory)

namespace harness
{
    bool g_egModes = false;
    bool g_isEg[kTableSlots] = {};

    int enterMode(uint32_t pc)
    {
        if (!g_egModes)
            return -1;
        const bool want = (pc >> 2) < kTableSlots && g_isEg[pc >> 2];
        const bool now = eg_fz_on();
        if (want == now)
            return -1;
        want ? eg_mode_eegen() : eg_mode_ps2recomp();
        return now ? 1 : 0;
    }

    void leaveMode(int previous)
    {
        if (previous >= 0)
            previous ? eg_mode_eegen() : eg_mode_ps2recomp();
    }
}

namespace harness
{
    template <typename T> T mmioLoad(uint32_t addr); // hwmodel.inc
    template <typename T> void mmioStore(uint32_t addr, T value);
}

namespace harness
{
    PS2Runtime::RecompiledFunction g_table[kTableSlots] = {};
    Counters g_counters{};

    namespace
    {
        // EeScheduler state the checkpoint reads
        uint64_t g_eeCycle = 0;
        uint64_t g_sliceEndCycle = ~0ull;
        std::atomic<bool> g_checkpointPending{false};
        std::atomic<bool> g_stopRequested{false};
        std::atomic<uint64_t> g_nextDeadlineCycle{0};
        // IOP stepping is reduced to its cycle carry
        uint64_t g_iopCarry = 0;
        uint64_t g_iopCycles = 0;

        struct DispatchHistory
        {
            std::array<uint32_t, 64> pcs{};
            uint32_t next = 0u;
            bool wrapped = false;
        };
        thread_local DispatchHistory g_history;

        bool slot(uint32_t address, uint32_t &out)
        {
            if ((address & 3u) != 0u || address >= kTableEnd)
                return false;
            out = address >> 2;
            return true;
        }

        void unexpected(const char *what, uint32_t value)
        {
            ++g_counters.unexpected;
            if (g_counters.unexpected <= 8u)
                std::fprintf(stderr, "[harness] unexpected %s 0x%08x\n", what, value);
        }
    }

    // out of line, like EeScheduler::checkpointDue in its own translation unit
    __attribute__((noinline)) bool checkpointDue(uint32_t cycles) noexcept
    {
        ++g_counters.checkpoints;
        const uint64_t elapsed = cycles ? cycles : 1u;
        g_eeCycle += elapsed;
        const uint64_t total = g_iopCarry + elapsed;
        g_iopCycles += total / 8u;
        g_iopCarry = total % 8u;
        if (g_checkpointPending.load(std::memory_order_acquire) ||
            g_stopRequested.load(std::memory_order_acquire))
            return true;
        const uint64_t next = g_nextDeadlineCycle.load(std::memory_order_acquire);
        if (next != 0u && g_eeCycle >= next)
            return true;
        // past the slice end the real scheduler also checks for ready
        // threads of higher priority; with no other threads that is false
        (void)(g_eeCycle < g_sliceEndCycle);
        return false;
    }

    uint64_t iopCyclesCharged() { return g_iopCycles; }

    // HARNESS_SKIP=0xADDR[,0xADDR...]: calls to these return at once (counted)
    std::map<uint32_t, uint64_t> g_skipped = []()
    {
        std::map<uint32_t, uint64_t> m;
        if (const char *e = std::getenv("HARNESS_SKIP"))
            for (const char *q = e; *q;)
            {
                char *end = nullptr;
                m[static_cast<uint32_t>(std::strtoul(q, &end, 0))] = 0;
                q = (*end == ',') ? end + 1 : end;
                if (end == q && *q == '\0')
                    break;
            }
        return m;
    }();

    // HARNESS_STOP_TIMER (hwmodel.inc) leaves through this
    std::jmp_buf g_stopJmp;

    // HARNESS_CALLS=PATH: count calls per target, written by report()
    std::map<uint32_t, uint64_t> *g_calls = std::getenv("HARNESS_CALLS") ? new std::map<uint32_t, uint64_t>() : nullptr;

    bool skipped(uint32_t pc)
    {
        if (g_skipped.empty())
            return false;
        const auto it = g_skipped.find(pc);
        if (it == g_skipped.end())
            return false;
        ++it->second;
        return true;
    }
}

using namespace harness;

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    DispatchHistory &h = g_history;
    h.pcs[h.next] = address;
    h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
    if (h.next == 0u)
        h.wrapped = true;
    uint32_t s = 0;
    if (slot(address, s) && g_table[s] != nullptr)
        return g_table[s];
    unexpected("missing function", address);
    std::abort();
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t s = 0;
    return slot(address, s) && g_table[s] != nullptr;
}

bool PS2Runtime::isStopRequested() const
{
    return g_stopRequested.load(std::memory_order_acquire);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return harness::checkpointDue(cycles);
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    (void)sourcePc;
    (void)debugName;
    ++g_counters.dispatches;
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
    if (harness::g_calls)
        ++(*harness::g_calls)[targetPc];
    if (isCall && harness::skipped(targetPc))
    {
        ctx->pc = fallthroughPc; // HARNESS_SKIP: e.g. the vsync wait, idle time on the PS2
        return true;
    }
#ifdef HARNESS_LEAN_DISPATCH
    // experiment V4: roughly what a direct call costs (one lookup and the
    // call; no checkpoint, history or second lookup)
    if (isCall)
    {
        const RecompiledFunction fn = g_table[targetPc >> 2];
        const int mode = enterMode(targetPc);
        fn(rdram, ctx, this);
        leaveMode(mode);
        if (ctx->pc == targetPc)
            ctx->pc = fallthroughPc;
        return ctx->pc == fallthroughPc;
    }
#endif
    if (harness::checkpointDue(8u)) // EeScheduler::kGuestDispatchCycles
        return false;
    if (!isCall)
    {
        if (!hasFunction(targetPc))
            unexpected("jump to missing function", targetPc);
        ctx->pc = targetPc;
        return false;
    }
    if (!hasFunction(targetPc))
    {
        unexpected("call to missing function", targetPc);
        return false;
    }
    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const int mode = enterMode(targetPc);
    targetFn(rdram, ctx, this);
    leaveMode(mode);
    if (isStopRequested() || ctx->pc == 0u)
        return false;
    if (ctx->pc == entryPc)
        ctx->pc = fallthroughPc;
    return ctx->pc == fallthroughPc;
}

// --- runtime members PS2Recomp's LibC.cpp stubs reference -----------------
// (its malloc family; the frame never reaches them)
uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t) { unexpected("guestMalloc", size); return 0; }
uint32_t PS2Runtime::guestCalloc(uint32_t n, uint32_t, uint32_t) { unexpected("guestCalloc", n); return 0; }
uint32_t PS2Runtime::guestRealloc(uint32_t a, uint32_t, uint32_t) { unexpected("guestRealloc", a); return 0; }
void PS2Runtime::guestFree(uint32_t a) { unexpected("guestFree", a); }

// byte access LibC.cpp's string stubs use (via runtime->memory())
uint8_t PS2Memory::read8(uint32_t addr)
{
    if (const uint8_t *p = getConstMemPtr(ps2portRdram, addr))
        return *p;
    return harness::mmioLoad<uint8_t>(addr);
}

void PS2Memory::write8(uint32_t addr, uint8_t value)
{
    if (uint8_t *p = getMemPtr(ps2portRdram, addr))
        *p = value;
    else
        harness::mmioStore(addr, value);
}

// --- things the recorded call should not need -----------------------------

#include "hwmodel.inc"
