// Thread spike (PHASE_4): can eegen's code run several guest threads? One host
// stack and one R5900Context per guest thread, a small priority scheduler, and an
// interrupt from a host timer signal that runs a guest handler at any instruction.
//
//   toy ROUNDS            passes round-robin, no timer: times a switch
//   toy ROUNDS timer      with interrupts, a high-priority thread and a RAM spinner
//
// Built against PS2Recomp's headers like the replay harness. Everything the game
// isn't using here is left out; a call of it would stop the run.
#include "eegen_rt.h"
#include "eegen_fp.h"
#include "toy_layout.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <sys/time.h>
#include <ucontext.h>
#include <vector>

namespace
{
    enum State { Ready, Running, Waiting, Dead };

    struct Thread
    {
        R5900Context ctx;
        ucontext_t host;
        int priority = 0;
        State state = Ready;
        bool wantFz = true; // this thread's FPU mode, must survive every switch
        uint32_t entry = 0;
        const char *name = "";
    };

    struct Sema
    {
        int count = 0;
        std::deque<Thread *> waiting;
    };

    std::vector<Thread *> g_threads;
    Thread *g_current = nullptr;
    Sema g_semas[8];
    uint8_t *g_ram = nullptr;
    PS2Runtime *g_runtime = nullptr;
    ucontext_t g_mainHost;
    int g_toFinish = 0;
    bool g_runDone = false;
    uint64_t g_switches = 0, g_modeErrors = 0;

    volatile sig_atomic_t g_inKernel = 0, g_inIrq = 0, g_pendingIrq = 0;
    bool g_needResched = false;
    alignas(16) R5900Context g_irqCtx;

