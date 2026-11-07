// The panel: the GLES backend behind the runtime's GS, on the main thread, which owns GL. The game thread only records.
// On with PS2PORT_DISPLAY=1 (weak hooks in ps2_runtime.cpp).
//
// PS2PORT_FRAME_DIR with PS2PORT_FRAME_EVERY (vsyncs, default 120) saves what is shown as PPMs: the test mode's screenshots.
// PS2PORT_SAMPLE=FILE, with PS2PORT_SAMPLE_FROM and PS2PORT_SAMPLE_TO (seconds): every thread's program counter each tick of CPU time, for
// scripts/sample_threads.py.
// PS2PORT_M5_STATS=1 logs a line every 5 s: game frames, where the time went, CPU by thread, temperature. A game frame is the game's
// own: this NTSC executable counts vblanks at 0x328490 and resets the count when its main loop goes on, once it has reached the number
// at 0x3A4FD8. So a frame's length in vsyncs against that number says whether it kept up.
#include "gs_gles_backend.h"
#include "gsplay_gl.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <typeinfo>
#include <string>
#include <thread>
#include <vector>

// PS2PORT_THROW_LOG=1: who throws C++ exceptions (the runtime and the IOP swallow many). The first 20, then every power of two:
// type, message for the standard ones, and the throwing code's offset in the binary, for addr2line on the unstripped runner.
extern "C" [[noreturn]] void __real___cxa_throw(void *object, std::type_info *type, void (*destroy)(void *));
extern "C" [[noreturn]] void __wrap___cxa_throw(void *object, std::type_info *type, void (*destroy)(void *))
{
    static const bool on = std::getenv("PS2PORT_THROW_LOG") != nullptr;
    if (on)
    {
        static std::atomic<uint64_t> count{0};
        const uint64_t n = count.fetch_add(1, std::memory_order_relaxed);
        if (n < 20u || (n & (n - 1u)) == 0u)
        {
            const char *name = type ? type->name() : "?";
            const char *what = "";
            if (std::strcmp(name, "St13runtime_error") == 0 || std::strcmp(name, "St12out_of_range") == 0 || std::strcmp(name, "St11logic_error") == 0 ||
                std::strcmp(name, "St16invalid_argument") == 0)
                what = static_cast<std::exception *>(object)->what();
            void *from = __builtin_return_address(0);
            Dl_info info{};
            const uintptr_t base = dladdr(from, &info) ? reinterpret_cast<uintptr_t>(info.dli_fbase) : 0u;
            std::fprintf(stderr, "[throw] #%llu %s \"%.80s\" from +0x%llx in %s\n", (unsigned long long)n, name, what,
                         (unsigned long long)(reinterpret_cast<uintptr_t>(from) - base), info.dli_fname ? info.dli_fname : "?");
        }
    }
    __real___cxa_throw(object, type, destroy);
}

void ps2portNativeCounts(uint64_t out[5]) __attribute__((weak));
extern uint64_t g_ps2portTouch[10];
extern uint64_t g_ps2portVif1Irqs;
extern uint64_t g_ps2portGatherBytes[2];
uint64_t ps2portGsThreadCounts(int which);

namespace
{
    GSGlesBackend *g_gles = nullptr;

    // Game frames, exactly: the vsync wait at 0x2cd6a0 first drains its semaphore with PollSema until it fails, so one failed poll returning
    // to 0x2cd6c8 is one frame. How long each took, from one to the next, in buckets up to 33.4, 50, 100 ms and over.
    std::atomic<uint64_t> g_frameMarks{0};
    std::atomic<uint64_t> g_frameBuckets[4] = {};
    std::atomic<int64_t> g_frameMaxNs{0};
    int64_t g_lastMarkNs = 0;
}

void ps2portPollFailed(uint32_t returnAddress)
{
    if (returnAddress != 0x2cd6c8u)
        return;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_lastMarkNs != 0)
    {
        const int64_t d = now - g_lastMarkNs;
        g_frameBuckets[d <= 33400000 ? 0 : d <= 50000000 ? 1 : d <= 100000000 ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
        int64_t m = g_frameMaxNs.load(std::memory_order_relaxed);
        while (d > m && !g_frameMaxNs.compare_exchange_weak(m, d, std::memory_order_relaxed))
        {
        }
    }
    g_lastMarkNs = now;
    g_frameMarks.fetch_add(1, std::memory_order_relaxed);
}

namespace
{

