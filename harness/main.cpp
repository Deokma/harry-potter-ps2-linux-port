// ps2port trial, step 3: replay one guest call recorded on the PC
// (PS2PORT_SNAP_PC in the trial patch) from PS2Recomp's generated code.
//
//   harness RECORDING [ITERATIONS]
//
// RECORDING.in holds the context, RDRAM and scratchpad at the call;
// RECORDING.out the same after it returned on the PC. Every iteration
// starts from RECORDING.in; only the call itself is timed. The first
// iteration's RAM and return value are compared with RECORDING.out.
// The counting build (HARNESS_COUNTING) reports guest instructions per call.

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <csignal>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "harness.h"
#include "eegen_rt.h"
#include "eegen_fp.h"

// eegen's functions (build.sh EG=1); absent in a PS2Recomp-only build
extern const EgFunction eg_functions[] __attribute__((weak));
extern const unsigned eg_function_count __attribute__((weak));
extern const bool eg_float_groups __attribute__((weak));

uint64_t harness_icount = 0;
uint64_t harness_classes[16] = {};

namespace
{
    // Hardware counters (HARNESS_PMU=1): cycles, instructions, L1I accesses
    // and misses, branch misses, summed over the timed calls.
    struct Pmu
    {
        static constexpr int kCount = 5;
        int fd[kCount] = {-1, -1, -1, -1, -1};
        uint64_t total[kCount] = {};
        const char *name[kCount] = {"cycles", "instructions", "L1I accesses", "L1I misses", "branch misses"};

        char rawNames[kCount][16] = {};

        bool open()
        {
            const uint64_t l1i = PERF_COUNT_HW_CACHE_L1I | (PERF_COUNT_HW_CACHE_OP_READ << 8);
            struct { uint32_t type; uint64_t config; } ev[kCount] = {
                {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
                {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
                {PERF_TYPE_HW_CACHE, l1i | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)},
                {PERF_TYPE_HW_CACHE, l1i | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
                {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES}};
            // HARNESS_PMU_RAW=0xE1,0xE7,...: up to five raw PMU event numbers
            // instead (Cortex-A53 TRM: 0xE1 cycles stalled on an i-cache miss,
            // 0xE7 / 0xE8 load / store stalls, 0x17 L2 refills, 0x03 L1D refills)
            if (const char *raw = std::getenv("HARNESS_PMU_RAW"))
            {
                int i = 0;
                for (const char *q = raw; *q && i < kCount; ++i)
                {
                    char *end = nullptr;
                    ev[i] = {PERF_TYPE_RAW, std::strtoull(q, &end, 0)};
                    std::snprintf(rawNames[i], sizeof(rawNames[i]), "raw 0x%llx",
                                  static_cast<unsigned long long>(ev[i].config));
                    name[i] = rawNames[i];
                    q = (*end == ',') ? end + 1 : end;
                }
                for (; i < kCount; ++i)
                    ev[i] = {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES}, name[i] = "cycles";
            }
            bool any = false;
            for (int i = 0; i < kCount; ++i)
            {
                perf_event_attr a{};
                a.size = sizeof(a);
                a.type = ev[i].type;
                a.config = ev[i].config;
                a.disabled = 1;
                a.exclude_kernel = 1;
                a.exclude_hv = 1;
                fd[i] = static_cast<int>(syscall(__NR_perf_event_open, &a, 0, -1, -1, 0));
                any |= fd[i] >= 0;
            }
            return any;
        }
        void start()
        {
            for (int f : fd)
                if (f >= 0) { ioctl(f, PERF_EVENT_IOC_RESET, 0); ioctl(f, PERF_EVENT_IOC_ENABLE, 0); }
        }
        void stop()
        {
            for (int i = 0; i < kCount; ++i)
                if (fd[i] >= 0)
                {
                    ioctl(fd[i], PERF_EVENT_IOC_DISABLE, 0);
                    uint64_t v = 0;
                    if (read(fd[i], &v, sizeof(v)) == sizeof(v))
                        total[i] += v;
                }
        }
        void print(int iterations) const
        {
            std::printf("PMU per call:");
            for (int i = 0; i < kCount; ++i)
                if (fd[i] >= 0)
                    std::printf(" %s %.0f", name[i], static_cast<double>(total[i]) / iterations);
                else
                    std::printf(" %s n/a", name[i]);
            std::printf("\n");
        }
    };
}

namespace
{
    struct State
    {
        R5900Context ctx;
        std::vector<uint8_t> rdram;
        std::vector<uint8_t> scratchpad;
    };

