// Plays a PCSX2 GS dump through the runtime's GS frontend, with no game running.
//   gsplay DUMP.gs [--out FILE.ppm] [--loops N]
// Writes the frame shown at the last vsync as a PPM. Without GSPLAY_GLES this is the software GS
// (the PC reference); with it, the GLES backend from gs_gles_backend.cpp.
#include "gsdump.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"

#ifdef GSPLAY_GLES
#include "gs_gles_backend.h"
#include "gsplay_gl.h"
#endif

#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // GSPLAY_SAMPLE=FILE: the program counter every millisecond of CPU time from the second loop on, as 64-bit addresses
    // (scripts/sample_profile.py maps them with nm). Addresses outside this binary are the GL driver or libc.
    constexpr size_t kMaxSamples = 1u << 20;
    uint64_t g_samples[kMaxSamples];
    volatile size_t g_sampleCount = 0;

    void onSampleTick(int, siginfo_t *, void *uc)
    {
        if (g_sampleCount >= kMaxSamples)
            return;
        const ucontext_t *u = static_cast<const ucontext_t *>(uc);
#if defined(__aarch64__)
        g_samples[g_sampleCount++] = u->uc_mcontext.pc;
#else
        g_samples[g_sampleCount++] = static_cast<uint64_t>(u->uc_mcontext.gregs[REG_RIP]);
#endif
    }

    void startSampling()
    {
        struct sigaction sa{};
        sa.sa_sigaction = onSampleTick;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        const itimerval tick{{0, 1000}, {0, 1000}};
        sigaction(SIGPROF, &sa, nullptr);
        setitimer(ITIMER_PROF, &tick, nullptr);
    }

    void stopSampling(const char *path)
    {
        const itimerval off{};
        setitimer(ITIMER_PROF, &off, nullptr);
        if (FILE *f = std::fopen(path, "wb"))
        {
            std::fwrite(g_samples, sizeof(uint64_t), g_sampleCount, f);
            std::fclose(f);
            std::printf("wrote %zu samples to %s\n", static_cast<size_t>(g_sampleCount), path);
        }
        // the libraries' addresses, for scripts/sample_modules.py
        const std::string mapsPath = std::string(path) + ".maps";
        FILE *in = std::fopen("/proc/self/maps", "rb");
        FILE *out = std::fopen(mapsPath.c_str(), "wb");
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
    }

    double nowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void loadPriv(GSRegisters &regs, const uint8_t *p)
    {
        regs.pmode = GsDump::read64(p + 0x00);
        regs.smode1 = GsDump::read64(p + 0x10);
        regs.smode2 = GsDump::read64(p + 0x20);
        regs.srfsh = GsDump::read64(p + 0x30);
        regs.synch1 = GsDump::read64(p + 0x40);
        regs.synch2 = GsDump::read64(p + 0x50);
        regs.syncv = GsDump::read64(p + 0x60);
        regs.dispfb1 = GsDump::read64(p + 0x70);
        regs.display1 = GsDump::read64(p + 0x80);
        regs.dispfb2 = GsDump::read64(p + 0x90);
        regs.display2 = GsDump::read64(p + 0xA0);
        regs.extbuf = GsDump::read64(p + 0xB0);
        regs.extdata = GsDump::read64(p + 0xC0);
        regs.extwrite = GsDump::read64(p + 0xD0);
        regs.bgcolor = GsDump::read64(p + 0xE0);
    }

    // Restores the frontend's registers from the dump's saved state. TRXDIR is left out on purpose:
    // writing it would start a transfer.
    void restoreRegisters(GS &gs, const GsDump &dump)
    {
        static const uint8_t envRegs[] = {GS_REG_PRIM, GS_REG_PRMODECONT, GS_REG_TEXCLUT, GS_REG_SCANMSK,
                                          GS_REG_TEXA, GS_REG_FOGCOL, GS_REG_DIMX, GS_REG_DTHE,
                                          GS_REG_COLCLAMP, GS_REG_PABE, GS_REG_BITBLTBUF};
        for (uint32_t i = 0; i < sizeof(envRegs); ++i)
            gs.writeRegister(envRegs[i], dump.env(i));
        gs.writeRegister(GS_REG_TRXPOS, dump.env(12));
        gs.writeRegister(GS_REG_TRXREG, dump.env(13));
        for (uint32_t c = 0; c < 2u; ++c)
        {
            const uint8_t d = static_cast<uint8_t>(c);
            gs.writeRegister(GS_REG_XYOFFSET_1 + d, dump.ctx(c, 0));
            gs.writeRegister(GS_REG_TEX1_1 + d, dump.ctx(c, 2));
            gs.writeRegister(GS_REG_CLAMP_1 + d, dump.ctx(c, 3));
            gs.writeRegister(GS_REG_MIPTBP1_1 + d, dump.ctx(c, 4));
            gs.writeRegister(GS_REG_MIPTBP2_1 + d, dump.ctx(c, 5));
            gs.writeRegister(GS_REG_SCISSOR_1 + d, dump.ctx(c, 6));
            gs.writeRegister(GS_REG_ALPHA_1 + d, dump.ctx(c, 7));
            gs.writeRegister(GS_REG_TEST_1 + d, dump.ctx(c, 8));
            gs.writeRegister(GS_REG_FBA_1 + d, dump.ctx(c, 9));
            gs.writeRegister(GS_REG_FRAME_1 + d, dump.ctx(c, 10));
            gs.writeRegister(GS_REG_ZBUF_1 + d, dump.ctx(c, 11));
            gs.writeRegister(GS_REG_TEX0_1 + d, dump.ctx(c, 1));
        }
        gs.writeRegister(GS_REG_RGBAQ, dump.vertexReg(0));
        gs.writeRegister(GS_REG_ST, dump.vertexReg(1));
        gs.writeRegister(GS_REG_UV, dump.vertexReg(2));
    }

    bool writePpm(const std::string &path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h, uint32_t stride)
    {
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> line(w * 3u);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *src = rgba.data() + y * stride;
            for (uint32_t x = 0; x < w; ++x)
            {
                line[x * 3u] = src[x * 4u];
                line[x * 3u + 1u] = src[x * 4u + 1u];
                line[x * 3u + 2u] = src[x * 4u + 2u];
            }
            std::fwrite(line.data(), 1, line.size(), f);
        }
        std::fclose(f);
        return true;
    }
}