    // The profile: pairs of program counter and thread id
    constexpr size_t kMaxSamples = 1u << 20;
    uint64_t g_samples[kMaxSamples * 2];
    uint64_t g_sampleLr[kMaxSamples]; // the link register, which in a leaf function (libc's copies) is where it was called from
    int32_t g_sampleEe[kMaxSamples];   // the EE guest thread running then (meaningful for the game thread's samples)
    const EeScheduler *g_sampleScheduler = nullptr;
    volatile size_t g_sampleCount = 0;

    void onSampleTick(int, siginfo_t *, void *uc)
    {
        if (g_sampleCount >= kMaxSamples)
            return;
        const ucontext_t *u = static_cast<const ucontext_t *>(uc);
        const size_t k = g_sampleCount;
#if defined(__aarch64__)
        g_samples[k * 2] = u->uc_mcontext.pc;
        g_sampleLr[k] = u->uc_mcontext.regs[30];
        g_sampleEe[k] = g_sampleScheduler ? g_sampleScheduler->currentThreadId() : -1;
#else
        g_samples[k * 2] = static_cast<uint64_t>(u->uc_mcontext.gregs[REG_RIP]);
#endif
        g_samples[k * 2 + 1] = static_cast<uint64_t>(syscall(SYS_gettid));
        g_sampleCount = k + 1;
    }

