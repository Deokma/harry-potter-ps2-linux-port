// Sound out (Phase 8): the SPU2 mixer's frames (ps2xIOP, iop_memory.cpp: 48 kHz stereo, made on the IOP's clock) through SDL2's
// audio device. SDL's callback resamples from the mixer's ring at the mixer's own rate, measured, nudged to keep about 80 ms
// waiting: while the game runs slower than real time (about 73 % in play, Phase 7), its sound plays slower and lower with it
// rather than breaking up. PS2PORT_AUDIO=0: no output. Opened from the main thread (ps2portDisplayInit).
extern "C"
{
#include "sdl_min.h"
}

#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

size_t ps2portSpuAvailable();
uint64_t ps2portSpuMixed();
size_t ps2portSpuPull(int16_t *out, size_t count);

namespace
{
    constexpr int kRate = 48000;
    constexpr double kTarget = 0.080 * kRate; // frames waiting in the ring

    bsdl_audio_device g_device = 0;
    double g_pos = 0.0;                     // between g_prev and g_next
    int32_t g_prev[2] = {0, 0}, g_next[2] = {0, 0};
    std::vector<int16_t> g_in, g_carry;
    double g_rate = kRate;                  // the mixer's frames a second, measured
    double g_rateSince = 0.0;
    uint64_t g_rateMixed = 0;
    uint64_t g_underruns = 0, g_callbacks = 0;

    double nowS()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void callback(void *, uint8_t *stream, int len)
    {
        int16_t *out = reinterpret_cast<int16_t *>(stream);
        const int frames = len / 4;
        ++g_callbacks;

        const double now = nowS();
        const uint64_t mixed = ps2portSpuMixed();
        if (g_rateSince == 0.0)
        {
            g_rateSince = now;
            g_rateMixed = mixed;
        }
        else if (now - g_rateSince >= 0.5)
        {
            const double measured = static_cast<double>(mixed - g_rateMixed) / (now - g_rateSince);
            g_rate = 0.7 * g_rate + 0.3 * std::clamp(measured, 0.1 * kRate, 2.0 * kRate);
            g_rateSince = now;
            g_rateMixed = mixed;
        }
        const double waiting = static_cast<double>(ps2portSpuAvailable());
        const double error = std::clamp((waiting - kTarget) / kTarget, -1.0, 1.0);
        const double ratio = std::clamp(g_rate / kRate * (1.0 + 0.05 * error), 0.2, 2.0);

        // frames this call reaches, after what the last call pulled and did not reach (g_carry)
        const size_t need = static_cast<size_t>(frames * ratio + g_pos) + 1u;
        const size_t carried = g_carry.size() / 2u;
        g_in.assign(g_carry.begin(), g_carry.end());
        g_in.resize(std::max(need, carried) * 2u);
        const size_t got = carried + (need > carried ? ps2portSpuPull(g_in.data() + carried * 2u, need - carried) : 0u);
        size_t used = 0;
        for (int i = 0; i < frames; ++i)
        {
            for (int c = 0; c < 2; ++c)
                out[i * 2 + c] = static_cast<int16_t>(g_prev[c] + (g_next[c] - g_prev[c]) * g_pos);
            g_pos += ratio;
            while (g_pos >= 1.0)
            {
                g_pos -= 1.0;
                g_prev[0] = g_next[0];
                g_prev[1] = g_next[1];
                if (used < got)
                {
                    g_next[0] = g_in[used * 2u];
                    g_next[1] = g_in[used * 2u + 1u];
                    ++used;
                }
                else
                {
                    // nothing waiting: fade what was playing rather than click
                    g_next[0] = g_next[0] * 15 / 16;
                    g_next[1] = g_next[1] * 15 / 16;
                    ++g_underruns;
                }
            }
        }
        g_carry.assign(g_in.begin() + static_cast<std::ptrdiff_t>(used * 2u), g_in.begin() + static_cast<std::ptrdiff_t>(got * 2u));
    }
}

void ps2portAudioInit()
{
    const char *setting = std::getenv("PS2PORT_AUDIO");
    if (setting && setting[0] == '0')
        return;
    if (!sdl.InitSubSystem || !sdl.OpenAudioDevice || !sdl.PauseAudioDevice)
    {
        std::fprintf(stderr, "[audio] this SDL2 lacks the audio calls: no sound\n");
        return;
    }
    if (sdl.InitSubSystem(BSDL_INIT_AUDIO) != 0)
    {
        std::fprintf(stderr, "[audio] SDL_InitSubSystem: %s\n", sdl.GetError());
        return;
    }
    bsdl_audio_spec want{}, have{};
    want.freq = kRate;
    want.format = BSDL_AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    want.callback = callback;
    g_device = sdl.OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_device)
    {
        std::fprintf(stderr, "[audio] SDL_OpenAudioDevice: %s\n", sdl.GetError());
        return;
    }
    // what the mixer made before the output opened is old: only the target's worth is kept
    std::vector<int16_t> drop(4096u * 2u);
    while (ps2portSpuAvailable() > static_cast<size_t>(kTarget))
        ps2portSpuPull(drop.data(), std::min<size_t>(4096u, ps2portSpuAvailable() - static_cast<size_t>(kTarget)));
    sdl.PauseAudioDevice(g_device, 0);
    std::fprintf(stderr, "[audio] %s, %d Hz, %d channels, %d frames a callback\n",
                 sdl.GetCurrentAudioDriver ? sdl.GetCurrentAudioDriver() : "?", have.freq, have.channels, have.samples);
}

// For the stats line: the mixer's rate and how often the output ran dry
void ps2portAudioStats(double &mixerRate, uint64_t &underruns)
{
    mixerRate = g_rate;
    underruns = g_underruns;
}
