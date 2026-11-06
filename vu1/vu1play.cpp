// Replays one frame's VIF1 stream through VU1, with no game running.
//   vu1play DIR [--mode interp|native|check] [--passes N]
// DIR comes from scripts/vu1_extract.py. VIF is the runtime's own code. The first pass leaves the VU1 registers
// as a real frame would, since the savestate's own aren't used; later passes are the ones measured and compared.
// check runs the interpreter and the translated code on separate copies of VU1 memory, call by call, and compares
// all registers, VU1 data memory and the GIF packets after each one.
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "vu1_rt.h"

#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern uint64_t g_egVu1Notes[2048];
extern uint64_t g_egVu1Counts[2048];
extern bool g_ps2portVifGeneric;

namespace
{
    double nowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::vector<uint8_t> readFile(const std::string &path)
    {
        std::vector<uint8_t> out;
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            std::fprintf(stderr, "vu1play: cannot open %s\n", path.c_str());
            std::exit(1);
        }
        std::fseek(f, 0, SEEK_END);
        out.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(out.data(), 1, out.size(), f) != out.size())
            std::exit(1);
        std::fclose(f);
        return out;
    }

    uint64_t fnv(const uint8_t *data, size_t size, uint64_t h = 1469598103934665603ull)
    {
        for (size_t i = 0; i < size; ++i)
            h = (h ^ data[i]) * 1099511628211ull;
        return h;
    }

    struct Packets
    {
        uint64_t count = 0, bytes = 0, hash = 1469598103934665603ull;
        std::vector<uint8_t> recent;
        void add(const uint8_t *data, uint32_t size, bool keep)
        {
            ++count;
            bytes += size;
            hash = fnv(data, size, hash);
            if (keep)
                recent.insert(recent.end(), data, data + size);
        }
    };

    // Vertices in a stream of GIF packets, counting XYZ registers
    uint64_t countVertices(const uint8_t *data, uint32_t size)
    {
        uint64_t vertices = 0;
        uint32_t pos = 0;
        while (pos + 16u <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + pos, 8);
            std::memcpy(&hi, data + pos + 8, 8);
            pos += 16u;
            const uint32_t nloop = lo & 0x7FFFu;
            const uint32_t flg = (lo >> 58) & 3u;
            uint32_t nreg = (lo >> 60) & 15u;
            if (nreg == 0u)
                nreg = 16u;
            if (flg == 0u)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint32_t reg = (hi >> (r * 4u)) & 15u;
                    if (reg == 4u || reg == 5u || reg == 12u || reg == 13u)
                        vertices += nloop;
                }
                pos += nloop * nreg * 16u;
            }
            else if (flg == 1u)
                pos += ((nloop * nreg + 1u) & ~1u) * 8u;
            else
                pos += nloop * 16u;
        }
        return vertices;
    }
}