int main(int argc, char **argv)
{
    std::string input, output;
    int loops = 1;
    int wantVsync = 0; // 0: the last vsync
    bool warm = false; // keep the backend and its caches between loops, as a running game would
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
            output = argv[++i];
        else if (!std::strcmp(argv[i], "--loops") && i + 1 < argc)
            loops = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--vsync") && i + 1 < argc)
            wantVsync = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--warm"))
            warm = true;
        else
            input = argv[i];
    }
    if (input.empty())
    {
        std::fprintf(stderr, "usage: gsplay DUMP.gs [--out FILE.ppm] [--loops N]\n");
        return 2;
    }

    GsDump dump;
    if (!dump.load(input))
    {
        std::fprintf(stderr, "gsplay: %s\n", dump.error.c_str());
        return 1;
    }
    uint32_t transfers = 0, vsyncs = 0, regPackets = 0, fifoReads = 0;
    uint64_t transferBytes = 0;
    for (const GsDump::Packet &p : dump.packets)
    {
        if (p.type == GsDump::PacketType::Transfer)
        {
            ++transfers;
            transferBytes += p.size;
        }
        else if (p.type == GsDump::PacketType::Vsync)
            ++vsyncs;
        else if (p.type == GsDump::PacketType::Registers)
            ++regPackets;
        else
            ++fifoReads;
    }
    std::printf("dump: state v%u, %u transfers (%.1f MB), %u vsyncs, %u register packets, %u FIFO reads\n",
                dump.stateVersion, transfers, transferBytes / 1048576.0, vsyncs, regPackets, fifoReads);
    for (uint32_t i = 0; i < 4u; ++i)
    {
        const uint64_t tagLo = GsDump::read64(dump.paths() + i * 20u);
        if (tagLo & 0x7FFFu)
            std::printf("note: GIF path %u was mid-tag at the start (nloop %u); the first packet may misparse\n", i,
                        static_cast<uint32_t>(tagLo & 0x7FFFu));
    }

#ifdef GSPLAY_GLES
    if (!gsplayGlInit())
        return 1;
#endif

    static std::vector<uint8_t> vram(GsDump::kVramSize);
    static GSRegisters regs{};
    std::vector<uint8_t> shown;
    uint32_t shownW = 0, shownH = 0;
    std::vector<double> times;
    std::vector<double> vsyncMs;
    std::unique_ptr<GS> gsOwner;
#ifdef GSPLAY_GLES
    GSGlesBackend *gles = nullptr;