    void sampling(double seconds)
    {
        static const char *path = std::getenv("PS2PORT_SAMPLE");
        static const double from = std::getenv("PS2PORT_SAMPLE_FROM") ? std::atof(std::getenv("PS2PORT_SAMPLE_FROM")) : 0.0;
        static const double to = std::getenv("PS2PORT_SAMPLE_TO") ? std::atof(std::getenv("PS2PORT_SAMPLE_TO")) : 1e9;
        static int state = 0;
        if (!path || state == 2)
            return;
        if (state == 0 && seconds >= from)
        {
            struct sigaction sa{};
            sa.sa_sigaction = onSampleTick;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            const itimerval tick{{0, 1000}, {0, 1000}};
            sigaction(SIGPROF, &sa, nullptr);
            setitimer(ITIMER_PROF, &tick, nullptr);
            state = 1;
        }
        else if (state == 1 && seconds >= to)
        {
            const itimerval off{};
            setitimer(ITIMER_PROF, &off, nullptr);
            state = 2;
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(g_samples, sizeof(uint64_t) * 2, g_sampleCount, f);
                std::fclose(f);
            }
            if (FILE *f = std::fopen((std::string(path) + ".lr").c_str(), "wb"))
            {
                std::fwrite(g_sampleLr, sizeof(uint64_t), g_sampleCount, f);
                std::fclose(f);
            }
            if (FILE *f = std::fopen((std::string(path) + ".ee").c_str(), "wb"))
            {
                std::fwrite(g_sampleEe, sizeof(int32_t), g_sampleCount, f);
                std::fclose(f);
            }
            // where the libraries are, and which thread is which
            const std::string maps = std::string(path) + ".maps";
            FILE *in = std::fopen("/proc/self/maps", "rb");
            FILE *out = std::fopen(maps.c_str(), "wb");
            if (in && out)
            {
                char buffer[4096];
                size_t n;
                while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
                    std::fwrite(buffer, 1, n, out);
            }
            if (in)
                std::fclose(in);
            if (out)
                std::fclose(out);
            const std::string names = std::string(path) + ".threads";
            if (FILE *f = std::fopen(names.c_str(), "wb"))
            {
                if (DIR *dir = opendir("/proc/self/task"))
                {
                    while (dirent *e = readdir(dir))
                    {
                        const int tid = std::atoi(e->d_name);
                        if (tid <= 0)
                            continue;
                        char path2[64], name[64] = {};
                        std::snprintf(path2, sizeof(path2), "/proc/self/task/%d/comm", tid);
                        if (FILE *c = std::fopen(path2, "r"))
                        {
                            if (std::fgets(name, sizeof(name), c))
                                std::fprintf(f, "%d %s", tid, name);
                            std::fclose(c);
                        }
                    }
                    closedir(dir);
                }
                std::fclose(f);
            }
            std::fprintf(stderr, "[display] %zu samples written\n", static_cast<size_t>(g_sampleCount));
        }
    }

    double nowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    double temperature()
    {
        double c = 0.0;
        if (FILE *f = std::fopen("/sys/class/thermal/thermal_zone0/temp", "r"))
        {
            long v = 0;
            if (std::fscanf(f, "%ld", &v) == 1)
                c = v / 1000.0;
            std::fclose(f);
        }
        return c;
    }

    // CPU ticks by thread, from /proc
    struct ThreadCpu
    {
        std::string name;
        long ticks = 0;
    };

    std::map<int, ThreadCpu> readThreads()
    {
        std::map<int, ThreadCpu> out;
        DIR *dir = opendir("/proc/self/task");
        if (!dir)
            return out;
        while (dirent *e = readdir(dir))
        {
            const int tid = std::atoi(e->d_name);
            if (tid <= 0)
                continue;
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
            FILE *f = std::fopen(path, "r");
            if (!f)
                continue;
            char line[512] = {};
            const size_t n = std::fread(line, 1, sizeof(line) - 1, f);
            std::fclose(f);
            line[n] = 0;
            const char *open = std::strchr(line, '(');
            const char *close = std::strrchr(line, ')');
            if (!open || !close)
                continue;
            ThreadCpu t;
            t.name.assign(open + 1, close);
            long utime = 0, stime = 0;
            // after ")": state, then fields 4 to 13 of stat; utime and stime are 14 and 15
            if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &utime, &stime) == 2)
                t.ticks = utime + stime;
            out[tid] = t;
        }
        closedir(dir);
        return out;
    }

    void writeShot(PS2Runtime &runtime, const char *dir, uint64_t index, double seconds)
    {
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        if (!runtime.gs().copyLatchedHostPresentationFrame(pixels, w, h) || pixels.empty())
            return;
        char name[512];
        std::snprintf(name, sizeof(name), "%s/shot_%06llu_t%06.1f.ppm", dir, static_cast<unsigned long long>(index), seconds);
        FILE *f = std::fopen(name, "wb");
        if (!f)
            return;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        const size_t stride = pixels.size() / h;
        std::vector<uint8_t> row(static_cast<size_t>(w) * 3u);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *in = pixels.data() + stride * y;
            for (uint32_t x = 0; x < w; ++x)
            {
                row[x * 3u] = in[x * 4u];
                row[x * 3u + 1u] = in[x * 4u + 1u];
                row[x * 3u + 2u] = in[x * 4u + 2u];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
}

bool ps2portDisplayInit(PS2Runtime &runtime)
{
    if (!std::getenv("PS2PORT_DISPLAY"))
        return false;
    if (!gsplayGlInit())
    {
        std::fprintf(stderr, "[display] no GL: staying with the software GS\n");
        return false;
    }
    auto backend = std::make_unique<GSGlesBackend>();
    g_gles = backend.get();
    runtime.gs().setRasterBackend(std::move(backend));
    std::fprintf(stderr, "[display] GLES backend on, %dx%d\n", gsplayScreenWidth(), gsplayScreenHeight());
    return true;
}

void ps2portDisplayFrame(PS2Runtime &runtime)
{
    g_sampleScheduler = &runtime.eeScheduler();
    static const auto start = std::chrono::steady_clock::now();
    static uint64_t last = ~0ull;
    static uint64_t shown = 0;
    static const char *frameDir = std::getenv("PS2PORT_FRAME_DIR");
    static const uint64_t frameEvery = std::getenv("PS2PORT_FRAME_EVERY") ? std::strtoull(std::getenv("PS2PORT_FRAME_EVERY"), nullptr, 10) : 120u;
    static const bool wantStats = std::getenv("PS2PORT_M5_STATS") != nullptr;

    // Each call is one vsync's frame; a swap may already have taken a vsync or more.
    const double waitStart = nowMs();
    uint64_t tick = runtime.eeScheduler().currentVSyncTick();
    while (tick == last && nowMs() - waitStart < 50.0)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        tick = runtime.eeScheduler().currentVSyncTick();
    }
    if (tick == last)
        return;
    const uint64_t passed = last == ~0ull ? 1u : tick - last;
    last = tick;
    ++shown;

    const bool shot = frameDir && frameEvery != 0u && shown % frameEvery == 0u;
    if (shot)
        g_gles->setCaptureNextPresent(true);
    const double t0 = nowMs();
    runtime.gs().latchHostPresentationFrame();
    const double t1 = nowMs();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    sampling(seconds);
    if (shot)
    {
        g_gles->setCaptureNextPresent(false);
        writeShot(runtime, frameDir, shown, seconds);
    }
    const double t2 = nowMs();
    gsplayGlSwap();
    const double t3 = nowMs();

    if (!wantStats)
        return;

    static uint32_t lastCount = 0;
    static uint64_t sinceFlip = 0, flips = 0, ticks = 0;
    static uint64_t hist[6] = {};
    static double latchSum = 0, latchMax = 0, swapSum = 0, swapMax = 0, windowStart = 0, lastFrameAt = 0, frameMax = 0;
    static uint64_t frames = 0;
    static std::map<int, ThreadCpu> threads = readThreads();
    if (windowStart == 0.0)
        windowStart = t0;

    uint32_t count = 0, needed = 0;
    std::memcpy(&count, runtime.memory().getRDRAM() + 0x328490u, 4);
    std::memcpy(&needed, runtime.memory().getRDRAM() + 0x3A4FD8u, 4);
    sinceFlip += passed;
    ticks += passed;
    if (count < lastCount)
    {
        ++flips;
        ++hist[std::min<uint64_t>(sinceFlip, 5u)];
        sinceFlip = 0;
        if (lastFrameAt != 0.0)
            frameMax = std::max(frameMax, t0 - lastFrameAt);
        lastFrameAt = t0;
    }
    lastCount = count;
    latchSum += t1 - t0;
    latchMax = std::max(latchMax, t1 - t0);
    swapSum += t3 - t2;
    swapMax = std::max(swapMax, t3 - t2);
    ++frames;

    if (t3 - windowStart < 5000.0)
        return;
    const double span = t3 - windowStart;
    const std::map<int, ThreadCpu> current = readThreads();
    std::vector<std::pair<long, std::string>> busy;
    for (const auto &entry : current)
    {
        const auto before = threads.find(entry.first);
        const long delta = entry.second.ticks - (before == threads.end() ? 0 : before->second.ticks);
        if (delta > 0)
            busy.emplace_back(delta, entry.second.name);
    }
    std::sort(busy.rbegin(), busy.rend());
    const double tickMs = 1000.0 / static_cast<double>(sysconf(_SC_CLK_TCK));
    char cpu[256] = {};
    size_t used = 0;
    for (size_t i = 0; i < busy.size() && i < 5u && used + 40u < sizeof(cpu); ++i)
        used += static_cast<size_t>(std::snprintf(cpu + used, sizeof(cpu) - used, " %s:%.0f%%", busy[i].second.c_str(), 100.0 * busy[i].first * tickMs / span));
    {
        static uint64_t before[5] = {};
        static double gpuBefore[12] = {};
        static uint32_t tickBefore = 0;
        uint64_t counts[5] = {};
        double gpu[18] = {};
        uint32_t tick = 0;
        std::memcpy(&tick, runtime.memory().getRDRAM() + 0x38C210u, 4);
        if (ps2portNativeCounts)
            ps2portNativeCounts(counts);
        g_gles->countStats(gpu);
        const uint64_t marks = g_frameMarks.exchange(0, std::memory_order_relaxed);
        uint64_t buckets[4];
        for (int b = 0; b < 4; ++b)
            buckets[b] = g_frameBuckets[b].exchange(0, std::memory_order_relaxed);
        const double maxMs = g_frameMaxNs.exchange(0, std::memory_order_relaxed) / 1e6;
        std::fprintf(stderr, "[m5f] t=%.0f game frames/s %.1f, %.1f ms avg, %.1f max; up to 33.4 ms %llu, 50 %llu, 100 %llu, more %llu\n", seconds, marks * 1000.0 / span, marks ? span / marks : 0.0, maxMs,
                     (unsigned long long)buckets[0], (unsigned long long)buckets[1], (unsigned long long)buckets[2], (unsigned long long)buckets[3]);
        {
            static uint64_t touchBefore[11] = {};
            uint64_t touch[11];
            std::memcpy(touch, g_ps2portTouch, sizeof(g_ps2portTouch));
            touch[10] = g_ps2portVif1Irqs;
            double per[11];
            for (int k = 0; k < 11; ++k)
                per[k] = (touch[k] - touchBefore[k]) * 1000.0 / span;
            std::memcpy(touchBefore, touch, sizeof(touch));
            static uint64_t gsBefore[5] = {};
            uint64_t gs[5];
            for (int k = 0; k < 5; ++k)
                gs[k] = ps2portGsThreadCounts(k);
            std::fprintf(stderr, "[m5t] t=%.0f per s: gs priv r %.0f w %.0f; vif1 r %.0f w %.0f; gif regs %.0f; channel reads %.0f; vu1 mem %.0f; fifo %.0f; kicks vif1 %.0f gif %.0f; vif1 irqs %.0f; gs thread jobs %.0f handed %.0f waits %.0f; in jobs %.0f ms of which waiting for the EE %.0f ms\n", seconds,
                         per[0], per[1], per[2], per[3], per[4], per[5], per[6], per[7], per[8], per[9], per[10], (gs[2] - gsBefore[2]) * 1000.0 / span,
                         (gs[0] - gsBefore[0]) * 1000.0 / span, (gs[1] - gsBefore[1]) * 1000.0 / span, (gs[4] - gsBefore[4]) / 1e6 * 1000.0 / span,
                         (gs[3] - gsBefore[3]) / 1e6 * 1000.0 / span);
            std::memcpy(gsBefore, gs, sizeof(gs));
            static uint64_t gatherBefore[2] = {};
            std::fprintf(stderr, "[m5g] t=%.0f DMA chains gathered %.2f MB/s, of which already copied in the same chain %.2f MB/s\n", seconds,
                         (g_ps2portGatherBytes[0] - gatherBefore[0]) * 1000.0 / span / 1048576.0, (g_ps2portGatherBytes[1] - gatherBefore[1]) * 1000.0 / span / 1048576.0);
            std::memcpy(gatherBefore, g_ps2portGatherBytes, sizeof(gatherBefore));
        }
        std::fprintf(stderr, "[m5m] t=%.0f tick/s %.1f drawn/s %.1f; palettes %.0f, targets %.1f MB, depth %.0f (%.1f MB), textures %.0f (%.1f MB)\n", seconds, (tick - tickBefore) * 1000.0 / span, frames * 1000.0 / span,
                     gpu[12], gpu[13] / 1048576.0, gpu[14], gpu[15] / 1048576.0, gpu[16], gpu[17] / 1048576.0);
        tickBefore = tick;
        std::fprintf(stderr, "[m5w] t=%.0f per s: VU0 %.0f gave up %.0f other %.0f; VU1 %.0f other %.0f; runs %.0f vertices %.0f passes %.0f rt switches %.0f tex uploads %.1f (%.2f MB) rt uploads %.1f feedback %.1f cluts %.1f; execute %.1f ms; %.0f targets, %.0f programs\n", seconds,
                     (counts[0] - before[0]) * 1000.0 / span, (counts[1] - before[1]) * 1000.0 / span, (counts[2] - before[2]) * 1000.0 / span, (counts[3] - before[3]) * 1000.0 / span, (counts[4] - before[4]) * 1000.0 / span,
                     (gpu[0] - gpuBefore[0]) * 1000.0 / span, (gpu[1] - gpuBefore[1]) * 1000.0 / span, (gpu[2] - gpuBefore[2]) * 1000.0 / span, (gpu[3] - gpuBefore[3]) * 1000.0 / span, (gpu[4] - gpuBefore[4]) * 1000.0 / span,
                     (gpu[5] - gpuBefore[5]) * 1000.0 / span / 1048576.0, (gpu[6] - gpuBefore[6]) * 1000.0 / span, (gpu[7] - gpuBefore[7]) * 1000.0 / span, (gpu[8] - gpuBefore[8]) * 1000.0 / span, (gpu[11] - gpuBefore[11]) * 1000.0 / span, gpu[9], gpu[10]);
        std::memcpy(before, counts, sizeof(before));
        std::memcpy(gpuBefore, gpu, sizeof(gpuBefore));
    }
    std::fprintf(stderr, "[m5] t=%.0f vsyncs/s %.1f; game frames/s %.1f, %.1f ms avg, %.0f max, needing %u vsyncs, lasting 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu; latch avg %.1f max %.1f, swap avg %.1f max %.1f; cpu%s; %.1f C\n",
                 seconds, ticks * 1000.0 / span, flips * 1000.0 / span, flips ? span / flips : 0.0, frameMax, needed, (unsigned long long)hist[1], (unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)hist[5],
                 latchSum / std::max<uint64_t>(frames, 1u), latchMax, swapSum / std::max<uint64_t>(frames, 1u), swapMax, cpu, temperature());
    windowStart = t3;
    threads = current;
    flips = ticks = frames = 0;
    latchSum = latchMax = swapSum = swapMax = frameMax = 0;
    std::memset(hist, 0, sizeof(hist));
}
