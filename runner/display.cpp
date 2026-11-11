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

#include "host/png_write.h"

#include <dirent.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <execinfo.h>
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
#include <malloc.h>
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
    std::atomic<uint64_t> g_frameTotal{0}; // game frames since the start (screenshot names)
    std::atomic<bool> g_userShot{false};   // a screenshot asked for: Menu + L1 (pad_sdl.cpp), SIGUSR1, Enter on the PC, PS2PORT_SHOT_AT
    std::atomic<uint64_t> g_frameBuckets[4] = {};
    std::atomic<int64_t> g_frameMaxNs{0};
    int64_t g_lastMarkNs = 0;
    const int64_t g_processStartNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    std::atomic<uint16_t> g_scriptButtons{0}; // PAD_* bits the state-driven script holds (ps2_pad.cpp asks)
    std::atomic<uint32_t> g_scriptStick{0};   // the same for the left stick: 0x10000 | y byte << 8 | x byte, 0 = the pad's own
    std::atomic<bool> g_shotRequest{false};   // a screenshot at the next presented frame (PS2PORT_FRAME_DIR)

    // PS2PORT_PAD_SEQ=VALUE:BUTTON[:SETTLE],...: a test script driven by the game's state, counted in game frames. Step by step: wait
    // until the guest word PS2PORT_PAD_SEQ_WORD (default 0x38c194, the play mode) has VALUE (hex, or * for any) for SETTLE frames
    // (default 10), take a screenshot, press BUTTON for 3 frames, release for 5, go on. "[padseq]" lines report each step.
    void padSequence(const uint8_t *rdram, double seconds)
    {
        struct Step
        {
            bool any;
            uint32_t value;
            uint16_t mask;
            int settle;
            std::string name;
            uint32_t stick = 0;
            int hold = 3;
        };
        static std::vector<Step> steps;
        static uint32_t word = 0x38c194u;
        static bool parsed = false;
        static size_t at = 0;
        static int phase = 0, count = 0, tail = -1;
        if (!parsed)
        {
            parsed = true;
            const char *env = std::getenv("PS2PORT_PAD_SEQ");
            if (!env)
                return;
            if (const char *w = std::getenv("PS2PORT_PAD_SEQ_WORD"))
                word = static_cast<uint32_t>(std::strtoul(w, nullptr, 0));
            static const struct { const char *name; uint16_t mask; } names[] = {
                {"LEFT", 0x0080}, {"DOWN", 0x0040}, {"RIGHT", 0x0020}, {"UP", 0x0010}, {"START", 0x0008}, {"R3", 0x0004},
                {"L3", 0x0002}, {"SELECT", 0x0001}, {"SQUARE", 0x8000}, {"CROSS", 0x4000}, {"CIRCLE", 0x2000},
                {"TRIANGLE", 0x1000}, {"R1", 0x0800}, {"L1", 0x0400}, {"R2", 0x0200}, {"L2", 0x0100}};
            std::string s(env);
            size_t p = 0;
            while (p < s.size())
            {
                size_t e = s.find(',', p);
                if (e == std::string::npos)
                    e = s.size();
                const std::string item = s.substr(p, e - p);
                p = e + 1;
                char value[16] = {}, button[16] = {};
                int settle = 10, hold = 3;
                if (std::sscanf(item.c_str(), "%15[^:]:%15[A-Z0-9]:%d:%d", value, button, &settle, &hold) < 2)
                    continue;
                Step st{value[0] == '*', static_cast<uint32_t>(std::strtoul(value, nullptr, 16)), 0, settle, button};
                st.hold = hold;
                for (const auto &n : names)
                    if (st.name == n.name)
                        st.mask = n.mask;
                // the left stick pushed fully one way: LSLEFT, LSRIGHT, LSUP, LSDOWN (VALUE:NAME:SETTLE:HOLD, HOLD in game frames)
                static const struct { const char *name; uint32_t stick; } sticks[] = {
                    {"LSLEFT", 0x18000}, {"LSRIGHT", 0x180FF}, {"LSUP", 0x10080}, {"LSDOWN", 0x1FF80}};
                for (const auto &n : sticks)
                    if (st.name == n.name)
                        st.stick = n.stick;
                if (st.mask || st.stick)
                    steps.push_back(st);
            }
            std::fprintf(stderr, "[padseq] %zu steps on word %#x\n", steps.size(), word);
        }
        if (!rdram)
            return;
        if (at >= steps.size())
        {
            // a last screenshot once the final step has had time to show its result
            if (tail >= 0 && ++tail == 30)
            {
                g_shotRequest.store(true);
                std::fprintf(stderr, "[padseq] done at %.1f s\n", seconds);
                tail = -1;
            }
            return;
        }
        Step &st = steps[at];
        uint32_t v;
        std::memcpy(&v, rdram + (word & 0x1FFFFFCu), 4);
        if (phase == 0)
        {
            count = (st.any || v == st.value) ? count + 1 : 0;
            if (count >= st.settle)
            {
                std::fprintf(stderr, "[padseq] step %zu: %s at %.1f s (word %x)\n", at, st.name.c_str(), seconds, v);
                g_shotRequest.store(true);
                g_scriptButtons.store(st.mask);
                g_scriptStick.store(st.stick);
                phase = 1;
                count = 0;
            }
        }
        else if (phase == 1 && ++count >= st.hold)
        {
            g_scriptButtons.store(0);
            g_scriptStick.store(0);
            g_shotRequest.store(true);
            phase = 2;
            count = 0;
        }
        else if (phase == 2 && ++count >= 5)
        {
            phase = 0;
            count = 0;
            if (++at >= steps.size())
                tail = 0;
        }
    }
}