    uint32_t ramU32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + a, 4); return v; }
    void setRamU32(uint32_t a, uint32_t v) { std::memcpy(g_ram + a, &v, 4); }

    PS2Runtime::RecompiledFunction eegenFunction(uint32_t address)
    {
        for (unsigned k = 0; k < eg_function_count; ++k)
            if (eg_functions[k].address == address)
                return eg_functions[k].function;
        std::printf("no eegen function at %#x\n", address);
        std::exit(2);
    }

    // VU0 is one unit on the PS2, not per thread, so its state follows the switch
    void copyVu0(const R5900Context &from, R5900Context &to)
    {
        std::memcpy(to.vu0_vf, from.vu0_vf, sizeof(to.vu0_vf));
        std::memcpy(to.vi, from.vi, sizeof(to.vi));
        to.vu0_acc = from.vu0_acc;
        to.vu0_q = from.vu0_q;
    }

    // Ready threads in order: a woken thread queues behind its priority, a preempted
    // one goes first, so it carries on as soon as the higher priority is done.
    std::deque<Thread *> g_ready;

    Thread *nextReady()
    {
        Thread *best = nullptr;
        for (Thread *t : g_ready)
            if (!best || t->priority < best->priority)
                best = t;
        return best;
    }

    void takeReady(Thread *t)
    {
        for (size_t k = 0; k < g_ready.size(); ++k)
            if (g_ready[k] == t)
            {
                g_ready.erase(g_ready.begin() + k);
                return;
            }
    }

    // Pick the next thread. Same priority doesn't preempt: a running thread gives way
    // only to a higher priority, or when it can't run.
    void schedule()
    {
        Thread *prev = g_current;
        const bool canContinue = prev->state == Running;
        Thread *next = nextReady();
        if (!next && canContinue)
            return;
        if (!next)
        {
            std::printf("deadlock: nothing can run, %d to finish, log %u, interrupts %u:", g_toFinish,
                        ramU32(TOY_LOGPTR), ramU32(TOY_IRQS));
            for (Thread *t : g_threads)
                std::printf(" %s=%d", t->name, t->state);
            std::printf(" semas %d %d %d %d\n", g_semas[1].count, g_semas[2].count, g_semas[3].count, g_semas[4].count);
            std::exit(3);
        }
        if (canContinue)
        {
            if (next->priority >= prev->priority)
                return;
            prev->state = Ready;
            g_ready.push_front(prev);
        }
        takeReady(next);
        next->state = Running;
        copyVu0(prev->ctx, next->ctx);
        g_current = next;
        ++g_switches;
        swapcontext(&prev->host, &next->host);
    }

    void wake(Sema &s)
    {
        if (s.waiting.empty())
        {
            ++s.count;
            return;
        }
        Thread *t = s.waiting.front();
        s.waiting.pop_front();
        t->state = Ready;
        g_ready.push_back(t);
        if (t->priority < g_current->priority)
            g_needResched = true;
    }

    void runIrq();

    struct KernelScope
    {
        sig_atomic_t saved;
        KernelScope() : saved(g_inKernel) { g_inKernel = 1; }
        ~KernelScope()
        {
            g_inKernel = saved;
            if (!g_inKernel && !g_inIrq && g_pendingIrq)
                runIrq();
        }
    };

    // The guest handler runs on the interrupted thread's host stack with its own
    // context, as the PS2 runs it on its own stack: the thread's registers, in
    // eegen's locals or host registers, are saved by the signal frame.
    void runIrq()
    {
        g_pendingIrq = 0;
        g_inIrq = 1;
        static const PS2Runtime::RecompiledFunction handler = eegenFunction(TOY_IRQ_HANDLER);
        SET_GPR_U32(&g_irqCtx, 31, 0u);
        // a guest handler sets its own FPU mode: Linux resets it on entering a signal
        // handler on x86, and here it may also be running in a thread's other mode
        const bool fz = eg_fz_on();
        eg_mode_eegen();
        handler(g_ram, &g_irqCtx, g_runtime);
        fz ? eg_mode_eegen() : eg_mode_ps2recomp();
        g_inIrq = 0;
        if (g_needResched)
        {
            g_needResched = false;
            g_inKernel = 1;
            schedule();
            g_inKernel = 0;
        }
    }

    void onTimer(int)
    {
        if (g_inKernel || g_inIrq)
        {
            g_pendingIrq = 1;
            return;
        }
        runIrq();
    }

    void threadMain()
    {
        Thread *self = g_current;
        g_inKernel = 0;
        if (self->wantFz)
            eg_mode_eegen();
        else
            eg_mode_ps2recomp();
        eegenFunction(self->entry)(g_ram, &self->ctx, g_runtime);
        g_inKernel = 1;
        self->state = Dead;
        if (--g_toFinish == 0)
        {
            g_runDone = true;
            struct itimerval off = {};
            setitimer(ITIMER_REAL, &off, nullptr);
            setcontext(&g_mainHost);
        }
        schedule();
    }

    Thread *addThread(const char *name, uint32_t entry, int priority, uint32_t arg, uint32_t guestStack, bool wantFz)
    {
        static_assert(sizeof(Thread) > 0);
        void *memory = nullptr;
        if (posix_memalign(&memory, 64, sizeof(Thread)) != 0)
            std::exit(4);
        Thread *t = new (memory) Thread();
        std::memset(&t->ctx, 0, sizeof(t->ctx));
        t->name = name;
        t->entry = entry;
        t->priority = priority;
        t->wantFz = wantFz;
        SET_GPR_U32(&t->ctx, 4, arg);
        SET_GPR_U32(&t->ctx, 29, guestStack);
        SET_GPR_U32(&t->ctx, 31, 0u);
        t->ctx.pc = entry;
        constexpr size_t kStack = 256 * 1024;
        getcontext(&t->host);
        t->host.uc_stack.ss_sp = std::malloc(kStack);
        t->host.uc_stack.ss_size = kStack;
        t->host.uc_link = nullptr;
        makecontext(&t->host, threadMain, 0);
        g_threads.push_back(t);
        g_ready.push_back(t);
        return t;
    }

    double now()
    {
        timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return t.tv_sec + t.tv_nsec * 1e-9;
    }

    // what a switch costs by itself: two contexts handing over with nothing else
    ucontext_t g_pingHost, g_pongHost;
    uint64_t g_pings = 0;
    void pong()
    {
        for (;;)
        {
            --g_pings;
            swapcontext(&g_pongHost, &g_pingHost);
        }
    }

    double bareSwitchNanos()
    {
        constexpr uint64_t kRounds = 200000;
        static char stack[64 * 1024];
        getcontext(&g_pongHost);
        g_pongHost.uc_stack.ss_sp = stack;
        g_pongHost.uc_stack.ss_size = sizeof(stack);
        g_pongHost.uc_link = nullptr;
        makecontext(&g_pongHost, pong, 0);
        g_pings = kRounds;
        const double t0 = now();
        while (g_pings)
            swapcontext(&g_pingHost, &g_pongHost);
        return (now() - t0) * 1e9 / (kRounds * 2);
    }
}

