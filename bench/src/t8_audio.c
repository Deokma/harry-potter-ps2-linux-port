/* T8, audio part: SDL2 audio at 48 kHz stereo with 512- and 1024-frame
   buffers while T6's integer kernel loads every core. The callback mixes
   48 voices (looping, linear interpolation, per-voice volume), roughly
   the work an SPU2 mixer does. SDL doesn't report device underruns, so the
   proxy is callback lateness: a gap longer than 1.5 buffer periods counts
   as a probable underrun. The mix goes to a scratch buffer and the device
   gets silence (the voices are noise). */
#define _POSIX_C_SOURCE 200809L
#include "gl_bench.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VOICES 48
#define WAVE 4096

typedef struct {
    int16_t wave[WAVE];
    int16_t scratch[2 * 8192];
    uint32_t pos[VOICES], step[VOICES];
    int32_t vol_l[VOICES], vol_r[VOICES];
    double period_s, last, max_gap, cost_sum, cost_max;
    atomic_int late, calls;
} mixer;

static void cb(void *user, uint8_t *stream, int len)
{
    mixer *m = user;
    double t0 = now_s();
    if (m->last > 0) {
        double gap = t0 - m->last;
        if (gap > m->max_gap)
            m->max_gap = gap;
        if (gap > 1.5 * m->period_s)
            atomic_fetch_add(&m->late, 1);
    }
    m->last = t0;
    int frames = len / 4;
    if (frames > 8192)
        frames = 8192;
    int16_t *out = m->scratch;
    memset(stream, 0, (size_t)len);
    for (int f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        for (int v = 0; v < VOICES; v++) {
            uint32_t p = m->pos[v], i = (p >> 16) & (WAVE - 1), fr = p & 0xFFFF;
            int32_t a = m->wave[i], b = m->wave[(i + 1) & (WAVE - 1)];
            int32_t s = a + (int32_t)(((int64_t)(b - a) * fr) >> 16);
            l += (s * m->vol_l[v]) >> 15;
            r += (s * m->vol_r[v]) >> 15;
            m->pos[v] = p + m->step[v];
        }
        out[2 * f] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * f + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    double c = now_s() - t0;
    m->cost_sum += c;
    if (c > m->cost_max)
        m->cost_max = c;
    atomic_fetch_add(&m->calls, 1);
}

static void run_one(int samples_per_buf, double seconds)
{
    mixer *m = calloc(1, sizeof *m);
    uint64_t seed = 0xA0D;
    for (int i = 0; i < WAVE; i++)
        m->wave[i] = (int16_t)(rng_next(&seed) >> 48);
    for (int v = 0; v < VOICES; v++) {
        m->step[v] = 0x8000 + (uint32_t)(rng_next(&seed) % 0x18000);
        m->vol_l[v] = (int32_t)(rng_next(&seed) % 700);
        m->vol_r[v] = (int32_t)(rng_next(&seed) % 700);
    }
    bsdl_audio_spec want, have;
    memset(&want, 0, sizeof want);
    memset(&have, 0, sizeof have);
    want.freq = 48000;
    want.format = BSDL_AUDIO_S16LSB;
    want.channels = 2;
    want.samples = (uint16_t)samples_per_buf;
    want.callback = cb;
    want.userdata = m;
    bsdl_audio_device dev = sdl.OpenAudioDevice(NULL, 0, &want, &have, 0);
    char var[32], par[256];
    snprintf(var, sizeof var, "audio_48k_%d", samples_per_buf);
    if (!dev) {
        printf("  T8 audio: open failed: %s\n", sdl.GetError());
        note_str("t8.audio_error", sdl.GetError());
        free(m);
        return;
    }
    m->period_s = (double)have.samples / have.freq;
    load_start(cpu_count());
    sdl.PauseAudioDevice(dev, 0);
    double t0 = now_s();
    while (now_s() - t0 < seconds) {
        gl_events();
        nanosleep(&(struct timespec){ 0, 50000000 }, NULL);
    }
    sdl.PauseAudioDevice(dev, 1);
    load_stop();
    sdl.CloseAudioDevice(dev);
    int calls = atomic_load(&m->calls);
    snprintf(par, sizeof par,
             "\"freq\":%d,\"samples\":%d,\"voices\":%d,\"load_threads\":%d,\"seconds\":%.0f,\"calls\":%d",
             have.freq, have.samples, VOICES, cpu_count(), seconds, calls);
    result_add1("T8", var, "late_callbacks", "count", atomic_load(&m->late), par);
    result_add1("T8", var, "max_gap_ms", "ms", m->max_gap * 1e3, par);
    result_add1("T8", var, "period_ms", "ms", m->period_s * 1e3, par);
    result_add1("T8", var, "callback_cost_mean_us", "us", calls ? m->cost_sum / calls * 1e6 : 0, par);
    result_add1("T8", var, "callback_cost_max_us", "us", m->cost_max * 1e6, par);
    free(m);
}

void t8_audio_run(double seconds)
{
    printf("T8: audio under 4-core load, %.0f s per buffer size\n", seconds);
    note_str("audio.driver", sdl.GetCurrentAudioDriver() ? sdl.GetCurrentAudioDriver() : "none");
    run_one(512, seconds);
    run_one(1024, seconds);
    temps_note("t8_audio");
}