int main(int argc, char **argv)
{
    std::string dir, mode = "interp";
    int passes = 2;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--mode") && i + 1 < argc)
            mode = argv[++i];
        else if (!std::strcmp(argv[i], "--passes") && i + 1 < argc)
            passes = std::atoi(argv[++i]);
        else
            dir = argv[i];
    }
    if (dir.empty())
    {
        std::fprintf(stderr, "usage: vu1play DIR [--mode interp|native|check] [--passes N]\n");
        return 2;
    }

    if (const char *core = std::getenv("VU1PLAY_CORE"))
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(std::atoi(core), &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            std::perror("vu1play: core");
    }

    const std::vector<uint8_t> stream = readFile(dir + "/stream.bin");
    const std::vector<uint8_t> data0 = readFile(dir + "/data.bin");
    const std::vector<uint8_t> micro = readFile(dir + "/micro.bin");
    const std::vector<uint8_t> regsFile = readFile(dir + "/vifregs.bin");
    uint32_t vifRegs[18];
    std::memcpy(vifRegs, regsFile.data(), sizeof(vifRegs));

    static PS2Memory mem;
    if (!mem.initialize())
    {
        std::fprintf(stderr, "vu1play: no memory\n");
        return 1;
    }
    std::memcpy(mem.getVU1Code(), micro.data(), micro.size());

    static GS gs;
    static VU1Interpreter interp;
    static std::vector<uint8_t> nativeData(data0.size());
    static EgVu1State nativeState;
    static Packets interpPackets, nativePackets;
    static uint64_t calls = 0, mismatches = 0, cycles = 0, vertices = 0, misses = 0;
    static uint64_t diffCounts[6] = {};
    static uint64_t dataHash = 1469598103934665603ull;
    g_ps2portVifGeneric = std::getenv("VU1PLAY_VIF_GENERIC") != nullptr;
    static double vuMs = 0;
    static uint32_t reported = 0;
    static std::vector<uint8_t> nativeKick;
    static bool measuring = false;

    const bool runInterp = mode != "native";
    const bool runNative = mode != "interp";

    mem.setGifPacketCallback([&](const uint8_t *data, uint32_t size)
                             { interpPackets.add(data, size, mode == "check"); vertices += countVertices(data, size); });
    // On the last pass of a timing run a packet is only copied out, as a queue to the GS thread would.
    static bool timing = false, keepKicks = mode == "check", countKicks = mode == "native";
    static std::vector<uint8_t> ring(1u << 21);
    static size_t ringPos = 0;
    egVu1SetKickSink([](const uint8_t *data, uint32_t size)
                     {
                         if (timing)
                         {
                             if (ringPos + size > ring.size())
                                 ringPos = 0;
                             std::memcpy(ring.data() + ringPos, data, size);
                             ringPos += size;
                             return;
                         }
                         nativePackets.add(data, size, keepKicks);
                         if (countKicks)
                             vertices += countVertices(data, size);
                     });

    mem.setVu1MscalCallback(
        [&](uint32_t startPC, uint32_t top, uint32_t itop)
        {
            ++calls;
            uint8_t *data = mem.getVU1Data();
            if (runNative && !runInterp)
            {
                if (!timing)
                    dataHash = fnv(data, data0.size(), dataHash);
                const double t0 = nowMs();
                if (!egVu1Run(nativeState, data, startPC / 8u, top, itop))
                    ++misses;
                vuMs += nowMs() - t0;
                return;
            }
            if (runNative)
            {
                // Same input for both: what VIF just unpacked goes to the translated code's copy too.
                std::memcpy(nativeData.data(), data, nativeData.size());
                interpPackets.recent.clear();
                nativePackets.recent.clear();
                if (!egVu1Run(nativeState, nativeData.data(), startPC / 8u, top, itop))
                    ++misses;
            }
            const uint64_t before = interp.state().cycles;
            const double t0 = nowMs();
            interp.execute(mem.getVU1Code(), static_cast<uint32_t>(micro.size()), data, static_cast<uint32_t>(data0.size()), gs, &mem, startPC, top, itop, 10000000u);
            vuMs += nowMs() - t0;
            cycles += interp.state().cycles - before;
            if (runNative)
            {
                std::string diff;
                const VU1State &a = interp.state();
                for (int r = 1; r < 16; ++r)
                    if (static_cast<uint16_t>(a.vi[r]) != static_cast<uint16_t>(nativeState.vi[r]))
                        diff += " vi" + std::to_string(r);
                for (int r = 1; r < 32; ++r)
                    if (std::memcmp(a.vf[r], nativeState.vf[r], 16) != 0)
                        diff += " vf" + std::to_string(r);
                if (std::memcmp(a.acc, nativeState.acc, 16) != 0)
                    diff += " acc";
                if (std::memcmp(&a.q, &nativeState.q, 4) != 0)
                    diff += " q";
                if (std::memcmp(data, nativeData.data(), nativeData.size()) != 0)
                    diff += " data";
                if (interpPackets.recent != nativePackets.recent)
                    diff += " gif";
                if (a.clip != nativeState.clip)
                    diff += " clip";
                if (std::memcmp(&a.p, &nativeState.p, 4) != 0)
                    diff += " p";
                if (std::memcmp(&a.i, &nativeState.i, 4) != 0)
                    diff += " i";
                if (!diff.empty())
                {
                    ++mismatches;
                    if (measuring && diff.find(" gif") != std::string::npos)
                        ++diffCounts[0];
                    if (measuring && diff.find(" data") != std::string::npos)
                        ++diffCounts[1];
                    if (measuring && diff.find(" vf") != std::string::npos)
                        ++diffCounts[2];
                    if (reported < 14u && measuring && (diff.find(" gif") != std::string::npos || std::getenv("VU1PLAY_ALLDIFF")))
                    {
                        ++reported;
                        std::printf("call %llu pc %#x top %#x: differs in%s\n", (unsigned long long)calls, startPC / 8u, top, diff.c_str());
                        if (interpPackets.recent != nativePackets.recent)
                        {
                            const size_t ni = interpPackets.recent.size(), nn = nativePackets.recent.size();
                            size_t o = 0;
                            while (o + 16 <= ni && o + 16 <= nn && std::memcmp(interpPackets.recent.data() + o, nativePackets.recent.data() + o, 16) == 0)
                                o += 16;
                            std::printf("  gif sizes %zu %zu, first difference at qword %zu", ni / 16, nn / 16, o / 16);
                            if (o + 16 <= ni && o + 16 <= nn)
                            {
                                uint32_t x[4], y[4];
                                std::memcpy(x, interpPackets.recent.data() + o, 16);
                                std::memcpy(y, nativePackets.recent.data() + o, 16);
                                std::printf(": interp %08x %08x %08x %08x native %08x %08x %08x %08x", x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                            }
                            size_t count = 0;
                            for (size_t q = 0; q + 16 <= ni && q + 16 <= nn; q += 16)
                                count += std::memcmp(interpPackets.recent.data() + q, nativePackets.recent.data() + q, 16) != 0;
                            std::printf(" (%zu qwords differ)", count);
                            std::putchar(10);
                        }
                        for (int r = 1; r < 32 && reported <= 4u; ++r)
                            if (std::memcmp(a.vf[r], nativeState.vf[r], 16) != 0)
                            {
                                uint32_t x[4], y[4];
                                std::memcpy(x, a.vf[r], 16);
                                std::memcpy(y, nativeState.vf[r], 16);
                                std::printf("  vf%d interp %08x %08x %08x %08x native %08x %08x %08x %08x\n", r, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                                break;
                            }
                        if (reported <= 4u)
                            for (size_t o = 0; o < nativeData.size(); o += 16)
                                if (std::memcmp(data + o, nativeData.data() + o, 16) != 0)
                                {
                                    uint32_t x[4], y[4];
                                    std::memcpy(x, data + o, 16);
                                    std::memcpy(y, nativeData.data() + o, 16);
                                    std::printf("  data qword %#zx interp %08x %08x %08x %08x native %08x %08x %08x %08x\n", o / 16, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                                    break;
                                }
                    }
                }
                // Carry on from the reference, so that one wrong call shows as one.
                for (int r = 0; r < 16; ++r)
                    nativeState.vi[r] = static_cast<uint16_t>(a.vi[r]);
                std::memcpy(nativeState.vf, a.vf, sizeof(nativeState.vf));
                std::memcpy(nativeState.acc, a.acc, 16);
                nativeState.q = a.q;
                nativeState.p = a.p;
                nativeState.i = a.i;
                nativeState.clip = a.clip;
            }
        });

    nativeState.q = 1.0f;
    double passMs = 0, passVuMs = 0;
    uint64_t keptCycles = 0, keptVertices = 0, keptCount = 0, keptBytes = 0, keptHash = 0;
    std::vector<double> vuTimes, allTimes;
    for (int pass = 0; pass < passes; ++pass)
    {
        measuring = pass == passes - 1;
        timing = pass > 0 && mode != "check";
        if (pass == 1)
        {
            const Packets &done = mode == "native" ? nativePackets : interpPackets;
            keptCycles = cycles;
            keptVertices = vertices;
            keptCount = done.count;
            keptBytes = done.bytes;
            keptHash = done.hash;
        }
        std::memcpy(mem.getVU1Data(), data0.data(), data0.size());
        VIFRegisters &v = mem.vif1_regs;
        v.stat = vifRegs[0];
        v.cycle = vifRegs[1];
        v.mode = vifRegs[2];
        v.mask = vifRegs[3];
        v.itops = vifRegs[4];
        v.base = vifRegs[5];
        v.ofst = vifRegs[6];
        v.tops = vifRegs[7];
        v.itop = vifRegs[8];
        v.top = vifRegs[9];
        std::memcpy(v.row, vifRegs + 10, 16);
        std::memcpy(v.col, vifRegs + 14, 16);
        calls = mismatches = cycles = vertices = 0;
        interpPackets = Packets{};
        nativePackets = Packets{};
        vuMs = 0;
        const double t0 = nowMs();
        mem.processVIF1Data(stream.data(), static_cast<uint32_t>(stream.size()));
        passMs = nowMs() - t0;
        passVuMs = vuMs;
        if (timing)
        {
            vuTimes.push_back(vuMs);
            allTimes.push_back(passMs);
        }
    }

    if (mode == "check")
    {
        const Packets &done = interpPackets;
        keptCycles = cycles;
        keptVertices = vertices;
        keptCount = done.count;
        keptBytes = done.bytes;
        keptHash = done.hash;
    }
    std::printf("%s: %llu calls, %llu VU cycles, %llu vertices, %llu GIF packets, %.2f MB, hash %016llx, entries missed %llu\n", mode.c_str(), (unsigned long long)calls,
                (unsigned long long)keptCycles, (unsigned long long)keptVertices, (unsigned long long)keptCount, keptBytes / 1048576.0, (unsigned long long)keptHash, (unsigned long long)misses);
    std::printf("VU data memory hash at each call %016llx\n", (unsigned long long)dataHash);
    std::printf("last pass ms: total %.2f, VU %.2f, VIF and the rest %.2f\n", passMs, passVuMs, passMs - passVuMs);
    if (!vuTimes.empty())
    {
        std::sort(vuTimes.begin(), vuTimes.end());
        std::sort(allTimes.begin(), allTimes.end());
        const double vuMid = vuTimes[vuTimes.size() / 2];
        std::printf("over %zu passes, ms: VU min %.2f median %.2f max %.2f; with VIF min %.2f median %.2f; ns per vertex %.0f\n", vuTimes.size(), vuTimes.front(), vuMid, vuTimes.back(),
                    allTimes.front(), allTimes[allTimes.size() / 2], keptVertices ? vuMid * 1e6 / keptVertices : 0.0);
    }
    if (keptVertices)
        std::printf("VU cycles per vertex %.1f, ns per vertex %.1f\n", static_cast<double>(keptCycles) / keptVertices, passVuMs * 1e6 / keptVertices);
    if (const char *where = std::getenv("VU1PLAY_COUNTS"))
    {
        FILE *f = std::fopen(where, "wb");
        std::fwrite(g_egVu1Counts, 8, 2048, f);
        std::fclose(f);
    }
    if (std::getenv("VU1PLAY_NOTES"))
        for (int pc = 0; pc < 2048; ++pc)
            if (g_egVu1Notes[pc])
                std::printf("note %#x %llu\n", pc, (unsigned long long)g_egVu1Notes[pc]);
    if (runNative && runInterp)
        std::printf("check: %llu of %llu calls differ; in the GIF packets %llu, data %llu, vf %llu\n", (unsigned long long)mismatches, (unsigned long long)calls,
                    (unsigned long long)diffCounts[0], (unsigned long long)diffCounts[1], (unsigned long long)diffCounts[2]);
    return 0;
}
