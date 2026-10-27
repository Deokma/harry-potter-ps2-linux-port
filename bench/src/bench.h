/* Shared declarations for the Phase 2 device benchmarks
   (docs/PHASE_2_DEVICE_BENCH.md). Test ids T1..T8 follow that spec. */
#ifndef BENCH_H
#define BENCH_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BENCH_MAX_SAMPLES 16

typedef struct {
    int reps;           /* repetitions per measurement: median / min / max */
    double warmup_s;    /* all-core warm-up before the first test */
    int quick;          /* small sizes, 1 rep: qemu and desktop smoke runs */
    const char *out_dir;
} bench_opts;

extern bench_opts g_opts;

double now_s(void);                 /* CLOCK_MONOTONIC seconds */
int cpu_count(void);
uint64_t rng_next(uint64_t *state); /* xorshift64*, deterministic test data */
uint64_t fnv1a(const void *p, size_t n);
int mkdir_p(const char *path);      /* 0 on success or if it exists */

/* ---- results ------------------------------------------------------------ */

typedef struct {
    double v[BENCH_MAX_SAMPLES];
    int n;
} samples;

void samples_add(samples *s, double x);
double samples_median(const samples *s);
double samples_min(const samples *s);
double samples_max(const samples *s);

/* One result row. params_json is a comma-separated list of JSON members
   without braces (e.g. "\"n\":1000,\"path\":\"vbo\""), or NULL. */
void result_add(const char *test, const char *variant, const char *metric,
                const char *unit, const samples *s, const char *params_json);
void result_add1(const char *test, const char *variant, const char *metric,
                 const char *unit, double value, const char *params_json);
void note_str(const char *key, const char *value);   /* run conditions */
void note_num(const char *key, double value);
/* Writes <out_dir>/<date>-<firmware>-<prefix>.json and .csv. */
int results_write(const char *prefix);

/* ---- system ------------------------------------------------------------- */

void sysinfo_collect(void);         /* uname, OS, CPU, memory, libc, build */
double cpu_cur_freq_mhz(int cpu);   /* < 0 if unknown (e.g. under qemu) */
double mem_available_mb(void);
void temps_note(const char *when);  /* thermal zones -> notes temp.<when>.* */
void warmup_all_cores(double seconds);

typedef void (*thread_fn)(void *arg, int index, int count);
/* Runs fn on `count` threads (thread i pinned to CPU i if pin) and joins. */
void run_threads(int count, thread_fn fn, void *arg, int pin);

/* linux_gnu.c: the only file built with _GNU_SOURCE */
int pin_to_cpu(int cpu);
typedef struct { int fd_ins, fd_cyc; } perf_ctr;
int perf_open(perf_ctr *p);         /* 0 if user-space HW counters work */
void perf_start(perf_ctr *p);
void perf_stop(perf_ctr *p, uint64_t *ins, uint64_t *cyc);
void perf_close(perf_ctr *p);

/* ---- tests -------------------------------------------------------------- */

void t5_cpu_run(void);
void t6_run(void);
void t7_run(void);
void t8_sys_run(const char *sd_path, int sd_mb);
void t8_thermal_run(double seconds);

/* Background load for T8 (thermal, audio): T6's integer kernel on
   `threads` threads until load_stop(). */
void load_start(int threads);
void load_stop(void);

/* Self-tests: print diagnostics, return the number of failures. */
int t5_selftest(void);
int t6_selftest(void);
int t7_selftest(void);

#endif