    bool load(const std::string &path, State &state)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        uint32_t header[4] = {};
        bool ok = std::fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x50534E50u &&
                  header[2] == sizeof(R5900Context);
        state.rdram.resize(PS2_RAM_SIZE);
        state.scratchpad.resize(PS2_SCRATCHPAD_SIZE);
        ok = ok && std::fread(&state.ctx, sizeof(R5900Context), 1, f) == 1 &&
             std::fread(state.rdram.data(), PS2_RAM_SIZE, 1, f) == 1 &&
             std::fread(state.scratchpad.data(), PS2_SCRATCHPAD_SIZE, 1, f) == 1;
        std::fclose(f);
        return ok;
    }

    double now()
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }
}

// HARNESS_SAMPLE=FILE: the host program counter every tick of CPU time
// (SIGPROF) during the timed calls, written to FILE as 64-bit addresses for
// a per-function profile (scripts/sample_profile.py maps them with nm)
namespace sampler
{
    constexpr size_t kMax = 1u << 20;
    uint64_t g_pcs[kMax];
    volatile size_t g_count = 0;
    volatile bool g_on = false;

    void onTick(int, siginfo_t *, void *uc)
    {
        if (!g_on || g_count >= kMax)
            return;
        const ucontext_t *u = static_cast<const ucontext_t *>(uc);
#if defined(__aarch64__)
        g_pcs[g_count++] = u->uc_mcontext.pc;
#else
        g_pcs[g_count++] = static_cast<uint64_t>(u->uc_mcontext.gregs[REG_RIP]);
#endif
    }

    bool start()
    {
        struct sigaction sa{};
        sa.sa_sigaction = onTick;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        const itimerval tick{{0, 1000}, {0, 1000}};
        return sigaction(SIGPROF, &sa, nullptr) == 0 && setitimer(ITIMER_PROF, &tick, nullptr) == 0;
    }

    void write(const char *path)
    {
        const itimerval off{};
        setitimer(ITIMER_PROF, &off, nullptr);
        std::FILE *f = std::fopen(path, "wb");
        if (!f)
            return;
        std::fwrite(g_pcs, sizeof(uint64_t), g_count, f);
        std::fclose(f);
        std::printf("wrote %zu samples to %s\n", static_cast<size_t>(g_count), path);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s RECORDING [ITERATIONS]\n", argv[0]);
        return 2;
    }
    const std::string base = argv[1];
    const int iterations = argc > 2 ? std::max(1, std::atoi(argv[2])) : 20;

    State in{}, out{};
    if (!load(base + ".in", in))
    {
        std::fprintf(stderr, "cannot read %s.in\n", base.c_str());
        return 1;
    }
    const bool haveOut = load(base + ".out", out);

    harness::registerFunctions();
    if (&eg_function_count != nullptr)
    {
        // eegen's functions replace PS2Recomp's at their start address;
        // other entries (resuming mid-function) stay PS2Recomp's
        for (unsigned k = 0; k < eg_function_count; ++k)
        {
            harness::g_table[eg_functions[k].address >> 2] = eg_functions[k].function;
            harness::g_isEg[eg_functions[k].address >> 2] = true;
        }
        harness::g_egModes = &eg_float_groups != nullptr && eg_float_groups;
        std::printf("eegen functions: %u%s\n", eg_function_count, harness::g_egModes ? ", float groups" : "");
    }

    // RDRAM and scratchpad the generated code works on (64-byte aligned)
    std::unique_ptr<uint8_t, decltype(&std::free)> rdram(
        static_cast<uint8_t *>(std::aligned_alloc(64, PS2_RAM_SIZE + 64)), &std::free); // slack: eegen_rt.h
    std::unique_ptr<uint8_t, decltype(&std::free)> scratchpad(
        static_cast<uint8_t *>(std::aligned_alloc(64, PS2_SCRATCHPAD_SIZE)), &std::free);
    ps2SetScratchpadHostPtr(scratchpad.get());
    harness::setDmaRam(rdram.get());
    ps2portRdram = rdram.get();
    harness::loadVu0(base);

    // the generated code only calls non-virtual members, which use no state
    alignas(64) static unsigned char runtimeStorage[sizeof(PS2Runtime)];
    PS2Runtime *runtime = reinterpret_cast<PS2Runtime *>(runtimeStorage);

    const uint32_t entry = in.ctx.pc;
    std::printf("entry 0x%08x, %d iterations, R5900Context %zu bytes\n", entry, iterations, sizeof(R5900Context));

