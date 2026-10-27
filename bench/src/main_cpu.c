/* bench_cpu: the CPU-side tests (T5 CPU path, T6, T7, T8 system and
   thermal). Built statically so it runs on any aarch64 Linux and under
   qemu-user without a sysroot. */
#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    puts("usage: bench_cpu [--selftest] [--quick] [--reps N] [--warmup S] [--out DIR]\n"
         "                 [--tests T8,T5,T6,T7] [--sd FILE] [--sd-mb N] [--thermal S]\n"
         "  --selftest   correctness checks only; prints checksums to compare builds\n"
         "  --quick      small sizes, 1 rep, no warm-up (qemu / desktop smoke run)\n"
         "  --tests      comma list, default T8,T5,T6,T7 (T8 = memory + SD)\n"
         "  --sd FILE    read an existing file for the SD test instead of writing one\n"
         "  --thermal S  only run the thermal test: S seconds of 4-core load\n"
         "Results: <out>/<date>-<firmware>-cpu.json and .csv (default out: results)");
}

static int has_test(const char *list, const char *id)
{
    size_t n = strlen(id);
    for (const char *p = list; (p = strstr(p, id)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ','))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *tests = "T8,T5,T6,T7", *sd = NULL;
    int selftest = 0, sd_mb = 128;
    double thermal = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--selftest"))
            selftest = 1;
        else if (!strcmp(a, "--quick"))
            g_opts.quick = 1;
        else if (!strcmp(a, "--reps") && v)
            g_opts.reps = atoi(argv[++i]);
        else if (!strcmp(a, "--warmup") && v)
            g_opts.warmup_s = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--out") && v)
            g_opts.out_dir = argv[++i];
        else if (!strcmp(a, "--tests") && v)
            tests = argv[++i];
        else if (!strcmp(a, "--sd") && v)
            sd = argv[++i];
        else if (!strcmp(a, "--sd-mb") && v)
            sd_mb = atoi(argv[++i]);
        else if (!strcmp(a, "--thermal") && v)
            thermal = strtod(argv[++i], NULL);
        else {
            usage();
            return a[0] == '-' && a[1] == 'h' ? 0 : 2;
        }
    }
    if (g_opts.quick) {
        g_opts.reps = 1;
        g_opts.warmup_s = 0;
        sd_mb = 16;
    }
    if (g_opts.reps < 1 || g_opts.reps > BENCH_MAX_SAMPLES)
        g_opts.reps = 3;
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (selftest) {
        int fails = t5_selftest() + t6_selftest() + t7_selftest();
        printf("selftest: %d failure(s)\n", fails);
        return fails ? 1 : 0;
    }

    sysinfo_collect();
    note_str("binary", "bench_cpu");
    note_str("tests", thermal > 0 ? "thermal" : tests);
    if (thermal > 0) {
        t8_thermal_run(thermal);
        return results_write("thermal") ? 1 : 0;
    }
    warmup_all_cores(g_opts.warmup_s);
    if (has_test(tests, "T8"))
        t8_sys_run(sd, sd_mb);
    if (has_test(tests, "T5"))
        t5_cpu_run();
    temps_note("after_T5");
    if (has_test(tests, "T6"))
        t6_run();
    temps_note("after_T6");
    if (has_test(tests, "T7"))
        t7_run();
    temps_note("end");
    return results_write("cpu") ? 1 : 0;
}