uint16_t ps2portScriptButtons() { return g_scriptButtons.load(std::memory_order_relaxed); }

// Screenshots for the user's visual checks (2026-11-10). Asked for by Menu + L1 on the device, by `kill -USR1 PID` anywhere, by
// Enter in the terminal the PC runner was started from (it has no window and no keys), or at the seconds listed in
// PS2PORT_SHOT_AT=T1,T2,... The main thread copies the frame it shows next (a few ms, once); a thread of its own writes
// DIR/ps2port-m<game mode>-<uptime>s-f<game frame>.png, DIR being PS2PORT_SHOT_DIR, else /userdata/screenshots on the device and
// private/screenshots on the PC. Never on the EE thread.
void ps2portRequestShot() { g_userShot.store(true, std::memory_order_relaxed); }

namespace
{
    void shotSignal(int) { g_userShot.store(true, std::memory_order_relaxed); }

    const char *shotDir()
    {
        static const std::string dir = std::getenv("PS2PORT_SHOT_DIR") ? std::getenv("PS2PORT_SHOT_DIR")
                                       : std::getenv("PS2PORT_DISPLAY") ? "/userdata/screenshots" : "private/screenshots";
        return dir.c_str();
    }

    // main thread, every shown frame: true once per request
    bool shotWanted(double seconds)
    {
        static std::vector<double> at;
        static size_t next = 0;
        static const bool ready = []()
        {
            struct sigaction sa = {};
            sa.sa_handler = shotSignal;
            sigaction(SIGUSR1, &sa, nullptr);
            if (const char *env = std::getenv("PS2PORT_SHOT_AT"))
                for (const char *p = env; *p;)
                {
                    char *end = nullptr;
                    const double t = std::strtod(p, &end);
                    if (end == p)
                        break;
                    at.push_back(t);
                    p = *end ? end + 1 : end;
                }
            if (!std::getenv("PS2PORT_DISPLAY") && isatty(0))
                std::thread([]()
                            {
                                for (int c; (c = std::getchar()) != EOF;)
                                    if (c == '\n')
                                        g_userShot.store(true, std::memory_order_relaxed);
                            }).detach();
            return true;
        }();
        (void)ready;
        if (next < at.size() && seconds >= at[next])
        {
            ++next;
            g_userShot.store(true, std::memory_order_relaxed);
        }
        return g_userShot.exchange(false, std::memory_order_relaxed);
    }