    // Between iterations only the 4 KB pages the call changes are restored,
    // so caches stay about as warm as in a running frame loop. Without
    // RECORDING.out every iteration restores all of RDRAM.
    constexpr size_t kPage = 4096;
    std::vector<size_t> dirtyPages;
    if (haveOut)
        for (size_t p = 0; p < PS2_RAM_SIZE; p += kPage)
            if (std::memcmp(in.rdram.data() + p, out.rdram.data() + p, kPage) != 0)
                dirtyPages.push_back(p);
    std::printf("restoring %s between iterations\n",
                haveOut ? (std::to_string(dirtyPages.size()) + " dirty pages").c_str() : "all of RDRAM");
    std::memcpy(rdram.get(), in.rdram.data(), PS2_RAM_SIZE);

    Pmu pmu;
    const bool pmuOn = std::getenv("HARNESS_PMU") && pmu.open();
    if (std::getenv("HARNESS_PMU") && !pmuOn)
        std::printf("PMU unavailable\n");
    const char *samplePath = std::getenv("HARNESS_SAMPLE");
    if (samplePath && !sampler::start())
        samplePath = nullptr;
    std::vector<double> seconds;
    uint64_t instructions = 0, dispatches = 0, checkpoints = 0;
    // VU0 results change which pages the frame writes, so the reference's
    // dirty-page list doesn't hold then. HARNESS_VU0=2 records in an extra,
    // untimed pass (i = -1).
    const bool restoreAll = !haveOut || harness::vu0Running();
    std::vector<uint8_t> recordedRam;
    for (int i = harness::vu0NeedsRecording() ? -1 : 0; i < iterations; ++i)
    {
        if (restoreAll)
            std::memcpy(rdram.get(), in.rdram.data(), PS2_RAM_SIZE);
        else
            for (size_t p : dirtyPages)
                std::memcpy(rdram.get() + p, in.rdram.data() + p, kPage);
        std::memcpy(scratchpad.get(), in.scratchpad.data(), PS2_SCRATCHPAD_SIZE);
        harness::resetHardware();
        R5900Context ctx = in.ctx;
        ctx.pc = entry;
        const uint64_t icount0 = harness_icount;
        const harness::Counters c0 = harness::g_counters;

        if (pmuOn && i >= 0) pmu.start();
        sampler::g_on = samplePath != nullptr && i >= 0;
        const double t0 = now();
        // like PS2Recomp's EE scheduler: when generated code unwinds (a jump
        // into another function), carry on at ctx.pc until the call returns
        // to ra = 0, or HARNESS_STOP_TIMER ends it
        if (setjmp(harness::g_stopJmp) == 0)
            do
            {
                const PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(ctx.pc);
                const int mode = harness::enterMode(ctx.pc);
                fn(rdram.get(), &ctx, runtime);
                harness::leaveMode(mode);
            } while (ctx.pc != 0u);
        const double t1 = now();
        sampler::g_on = false;
        if (pmuOn && i >= 0) pmu.stop();

        if (i < 0)
        {
            // the recording pass: the replayed iterations must end the same
            recordedRam.assign(rdram.get(), rdram.get() + PS2_RAM_SIZE);
            harness::vu0FinishRecording();
            std::printf("VU0 recording pass: %.3f ms, %llu guest instructions\n", (t1 - t0) * 1e3,
                        static_cast<unsigned long long>(harness_icount - icount0));
            continue;
        }
        seconds.push_back(t1 - t0);
        instructions = harness_icount - icount0;
        dispatches = harness::g_counters.dispatches - c0.dispatches;
        checkpoints = harness::g_counters.checkpoints - c0.checkpoints;
        if (i == 0 && !recordedRam.empty())
        {
            size_t diff = 0;
            for (size_t b = 0; b < PS2_RAM_SIZE; ++b)
                diff += rdram.get()[b] != recordedRam[b];
            std::printf("VU0 replayed iteration vs the recording pass: %zu RDRAM bytes differ\n", diff);
        }
        if (i == 0)
            if (const char *dump = std::getenv("HARNESS_DUMP_OUT"))
            {
                // the result in .out format, for scripts/diff_ram_regions.py
                std::FILE *f = std::fopen(dump, "wb");
                const uint32_t header[4] = {0x50534E50u, 1u, static_cast<uint32_t>(sizeof(R5900Context)), 0u};
                std::fwrite(header, sizeof(header), 1, f);
                std::fwrite(&ctx, sizeof(ctx), 1, f);
                std::fwrite(rdram.get(), PS2_RAM_SIZE, 1, f);
                std::fwrite(scratchpad.get(), PS2_SCRATCHPAD_SIZE, 1, f);
                std::fclose(f);
            }

        if (i == 0 && !haveOut && std::getenv("HARNESS_WRITE_OUT"))
        {
            // the reference run: its result becomes RECORDING.out
            std::FILE *f = std::fopen((base + ".out").c_str(), "wb");
            const uint32_t header[4] = {0x50534E50u, 1u, static_cast<uint32_t>(sizeof(R5900Context)), 0u};
            std::fwrite(header, sizeof(header), 1, f);
            std::fwrite(&ctx, sizeof(ctx), 1, f);
            std::fwrite(rdram.get(), PS2_RAM_SIZE, 1, f);
            std::fwrite(scratchpad.get(), PS2_SCRATCHPAD_SIZE, 1, f);
            std::fclose(f);
            std::printf("wrote %s.out\n", base.c_str());
        }
        if (i == 0 && haveOut)
        {
            size_t diff = 0;
            for (size_t b = 0; b < PS2_RAM_SIZE; ++b)
                if (rdram.get()[b] != out.rdram[b] && ++diff <= 8)
                    std::printf("  differs at 0x%08zx: 0x%02x (PC 0x%02x)%c", b, rdram.get()[b], out.rdram[b], 10);
            const R5900Context *a = &ctx;
            const R5900Context *e = &out.ctx;
            std::printf("check vs PC: %zu RDRAM bytes differ, v0 0x%08x (PC 0x%08x), sp 0x%08x (PC 0x%08x)\n",
                        diff, GPR_U32(a, 2), GPR_U32(e, 2), GPR_U32(a, 29), GPR_U32(e, 29));
        }
    }

