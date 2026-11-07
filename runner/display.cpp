// The panel: the GLES backend behind the runtime's GS, on the main thread, which owns GL. The game thread only records.
// On with PS2PORT_DISPLAY=1 (weak hooks in ps2_runtime.cpp).
//
// PS2PORT_FRAME_DIR with PS2PORT_FRAME_EVERY (vsyncs, default 120) saves what is shown as PPMs: the test mode's screenshots.
// PS2PORT_M5_STATS=1 logs a line every 5 s: game frames, where the time went, CPU by thread, temperature. A game frame is the game's
// own: this NTSC executable counts vblanks at 0x328490 and resets the count when its main loop goes on, once it has reached the number
// at 0x3A4FD8. So a frame's length in vsyncs against that number says whether it kept up.
#include "gs_gles_backend.h"
#include "gsplay_gl.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
    GSGlesBackend *g_gles = nullptr;

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
    std::fprintf(stderr, "[m5] t=%.0f vsyncs/s %.1f; game frames/s %.1f, %.1f ms avg, %.0f max, needing %u vsyncs, lasting 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu; latch avg %.1f max %.1f, swap avg %.1f max %.1f; cpu%s; %.1f C\n",
                 seconds, ticks * 1000.0 / span, flips * 1000.0 / span, flips ? span / flips : 0.0, frameMax, needed, (unsigned long long)hist[1], (unsigned long long)hist[2], (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)hist[5],
                 latchSum / std::max<uint64_t>(frames, 1u), latchMax, swapSum / std::max<uint64_t>(frames, 1u), swapMax, cpu, temperature());
    windowStart = t3;
    threads = current;
    flips = ticks = frames = 0;
    latchSum = latchMax = swapSum = swapMax = frameMax = 0;
    std::memset(hist, 0, sizeof(hist));
}