// ---- what eegen's code calls ----
void PS2Runtime::handleSyscall(uint8_t *, R5900Context *ctx, uint32_t code)
{
    KernelScope scope;
    const int id = code ? static_cast<int>(code) : static_cast<int32_t>(GPR_U32(ctx, 3));
    const int sema = static_cast<int>(GPR_U32(ctx, 4)) & 7;
    // each thread keeps its own FPU mode through every switch and interrupt
    if (!g_inIrq && (eg_fz_on() != 0) != g_current->wantFz)
        ++g_modeErrors;
    switch (id)
    {
    case 0x44: // WaitSema
        if (g_semas[sema].count > 0)
        {
            --g_semas[sema].count;
            break;
        }
        g_current->state = Waiting;
        g_semas[sema].waiting.push_back(g_current);
        schedule();
        break;
    case 0x42: // SignalSema
        wake(g_semas[sema]);
        if (g_needResched)
        {
            g_needResched = false;
            schedule();
        }
        break;
    case -67: // iSignalSema, from the handler: the switch waits until it returns
        wake(g_semas[sema]);
        break;
    case TOY_HOOK_VU0:
    {
        float *v = reinterpret_cast<float *>(&ctx->vu0_vf[1]);
        v[0] += 1.0f;
        break;
    }
    default:
        std::printf("syscall %#x\n", id);
        std::exit(5);
    }
    SET_GPR_S32(ctx, 2, sema);
}

void PS2Runtime::handleBreak(uint8_t *, R5900Context *ctx)
{
    std::printf("break at %#x\n", ctx->pc);
    std::exit(6);
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *, R5900Context *, uint32_t target, uint32_t, uint32_t, GuestBranchKind, const char *)
{
    std::printf("unexpected dispatch to %#x\n", target);
    std::exit(7);
}

// the toy stays in RDRAM: any other address is a bug
#define TOY_NO_MMIO(a)                                 \
    do                                                 \
    {                                                  \
        std::printf("access outside RDRAM: %#x\n", a); \
        std::exit(8);                                  \
    } while (0)
uint8_t PS2Runtime::Load8(uint8_t *, R5900Context *, uint32_t a) { TOY_NO_MMIO(a); }
uint16_t PS2Runtime::Load16(uint8_t *, R5900Context *, uint32_t a) { TOY_NO_MMIO(a); }
uint32_t PS2Runtime::Load32(uint8_t *, R5900Context *, uint32_t a) { TOY_NO_MMIO(a); }
uint64_t PS2Runtime::Load64(uint8_t *, R5900Context *, uint32_t a) { TOY_NO_MMIO(a); }
__m128i PS2Runtime::Load128(uint8_t *, R5900Context *, uint32_t a) { TOY_NO_MMIO(a); }
void PS2Runtime::Store8(uint8_t *, R5900Context *, uint32_t a, uint8_t) { TOY_NO_MMIO(a); }
void PS2Runtime::Store16(uint8_t *, R5900Context *, uint32_t a, uint16_t) { TOY_NO_MMIO(a); }
void PS2Runtime::Store32(uint8_t *, R5900Context *, uint32_t a, uint32_t) { TOY_NO_MMIO(a); }
void PS2Runtime::Store64(uint8_t *, R5900Context *, uint32_t a, uint64_t) { TOY_NO_MMIO(a); }
void PS2Runtime::Store128(uint8_t *, R5900Context *, uint32_t a, __m128i) { TOY_NO_MMIO(a); }