    if (pmuOn) pmu.print(iterations);
    if (samplePath)
        sampler::write(samplePath);
#ifdef EG_FLOAT_STATS
    std::printf("eegen float slow path: calls %llu, operand exp 255 %llu, operand denormal %llu, result exp 255 %llu, "
                "result denormal %llu, zero result %llu%c",
                (unsigned long long)eg_float_stats[0], (unsigned long long)eg_float_stats[1],
                (unsigned long long)eg_float_stats[2], (unsigned long long)eg_float_stats[3],
                (unsigned long long)eg_float_stats[4], (unsigned long long)eg_float_stats[5], 10);
    {
        std::vector<EgFloatPair> top(eg_float_pairs, eg_float_pairs + 4096);
        std::sort(top.begin(), top.end(), [](const EgFloatPair &x, const EgFloatPair &y) { return x.count > y.count; });
        std::printf("eegen float slow path, top operand pairs:");
        for (int k = 0; k < 12 && top[k].count; ++k)
            std::printf(" %08x,%08x x%llu", (unsigned)(top[k].key >> 32), (unsigned)top[k].key, (unsigned long long)top[k].count);
        std::printf("%c", 10);
    }
    std::printf("eegen float groups: fast %llu, exact %llu%c", (unsigned long long)eg_group_stats[0],
                (unsigned long long)eg_group_stats[1], 10);
    std::printf("eegen group re-runs: inf/NaN %llu, invalid %llu, overflow %llu, underflow %llu, denormal input %llu%c",
                (unsigned long long)eg_group_why[0], (unsigned long long)eg_group_why[1],
                (unsigned long long)eg_group_why[2], (unsigned long long)eg_group_why[3],
                (unsigned long long)eg_group_why[4], 10);
#endif
    if (std::getenv("HARNESS_TIMES"))
    {
        std::printf("per iteration, ms:");
        for (double s : seconds)
            std::printf(" %.1f", s * 1e3);
        std::printf("\n");
    }
    std::sort(seconds.begin(), seconds.end());
    const double best = seconds.front();
    const double median = seconds[seconds.size() / 2];
    std::printf("per call: best %.2f us, median %.2f us\n", best * 1e6, median * 1e6);
    std::printf("per call: dispatches %llu, checkpoints %llu; all iterations: lock syscalls %llu, unexpected %llu\n",
                static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(checkpoints),
                static_cast<unsigned long long>(harness::g_counters.syscalls),
                static_cast<unsigned long long>(harness::g_counters.unexpected));
    harness::report();
    harness::reportVu0(iterations);
    harness::reportDma();
#ifdef HARNESS_COUNTING
    {
        static const char *names[] = {"int ALU", "load/store", "lq/sq 128-bit", "branch/jump", "FPU",
                                      "VU0 macro", "MMI", "mul/div", "other", "nop"};
        uint64_t all = 0;
        for (uint64_t v : harness_classes)
            all += v;
        std::printf("guest instruction mix (all iterations):");
        for (int k = 0; k < 10; ++k)
            if (harness_classes[k])
                std::printf(" %s %.1f%%", names[k], all ? 100.0 * harness_classes[k] / all : 0.0);
        std::printf("%c", 10);
    }
    std::printf("guest instructions per call %llu\n", static_cast<unsigned long long>(instructions));
#else
    (void)instructions;
#endif
    return harness::g_counters.unexpected ? 3 : 0;
}