    void saveUserShot(std::vector<uint8_t> &&pixels, uint32_t w, uint32_t h, uint32_t mode, double seconds)
    {
        if (pixels.empty() || w == 0u || h == 0u)
            return;
        ::mkdir(shotDir(), 0755);
        const unsigned long long frame = g_frameTotal.load(std::memory_order_relaxed);
        char name[512];
        std::snprintf(name, sizeof(name), "%s/ps2port-m%u-%05.0fs-f%llu.png", shotDir(), mode, seconds, frame);
        std::fprintf(stderr, "[shot] frame %llu mode %u uptime %.1f s -> %s\n", frame, mode, seconds, name);
        std::thread([p = std::move(pixels), w, h, path = std::string(name)]()
                    {
                        if (!ps2portpng::write(path.c_str(), p.data(), w, h, p.size() / h))
                            std::fprintf(stderr, "[shot] cannot write %s\n", path.c_str());
                    }).detach();
    }
}

// The PC runner's present loop (ps2_runtime.cpp, no display of ours): whether to save the frame it shows, and the saving
namespace
{
    void sampling(double seconds); // below
}
// PS2PORT_STACKS=S1,S2,...: at those seconds, every thread whose name starts with PS2PORT_STACKS_THREAD (default "Game") prints its
// host call stack (addresses; scripts/stack_symbols.py names them). For "where does a blocked thread wait" when there is no debugger.
namespace
{
    void stackSignal(int)
    {
        void *frames[48];
        const int n = backtrace(frames, 48);
        char line[48 * 20 + 64]; // one write, so two threads' stacks do not interleave
        int len = std::snprintf(line, sizeof(line), "[stack] tid %ld:", static_cast<long>(syscall(SYS_gettid)));
        for (int i = 0; i < n && len < static_cast<int>(sizeof(line)) - 24; ++i)
            len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " %p", frames[i]);
        line[len++] = '\n';
        (void)!write(2, line, static_cast<size_t>(len));
    }

    void stackDumps(double seconds)
    {
        static const char *when = std::getenv("PS2PORT_STACKS");
        static size_t next = 0;
        static std::vector<double> times;
        if (!when)
            return;
        if (times.empty() && next == 0)
        {
            for (const char *q = when; *q;)
            {
                char *end = nullptr;
                const double t = std::strtod(q, &end);
                if (end == q)
                    break;
                times.push_back(t);
                q = *end ? end + 1 : end;
            }
            void *warm[4];
            (void)backtrace(warm, 4); // loads libgcc before any signal
            signal(SIGUSR2, stackSignal);
            if (FILE *maps = std::fopen("/proc/self/maps", "rb"))
            {
                char row[512];
                if (std::fgets(row, sizeof(row), maps))
                    std::fprintf(stderr, "[stack] map %s", row); // the runner's load address, for the symbol names
                while (std::fgets(row, sizeof(row), maps))
                    if (std::strstr(row, "libc.so") && std::strstr(row, " 00000000 "))
                        std::fprintf(stderr, "[stack] libc %s", row); // and libc's
                std::fclose(maps);
            }
        }
        if (next >= times.size() || seconds < times[next])
            return;
        ++next;
        const char *prefix = std::getenv("PS2PORT_STACKS_THREAD") ? std::getenv("PS2PORT_STACKS_THREAD") : "Game";
        if (DIR *dir = opendir("/proc/self/task"))
        {
            while (dirent *e = readdir(dir))
            {
                if (e->d_name[0] == '.')
                    continue;
                char path[96], name[32] = {};
                std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
                if (FILE *f = std::fopen(path, "rb"))
                {
                    if (!std::fgets(name, sizeof(name), f))
                        name[0] = 0;
                    std::fclose(f);
                }
                if (std::strncmp(name, prefix, std::strlen(prefix)) == 0)
                    syscall(SYS_tgkill, getpid(), std::atoi(e->d_name), SIGUSR2);
            }
            closedir(dir);
        }
    }
}

// (the PC runner also samples threads from here, PS2PORT_SAMPLE: no display loop of ours runs there)
bool ps2portShotWanted(double seconds)
{
    sampling(seconds);
    stackDumps(seconds);
    return shotWanted(seconds);
}
void ps2portSaveShot(const uint8_t *rgba, uint32_t w, uint32_t h, const uint8_t *rdram, double seconds)
{
    uint32_t mode = 0;
    std::memcpy(&mode, rdram + 0x38c194u, 4);
    saveUserShot(std::vector<uint8_t>(rgba, rgba + static_cast<size_t>(w) * h * 4u), w, h, mode, seconds);
}
uint32_t ps2portScriptStick() { return g_scriptStick.load(std::memory_order_relaxed); }