int main(int argc, char **argv)
{
    const uint32_t rounds = argc > 1 ? std::strtoul(argv[1], nullptr, 0) : 100000u;
    const bool timer = argc > 2;
    if (posix_memalign(reinterpret_cast<void **>(&g_ram), 64, 32u << 20) != 0)
        return 4;
    std::memset(g_ram, 0, 32u << 20);
    alignas(64) static unsigned char storage[64 * 1024];
    g_runtime = reinterpret_cast<PS2Runtime *>(storage);
    setRamU32(TOY_ROUNDS, rounds);
    setRamU32(TOY_PASSES, rounds * 3);

    const double bare = bareSwitchNanos();

    // threads 0 and 1 in eegen's FPU mode, thread 2 in PS2Recomp's: each must keep its own
    addThread("a", TOY_THREAD_ENTRY, 10, 0, 0x1000000, true);
    addThread("b", TOY_THREAD_ENTRY, 10, 1, 0x1010000, true);
    addThread("c", TOY_THREAD_ENTRY, 10, 2, 0x1020000, false);
    g_toFinish = 3;
    g_semas[1].count = 1; // thread a goes first
    if (timer)
    {
        addThread("d", TOY_THREAD_D, 1, 0, 0x1030000, true);
        addThread("s", TOY_THREAD_S, 50, 0, 0x1040000, true);
        g_toFinish = 4;
        std::memset(&g_irqCtx, 0, sizeof(g_irqCtx));
        SET_GPR_U32(&g_irqCtx, 29, 0x1050000u);
        struct sigaction action = {};
        action.sa_handler = onTimer;
        sigaction(SIGALRM, &action, nullptr);
        struct itimerval every = {};
        every.it_interval.tv_usec = every.it_value.tv_usec = 1000; // 1 kHz, far above vblank
        setitimer(ITIMER_REAL, &every, nullptr);
    }

    // start: the main host context hands over to the first thread
    Thread boot;
    std::memset(&boot.ctx, 0, sizeof(boot.ctx));
    boot.state = Dead;
    g_current = &boot;
    g_inKernel = 1;
    const double t0 = now();
    {
        Thread *first = nextReady();
        takeReady(first);
        first->state = Running;
        g_current = first;
        getcontext(&g_mainHost);
        if (!g_runDone)
            setcontext(&first->host);
    }
    const double elapsed = now() - t0;
    g_inKernel = 1;
    eg_mode_ps2recomp();

    // the expected result is known without running anything
    int failures = 0;
    const uint32_t passes = rounds * 3;
    if (ramU32(TOY_LOGPTR) != passes)
        ++failures, std::printf("FAIL: %u log entries, expected %u\n", ramU32(TOY_LOGPTR), passes);
    uint32_t orderErrors = 0;
    for (uint32_t k = 0; k < passes && k < ramU32(TOY_LOGPTR); ++k)
        orderErrors += ramU32(TOY_LOG + k * 4) != k % 3;
    if (orderErrors)
        ++failures, std::printf("FAIL: %u log entries out of order\n", orderErrors);
    if (ramU32(TOY_ERR))
        ++failures, std::printf("FAIL: %u stack canaries wrong\n", ramU32(TOY_ERR));
    for (int k = 0; k < 3; ++k)
    {
        float f;
        std::memcpy(&f, g_ram + TOY_FRESULT + k * 4, 4);
        if (ramU32(TOY_RESULT + k * 4) != rounds || f != static_cast<float>(rounds) * 1.5f)
            ++failures, std::printf("FAIL: thread %d count %u, float %g\n", k, ramU32(TOY_RESULT + k * 4), f);
    }
    if (g_modeErrors)
        ++failures, std::printf("FAIL: FPU mode wrong after %llu switches\n", (unsigned long long)g_modeErrors);
    if (timer)
    {
        const uint32_t irqs = ramU32(TOY_IRQS), woken = ramU32(TOY_DCOUNT);
        const float vu0 = reinterpret_cast<const float *>(&g_threads[0]->ctx.vu0_vf[1])[0];
        float newest = vu0;
        for (Thread *t : g_threads)
            newest = std::max(newest, reinterpret_cast<const float *>(&t->ctx.vu0_vf[1])[0]);
        if (ramU32(TOY_SPINDONE) != 1)
            ++failures, std::printf("FAIL: the RAM spinner didn't finish\n");
        if (irqs < 50 || woken + 2 < irqs)
            ++failures, std::printf("FAIL: %u interrupts, thread d woke %u times\n", irqs, woken);
        if (static_cast<uint32_t>(newest) != woken)
            ++failures, std::printf("FAIL: VU0 counter %g, thread d ran %u times\n", newest, woken);
        std::printf("interrupts %u, thread d woke %u, RAM spinner done\n", irqs, woken);
    }
    std::printf("eegen functions %u, %llu switches in %.3f s: %.0f ns per switch with the work, %.0f ns bare swapcontext\n",
                eg_function_count, (unsigned long long)g_switches, elapsed,
                g_switches ? elapsed * 1e9 / g_switches : 0.0, bare);
    std::printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
    return failures ? 1 : 0;
}