#endif

    for (int loop = 0; loop < loops; ++loop)
    {
        std::memcpy(vram.data(), dump.vram(), GsDump::kVramSize);
        loadPriv(regs, dump.priv);
        if (!gsOwner || !warm)
        {
            gsOwner = std::make_unique<GS>();
            gsOwner->init(vram.data(), GsDump::kVramSize, &regs);
            gsOwner->setDebugHistoryPaused(true);
#ifdef GSPLAY_GLES
            gles = new GSGlesBackend();
            gsOwner->setRasterBackend(std::unique_ptr<GSRasterBackend>(gles));
#endif
        }
        GS &gs = *gsOwner;
#ifdef GSPLAY_GLES
        gles->setCaptureNextPresent(loop == loops - 1 && !output.empty());
#endif
        restoreRegisters(gs, dump);
        if (loop == 1 && std::getenv("GSPLAY_SAMPLE"))
            startSampling();
        const double start = nowMs();
        double last = start;
        int vsyncIndex = 0;
        double packetMs = 0, latchMs = 0;
#ifdef GSPLAY_GLES
        {
            double a, b;
            gsplayGlTimes(a, b);
        }
#endif
        vsyncMs.clear();
        for (const GsDump::Packet &p : dump.packets)
        {
            switch (p.type)
            {
            case GsDump::PacketType::Transfer:
            {
                const double t0 = nowMs();
                gs.processGIFPacket(p.data, p.size);
                packetMs += nowMs() - t0;
                break;
            }
            case GsDump::PacketType::Vsync:
                ++vsyncIndex;
                regs.vsyncTick.fetch_add(1);
                {
                    const double t0 = nowMs();
                    gs.latchHostPresentationFrame();
                    latchMs += nowMs() - t0;
                }
                {
                    std::vector<uint8_t> pixels;
                    uint32_t w = 0, h = 0;
                    if (gs.copyLatchedHostPresentationFrame(pixels, w, h) && !pixels.empty() && (wantVsync == 0 || wantVsync == vsyncIndex))
                    {
                        shown.swap(pixels);
                        shownW = w;
                        shownH = h;
                    }
                }
#ifdef GSPLAY_GLES
                gsplayGlSwap();
#endif
                {
                    const double now = nowMs();
                    vsyncMs.push_back(now - last);
                    last = now;
                }
                break;
            case GsDump::PacketType::Registers:
                loadPriv(regs, p.data);
                break;
            case GsDump::PacketType::FifoRead:
            {
                static std::vector<uint8_t> sink;
                sink.resize(p.size);
                gs.consumeLocalToHostBytes(sink.data(), p.size);
                break;
            }
            }
        }
        times.push_back(nowMs() - start);
        if (loop == loops - 1 && loops > 1 && std::getenv("GSPLAY_SAMPLE"))
            stopSampling(std::getenv("GSPLAY_SAMPLE"));
        if (loop == loops - 1)
            {
#ifdef GSPLAY_GLES
            double finishMs = 0, swapMs = 0;
            gsplayGlTimes(finishMs, swapMs);
            std::printf("last loop ms: total %.2f, GIF packets %.2f, latch (run commands and present) %.2f, finish (GPU) %.2f, swap %.2f\n", times.back(), packetMs, latchMs, finishMs, swapMs);
#else
            std::printf("last loop ms: total %.2f, GIF packets %.2f, latch %.2f\n", times.back(), packetMs, latchMs);
#endif
        }
#ifdef GSPLAY_GLES
        if (!warm || loop == loops - 1)
            gles->printStats();
#endif
    }
    if (vsyncMs.size() <= 16u)
    {
        std::printf("vsync intervals ms:");
        for (double t : vsyncMs)
            std::printf(" %.2f", t);
        std::printf("\n");
    }

    if (loops > 1)
    {
        std::vector<double> sorted = times;
        for (size_t i = 0; i + 1 < sorted.size(); ++i)
            for (size_t j = i + 1; j < sorted.size(); ++j)
                if (sorted[j] < sorted[i])
                    std::swap(sorted[i], sorted[j]);
        std::printf("loops %d, median %.2f ms, min %.2f, max %.2f\n", loops, sorted[sorted.size() / 2], sorted.front(), sorted.back());
    }
    else
        std::printf("time %.2f ms\n", times[0]);

    if (!output.empty())
    {
        if (shown.empty())
        {
            std::fprintf(stderr, "gsplay: no frame to write\n");
            return 1;
        }
        const uint32_t stride = static_cast<uint32_t>(shown.size() / (shownH ? shownH : 1u));
        const uint32_t tall = static_cast<uint32_t>(shown.size() / stride);
        std::printf("frame %ux%u, buffer %zu bytes, stride %u\n", shownW, shownH, shown.size(), stride);
        if (!writePpm(output, shown, shownW, tall < shownH ? tall : shownH, stride))
            return 1;
    }
    return 0;
}