void ps2portDrawThrottle(); // ps2_memory.cpp (private runtime)
void ps2portFrameMark();    // the same: frame sync, the game's next BGCOLOR write is its flip
void ps2portRealtimeHold(bool hold); // EeScheduler.cpp (private runtime)
namespace ps2x::iop::detail // the private IOP: its thread follows real time except while the game loads; IOP cycles run ahead
{
    void ps2portIopRealtimeHold(bool hold) __attribute__((weak));
    uint64_t ps2portIopRealtimeExtra() __attribute__((weak));
}

void ps2portPollFailed(uint32_t returnAddress, const uint8_t *rdram)
{
    if (returnAddress != 0x2cd6c8u)
        return;
    // the game waits for its vsync here: where it waits for the display too when the display is too far behind
    ps2portDrawThrottle();
    ps2portFrameMark();
    {
        // the real-time clock (PS2PORT_REALTIME=1) stands still while the game loads
        uint32_t mode = 0;
        std::memcpy(&mode, rdram + 0x38c194u, 4);
        ps2portRealtimeHold(mode == 0u);
        // the IOP may run ahead of the EE in play (2), pause (3), Remembrall (4) and hint boxes (5); not while the game loads (0),
        // saves (6) or plays a cutscene (9), which stream from the disc with the IOP in step
        if (ps2x::iop::detail::ps2portIopRealtimeHold)
            ps2x::iop::detail::ps2portIopRealtimeHold(!(mode >= 2u && mode <= 5u));
    }
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
    // PS2PORT_FRAME_LOG=FILE: every game frame, milliseconds since the first and the frame's length (scripts/frame_stats.py),
    // then the game's vblank count (0x328490, its game time: 60 a second on a PS2) and the vblanks its last frame took (0x3287F0)
    static FILE *frameLog = std::getenv("PS2PORT_FRAME_LOG") ? std::fopen(std::getenv("PS2PORT_FRAME_LOG"), "w") : nullptr;
    if (frameLog && g_lastMarkNs != 0)
    {
        uint32_t vblanks = 0, last = 0;
        if (rdram)
        {
            std::memcpy(&vblanks, rdram + 0x328490u, 4);
            std::memcpy(&last, rdram + 0x3287F0u, 4);
        }
        std::fprintf(frameLog, "%.1f %.2f %u %u\n", (now - g_processStartNs) / 1e6, (now - g_lastMarkNs) / 1e6, vblanks, last);
    }
    // PS2PORT_WATCH=ITEM,ITEM,...: guest words printed ("[watch]" on stderr) at a game frame's end whenever one changes.
    // ITEM is 0xADDR (the word there), *0xADDR or *0xADDR+0xOFF (the word at the pointer stored at ADDR, plus OFF), or btn:
    // a mask of the game's button actions whose "pressed" halfword is set (table *(*0x38c130 + 0x10), 0x18 bytes an entry,
    // halfword at +0x10, entries 0-31; the pause check 0x250998 reads entry *0x3956b0).
    static std::vector<std::pair<int, std::pair<uint32_t, uint32_t>>> watch; // kind (0 word, 1 pointer, 2 btn), address, offset
    static bool watchParsed = false;
    static std::vector<uint32_t> watchLast;
    if (!watchParsed)
    {
        watchParsed = true;
        if (const char *w = std::getenv("PS2PORT_WATCH"))
        {
            std::string s(w);
            size_t p = 0;
            while (p <= s.size())
            {
                size_t e = s.find(',', p);
                if (e == std::string::npos)
                    e = s.size();
                std::string item = s.substr(p, e - p);
                if (item == "btn")
                    watch.push_back({2, {0, 0}});
                else if (!item.empty())
                {
                    const bool ptr = item[0] == '*';
                    const std::string body = ptr ? item.substr(1) : item;
                    const size_t plus = body.find('+');
                    const uint32_t addr = static_cast<uint32_t>(std::strtoul(body.substr(0, plus).c_str(), nullptr, 0));
                    const uint32_t off = plus == std::string::npos ? 0 : static_cast<uint32_t>(std::strtoul(body.substr(plus + 1).c_str(), nullptr, 0));
                    watch.push_back({ptr ? 1 : 0, {addr, off}});
                }
                p = e + 1;
            }
            watchLast.assign(watch.size(), 0xDEADBEEFu);
            std::fprintf(stderr, "[watch] %s\n", w);
        }
    }
    // PS2PORT_WATCH_EVERY=N: compare only every N game frames (for words that change every frame)
    static const uint64_t watchEvery = std::getenv("PS2PORT_WATCH_EVERY") ? std::max<uint64_t>(1u, std::strtoull(std::getenv("PS2PORT_WATCH_EVERY"), nullptr, 10)) : 1u;
    static uint64_t watchCalls = 0;
    if (!watch.empty() && rdram && (watchCalls++ % watchEvery) == 0)
    {
        auto rd32 = [rdram](uint32_t a) -> uint32_t {
            a &= 0x1FFFFFFCu;
            if (a >= 0x2000000u)
                return 0xFFFFFFFFu;
            uint32_t v;
            std::memcpy(&v, rdram + a, 4);
            return v;
        };
        bool changed = false;
        std::vector<uint32_t> cur(watch.size());
        for (size_t i = 0; i < watch.size(); ++i)
        {
            const auto &it = watch[i];
            if (it.first == 0)
                cur[i] = rd32(it.second.first);
            else if (it.first == 1)
                cur[i] = rd32(rd32(it.second.first) + it.second.second);
            else
            {
                const uint32_t base = rd32(rd32(0x38c130u) + 0x10u);
                uint32_t mask = 0;
                for (uint32_t k = 0; k < 32 && base != 0 && base < 0x2000000u; ++k)
                    if ((rd32(base + k * 0x18u + 0x10u) & 0xFFFFu) != 0)
                        mask |= 1u << k;
                cur[i] = mask;
            }
            changed |= cur[i] != watchLast[i];
        }
        if (changed)
        {
            std::fprintf(stderr, "[watch] %.1f f%llu", (now - g_processStartNs) / 1e9, static_cast<unsigned long long>(g_frameMarks.load(std::memory_order_relaxed)));
            for (uint32_t v : cur)
                std::fprintf(stderr, " %x", v);
            std::fputc('\n', stderr);
            watchLast = cur;
        }
    }
    padSequence(rdram, (now - g_processStartNs) / 1e9);
    g_lastMarkNs = now;
    g_frameMarks.fetch_add(1, std::memory_order_relaxed);
    g_frameTotal.fetch_add(1, std::memory_order_relaxed);
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

// runner/pad_sdl.cpp: the controller, on SDL, from this (the main) thread; runner/audio_sdl.cpp: the sound
void ps2portPadInit();
void ps2portPadPoll();
void ps2portAudioInit();
void ps2portAudioStats(double &mixerRate, uint64_t &underruns);
// ps2_memory.cpp (private runtime): the EE waits for a latch when stage B's recorded backlog is too large
bool ps2portDrawWantsLatch();
void ps2portDrawLatched();
void ps2portDrawThrottle();
// frame sync (2026-11-10): the game's flips, counted; the last one recorded in full; that its latch is done (the game goes on)
bool ps2portFrameSyncOn();
void ps2portFrameDisplayIdle(bool idle);
uint64_t ps2portFrameRecorded();
void ps2portFrameRegs(uint64_t flip, uint64_t out[7]);
void ps2portFrameShown(uint64_t flip);
uint64_t ps2portFrameSyncCounts(int which);

bool ps2portDisplayInit(PS2Runtime &runtime)
{
    if (!std::getenv("PS2PORT_DISPLAY"))
        return false;
    if (!gsplayGlInit())
    {
        std::fprintf(stderr, "[display] no GL: staying with the software GS\n");
        return false;
    }
    ps2portPadInit();
    ps2portAudioInit();
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

    ps2portPadPoll();
    // Frame sync: one latch for each whole frame the game flipped, as soon as it is recorded. Without it (PS2PORT_FRAME_SYNC=0), and
    // while the game does not flip (loads), each call is one guest vsync's frame; a swap may already have taken a vsync or more.
    static const bool frameSync = ps2portFrameSyncOn();
    static uint64_t takenFlip = 0;
    static double lastFlipAt = -1e9;
    const double waitStart = nowMs();
    uint64_t tick = runtime.eeScheduler().currentVSyncTick();
    uint64_t recorded = ps2portFrameRecorded();
    // ... or at once when stage B waits for a latch (its backlog is full; the EE may be waiting on it, so no vsync would come)
    const auto due = [&]()
    {
        if (ps2portDrawWantsLatch())
            return true;
        if (!frameSync)
            return tick != last;
        return recorded != takenFlip || (tick != last && nowMs() - lastFlipAt > 100.0);
    };
    for (int spins = 1; !due() && nowMs() - waitStart < 50.0; ++spins)
    {
        ps2portFrameDisplayIdle(true); // nothing to show: the game need not wait for the display at its flip
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        if ((spins & 7) == 0)
            ps2portPadPoll(); // every 4 ms or so: a short tap is not missed between two frames
        tick = runtime.eeScheduler().currentVSyncTick();
        recorded = ps2portFrameRecorded();
    }
    if (!due())
        return;
    ps2portFrameDisplayIdle(false);
    // Present with the display registers of the flip this latch takes (2026-11-10: menus and loading screens blinked: by the
    // time the main thread latched, the game had often flipped again, and the live DISPFB named its next, half-drawn buffer)
    static uint64_t regsMoved = 0;
    if (frameSync && recorded != takenFlip && recorded != 0u)
    {
        uint64_t regs[7];
        ps2portFrameRegs(recorded, regs);
        const GSRegisters &live = runtime.memory().gs();
        if (live.dispfb1 != regs[2] || live.dispfb2 != regs[4])
            ++regsMoved;
        runtime.gs().ps2portSetPresentationRegs(regs);
    }
    else if (frameSync && recorded == takenFlip)
        runtime.gs().ps2portSetPresentationRegs(nullptr); // no new flip (loads): the live registers, as before
    if (recorded != takenFlip)
        lastFlipAt = nowMs();
    takenFlip = recorded;
    const uint64_t passed = last == ~0ull ? 1u : tick - last;
    last = tick;
    ++shown;

    const bool shot = frameDir && ((frameEvery != 0u && shown % frameEvery == 0u) || g_shotRequest.exchange(false));
    const bool userShot = shotWanted(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (shot || userShot)
        g_gles->setCaptureNextPresent(true);
    const double t0 = nowMs();
    runtime.gs().latchHostPresentationFrame();
    ps2portDrawLatched();
    ps2portFrameShown(takenFlip);
    const double t1 = nowMs();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    sampling(seconds);
    if (shot || userShot)
        g_gles->setCaptureNextPresent(false);
    if (shot)
        writeShot(runtime, frameDir, shown, seconds);
    if (userShot)
    {
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0, mode = 0;
        std::memcpy(&mode, runtime.memory().getRDRAM() + 0x38c194u, 4);
        if (runtime.gs().copyLatchedHostPresentationFrame(pixels, w, h))
            saveUserShot(std::move(pixels), w, h, mode, seconds);
    }
    const double t2 = nowMs();
    gsplayGlSwap();
    const double t3 = nowMs();

    static const bool lagLog = wantStats || std::getenv("PS2PORT_LAG_LOG") != nullptr;
    if (lagLog)
    {
        static double lagStart = t3;
        static uint64_t before[6] = {}, latches = 0, regsMovedBefore = 0;
        ++latches;
        if (t3 - lagStart >= 5000.0)
        {
            uint64_t c[6];
            for (int i = 0; i < 6; ++i)
                c[i] = ps2portFrameSyncCounts(i);
            const double span = (t3 - lagStart) / 1000.0;
            const uint64_t flipped = c[0] - before[0], lagged = c[5] - before[5];
            std::fprintf(stderr, "[lag] t=%.0f sync %s: game flips/s %.1f, latches/s %.1f; flip to replayed %.1f ms avg, %.1f max; game waited %llu times, %.1f ms a frame, %llu time-outs; display registers had moved on at %llu latches\n",
                         seconds, frameSync ? "on" : "off", flipped / span, latches / span, lagged ? (c[4] - before[4]) / 1e6 / lagged : 0.0,
                         ps2portFrameSyncCounts(6) / 1e6, (unsigned long long)(c[1] - before[1]),
                         flipped ? (c[2] - before[2]) / 1e6 / flipped : 0.0, (unsigned long long)(c[3] - before[3]),
                         (unsigned long long)(regsMoved - regsMovedBefore));
            regsMovedBefore = regsMoved;
            std::memcpy(before, c, sizeof before);
            latches = 0;
            lagStart = t3;
        }
    }
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
            static uint64_t gsBefore[8] = {};
            uint64_t gs[8];
            for (int k = 0; k < 8; ++k)
                gs[k] = ps2portGsThreadCounts(k);
            std::fprintf(stderr, "[m5t] t=%.0f per s: gs priv r %.0f w %.0f; vif1 r %.0f w %.0f; gif regs %.0f; channel reads %.0f; vu1 mem %.0f; fifo %.0f; kicks vif1 %.0f gif %.0f; vif1 irqs %.0f; gs thread jobs %.0f handed %.0f waits %.0f; in jobs %.0f ms of which waiting for the EE %.0f ms; stage B busy %.0f ms, queue full %.0f; D1_CHCR reads busy %.0f\n", seconds,
                         per[0], per[1], per[2], per[3], per[4], per[5], per[6], per[7], per[8], per[9], per[10], (gs[2] - gsBefore[2]) * 1000.0 / span,
                         (gs[0] - gsBefore[0]) * 1000.0 / span, (gs[1] - gsBefore[1]) * 1000.0 / span, (gs[4] - gsBefore[4]) / 1e6 * 1000.0 / span,
                         (gs[3] - gsBefore[3]) / 1e6 * 1000.0 / span, (gs[5] - gsBefore[5]) / 1e6 * 1000.0 / span, (gs[6] - gsBefore[6]) * 1000.0 / span, (gs[7] - gsBefore[7]) * 1000.0 / span);
            std::memcpy(gsBefore, gs, sizeof(gs));
            static uint64_t gatherBefore[2] = {};
            std::fprintf(stderr, "[m5g] t=%.0f DMA chains gathered %.2f MB/s, of which already copied in the same chain %.2f MB/s\n", seconds,
                         (g_ps2portGatherBytes[0] - gatherBefore[0]) * 1000.0 / span / 1048576.0, (g_ps2portGatherBytes[1] - gatherBefore[1]) * 1000.0 / span / 1048576.0);
            std::memcpy(gatherBefore, g_ps2portGatherBytes, sizeof(gatherBefore));
        }
        std::fprintf(stderr, "[m5m] t=%.0f tick/s %.1f drawn/s %.1f; palettes %.0f, targets %.1f MB, depth %.0f (%.1f MB), textures %.0f (%.1f MB)\n", seconds, (tick - tickBefore) * 1000.0 / span, frames * 1000.0 / span,
                     gpu[12], gpu[13] / 1048576.0, gpu[14], gpu[15] / 1048576.0, gpu[16], gpu[17] / 1048576.0);
        {
            // malloc's own count (main arena and the large blocks it maps): whether a growth is the heap's
            const struct mallinfo mi = mallinfo();
            double mixerRate = 0.0;
            uint64_t underruns = 0;
            ps2portAudioStats(mixerRate, underruns);
            static uint64_t iopExtraBefore = 0;
            const uint64_t iopExtra = ps2x::iop::detail::ps2portIopRealtimeExtra ? ps2x::iop::detail::ps2portIopRealtimeExtra() : 0u;
            const double iopAheadMs = (iopExtra - iopExtraBefore) / 36864.0 / (span / 1000.0);
            iopExtraBefore = iopExtra;
            std::fprintf(stderr, "[m5h] t=%.0f heap in use %.1f MB (main arena %.1f MB, mapped blocks %.1f MB in %d); stage B waited for a latch %llu times; "
                                 "sound mixed at %.0f frames/s, output ran dry %llu times; IOP ahead of the EE by %.0f ms a second\n", seconds,
                         (static_cast<unsigned>(mi.uordblks) + static_cast<unsigned>(mi.hblkhd)) / 1048576.0, static_cast<unsigned>(mi.arena) / 1048576.0,
                         static_cast<unsigned>(mi.hblkhd) / 1048576.0, mi.hblks, static_cast<unsigned long long>(ps2portGsThreadCounts(8)),
                         mixerRate, static_cast<unsigned long long>(underruns), iopAheadMs);
        }
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
