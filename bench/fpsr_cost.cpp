// Phase 3 C4: what the A53 charges for the FPU system registers that eegen's
// float groups use (mrs fpsr after float work, msr fpsr, an FPCR.FZ toggle),
// next to plain float multiplies. Cycles per iteration at the pinned clock.
//   aarch64-linux-gnu-g++ -O2 -static -mtune=cortex-a53 -o fpsr_cost bench/fpsr_cost.cpp
//   fpsr_cost [KHZ]   (the clock the run is pinned to, default 1416000)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

static double now()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    const double khz = argc > 1 && std::atof(argv[1]) > 1000 ? std::atof(argv[1]) : 1416000.0;
    const long n = 20000000;
    volatile float seed = 1.0000001f;
    float a = seed, b = seed, c = seed, d = seed;
    uint64_t sink = 0;
    auto report = [&](const char *what, double s) {
        std::printf("%-44s %6.2f cycles / iteration%c", what, s * khz * 1e3 / n, 10);
    };

    double t0 = now();
    for (long i = 0; i < n; ++i)
    {
        a *= b; c *= d; // two independent multiplies
        asm volatile("" : "+w"(a), "+w"(c));
    }
    report("2 fmul", now() - t0);

    t0 = now();
    for (long i = 0; i < n; ++i)
    {
        a *= b; c *= d;
        uint64_t v;
        asm volatile("mrs %0, fpsr" : "=r"(v) : "w"(a), "w"(c) : "memory");
        sink += v;
    }
    report("2 fmul + mrs fpsr (needs their flags)", now() - t0);

    t0 = now();
    for (long i = 0; i < n; ++i)
    {
        a *= b; c *= d;
        asm volatile("" : "+w"(a), "+w"(c));
        asm volatile("msr fpsr, xzr" ::: "memory");
    }
    report("2 fmul + msr fpsr", now() - t0);

    t0 = now();
    for (long i = 0; i < n; ++i)
    {
        uint64_t f;
        asm volatile("mrs %0, fpcr" : "=r"(f));
        asm volatile("msr fpcr, %0" ::"r"(f | (1ull << 24)) : "memory");
        a *= b;
        asm volatile("msr fpcr, %0" ::"r"(f) : "memory");
        asm volatile("" : "+w"(a));
    }
    report("FZ on, fmul, FZ off (mrs + 2 msr fpcr)", now() - t0);

    std::printf("(sink %llu %g)%c", (unsigned long long)(sink & 1), (double)(a + c), 10);
    return 0;
}
