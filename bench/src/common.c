/* Timing, statistics, result collection (JSON + CSV), system information. */
#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#ifndef BENCH_GIT
#define BENCH_GIT "unknown"
#endif
#ifndef BENCH_TARGET
#define BENCH_TARGET "unknown"
#endif

bench_opts g_opts = { 3, 10.0, 0, "results" };

double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

uint64_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
}

/* ---- samples ------------------------------------------------------------ */

void samples_add(samples *s, double x)
{
    if (s->n < BENCH_MAX_SAMPLES)
        s->v[s->n++] = x;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double samples_median(const samples *s)
{
    double t[BENCH_MAX_SAMPLES];
    if (!s->n)
        return NAN;
    memcpy(t, s->v, (size_t)s->n * sizeof *t);
    qsort(t, (size_t)s->n, sizeof *t, cmp_d);
    return s->n & 1 ? t[s->n / 2] : 0.5 * (t[s->n / 2 - 1] + t[s->n / 2]);
}

double samples_min(const samples *s)
{
    double m = s->n ? s->v[0] : NAN;
    for (int i = 1; i < s->n; i++)
        m = s->v[i] < m ? s->v[i] : m;
    return m;
}

double samples_max(const samples *s)
{
    double m = s->n ? s->v[0] : NAN;
    for (int i = 1; i < s->n; i++)
        m = s->v[i] > m ? s->v[i] : m;
    return m;
}

/* ---- results and notes -------------------------------------------------- */

typedef struct {
    char test[8], variant[96], metric[48], unit[24];
    samples s;
    char *params;
} row;

typedef struct {
    char *key, *val;
    double num;
    int is_num;
} note;

static row *rows;
static int nrows, caprows;
static note *notes;
static int nnotes, capnotes;
static pthread_mutex_t res_lock = PTHREAD_MUTEX_INITIALIZER;

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static void copy_field(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

void result_add(const char *test, const char *variant, const char *metric,
                const char *unit, const samples *s, const char *params_json)
{
    pthread_mutex_lock(&res_lock);
    if (nrows == caprows) {
        caprows = caprows ? caprows * 2 : 64;
        rows = realloc(rows, (size_t)caprows * sizeof *rows);
    }
    row *r = &rows[nrows++];
    copy_field(r->test, sizeof r->test, test);
    copy_field(r->variant, sizeof r->variant, variant);
    copy_field(r->metric, sizeof r->metric, metric);
    copy_field(r->unit, sizeof r->unit, unit);
    r->s = *s;
    r->params = params_json ? dupstr(params_json) : NULL;
    pthread_mutex_unlock(&res_lock);
    printf("  %-4s %-34s %-22s %12.4g  [min %.4g max %.4g] %s\n", test, variant,
           metric, samples_median(s), samples_min(s), samples_max(s), unit);
    fflush(stdout);
}

void result_add1(const char *test, const char *variant, const char *metric,
                 const char *unit, double value, const char *params_json)
{
    samples s = { { value }, 1 };
    result_add(test, variant, metric, unit, &s, params_json);
}

static note *note_slot(const char *key)
{
    for (int i = 0; i < nnotes; i++)
        if (!strcmp(notes[i].key, key)) {
            free(notes[i].val);
            notes[i].val = NULL;
            return &notes[i];
        }
    if (nnotes == capnotes) {
        capnotes = capnotes ? capnotes * 2 : 64;
        notes = realloc(notes, (size_t)capnotes * sizeof *notes);
    }
    notes[nnotes].key = dupstr(key);
    notes[nnotes].val = NULL;
    return &notes[nnotes++];
}

void note_str(const char *key, const char *value)
{
    pthread_mutex_lock(&res_lock);
    note *n = note_slot(key);
    n->val = dupstr(value ? value : "");
    n->is_num = 0;
    pthread_mutex_unlock(&res_lock);
}

void note_num(const char *key, double value)
{
    pthread_mutex_lock(&res_lock);
    note *n = note_slot(key);
    n->num = value;
    n->is_num = 1;
    pthread_mutex_unlock(&res_lock);
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c == '\n')
            fputs("\\n", f);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static void json_num(FILE *f, double v)
{
    if (isfinite(v))
        fprintf(f, "%.6g", v);
    else
        fputs("null", f);
}

static const char *note_get(const char *key)
{
    for (int i = 0; i < nnotes; i++)
        if (!strcmp(notes[i].key, key) && !notes[i].is_num)
            return notes[i].val;
    return NULL;
}

int mkdir_p(const char *path)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0755) && errno != EEXIST)
                return -1;
            *p = '/';
        }
    return mkdir(buf, 0755) && errno != EEXIST ? -1 : 0;
}

int results_write(const char *prefix)
{
    char stamp[32], fw[40] = "unknown", path[600];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    const char *os = note_get("os.pretty_name");
    if (os) {
        size_t j = 0;
        for (const char *c = os; *c && j < sizeof fw - 1; c++)
            fw[j++] = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                      (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' ? *c : '_';
        fw[j] = 0;
    }
    if (mkdir_p(g_opts.out_dir)) {
        fprintf(stderr, "cannot create %s: %s\n", g_opts.out_dir, strerror(errno));
        return -1;
    }

    snprintf(path, sizeof path, "%s/%s-%s-%s.json", g_opts.out_dir, stamp, fw, prefix);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "{\n \"bench\": \"ps2port-bench\",\n \"binary\": ");
    json_str(f, prefix);
    fprintf(f, ",\n \"created\": ");
    json_str(f, stamp);
    fprintf(f, ",\n \"quick\": %s,\n \"reps\": %d,\n \"conditions\": {", g_opts.quick ? "true" : "false",
            g_opts.reps);
    for (int i = 0; i < nnotes; i++) {
        fprintf(f, "%s\n  ", i ? "," : "");
        json_str(f, notes[i].key);
        fputs(": ", f);
        if (notes[i].is_num)
            json_num(f, notes[i].num);
        else
            json_str(f, notes[i].val);
    }
    fprintf(f, "\n },\n \"results\": [");
    for (int i = 0; i < nrows; i++) {
        row *r = &rows[i];
        fprintf(f, "%s\n  {\"test\": ", i ? "," : "");
        json_str(f, r->test);
        fputs(", \"variant\": ", f);
        json_str(f, r->variant);
        fputs(", \"metric\": ", f);
        json_str(f, r->metric);
        fputs(", \"unit\": ", f);
        json_str(f, r->unit);
        fputs(", \"median\": ", f);
        json_num(f, samples_median(&r->s));
        fputs(", \"min\": ", f);
        json_num(f, samples_min(&r->s));
        fputs(", \"max\": ", f);
        json_num(f, samples_max(&r->s));
        fputs(", \"samples\": [", f);
        for (int k = 0; k < r->s.n; k++) {
            if (k)
                fputs(", ", f);
            json_num(f, r->s.v[k]);
        }
        fprintf(f, "], \"params\": {%s}}", r->params ? r->params : "");
    }
    fprintf(f, "\n ]\n}\n");
    fclose(f);
    printf("wrote %s\n", path);

    snprintf(path, sizeof path, "%s/%s-%s-%s.csv", g_opts.out_dir, stamp, fw, prefix);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "test,variant,metric,unit,median,min,max,n,params\n");
    for (int i = 0; i < nrows; i++) {
        row *r = &rows[i];
        fprintf(f, "%s,%s,%s,%s,%.6g,%.6g,%.6g,%d,\"", r->test, r->variant, r->metric,
                r->unit, samples_median(&r->s), samples_min(&r->s), samples_max(&r->s), r->s.n);
        for (const char *c = r->params ? r->params : ""; *c; c++) {
            if (*c == '"')
                fputc('"', f);
            fputc(*c, f);
        }
        fprintf(f, "\"\n");
    }
    fclose(f);
    printf("wrote %s\n", path);
    return 0;
}

/* ---- system information ------------------------------------------------- */

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static double read_num(const char *path)
{
    char b[64];
    return read_line(path, b, sizeof b) ? NAN : strtod(b, NULL);
}

double cpu_cur_freq_mhz(int cpu)
{
    char p[128];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    double khz = read_num(p);
    return isfinite(khz) ? khz / 1000.0 : -1.0;
}

static double meminfo_kb(const char *field)
{
    char line[256];
    double v = NAN;
    size_t n = strlen(field);
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return v;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, field, n) && line[n] == ':') {
            v = strtod(line + n + 1, NULL);
            break;
        }
    fclose(f);
    return v;
}

double mem_available_mb(void)
{
    return meminfo_kb("MemAvailable") / 1024.0;
}

void temps_note(const char *when)
{
    char p[128], type[64], key[160];
    for (int z = 0; z < 16; z++) {
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", z);
        if (read_line(p, type, sizeof type))
            continue;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        double mc = read_num(p);
        if (!isfinite(mc))
            continue;
        snprintf(key, sizeof key, "temp.%s.%d.%s", when, z, type);
        note_num(key, mc / 1000.0);
    }
}

static void note_file(const char *key, const char *path)
{
    char b[512];
    if (!read_line(path, b, sizeof b))
        note_str(key, b);
}

void sysinfo_collect(void)
{
    struct utsname u;
    char b[512], key[128], p[160];
    if (!uname(&u)) {
        note_str("kernel.release", u.release);
        note_str("kernel.version", u.version);
        note_str("kernel.machine", u.machine);
        note_str("host.name", u.nodename);
    }
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        while (fgets(b, sizeof b, f))
            if (!strncmp(b, "PRETTY_NAME=", 12)) {
                char *v = b + 12;
                v[strcspn(v, "\r\n")] = 0;
                if (*v == '"') {
                    v++;
                    v[strcspn(v, "\"")] = 0;
                }
                note_str("os.pretty_name", v);
            }
        fclose(f);
    }
    /* firmware version files, recorded only where present */
    note_file("fw.muos_version", "/opt/muos/config/version.txt");
    note_file("fw.batocera_version", "/usr/share/batocera/batocera.version");
    note_file("fw.issue", "/etc/issue");

    int n = cpu_count();
    note_num("cpu.online", n);
    for (int c = 0; c < n; c++) {
        static const char *fields[] = { "scaling_governor", "scaling_min_freq",
                                        "scaling_max_freq", "scaling_cur_freq",
                                        "cpuinfo_max_freq" };
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/%s", c, fields[i]);
            if (read_line(p, b, sizeof b))
                continue;
            snprintf(key, sizeof key, "cpu%d.%s", c, fields[i]);
            note_str(key, b);
        }
    }
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        int have_part = 0, have_feat = 0;
        while (fgets(b, sizeof b, f)) {
            b[strcspn(b, "\r\n")] = 0;
            char *colon = strchr(b, ':');
            if (!colon)
                continue;
            if (!have_part && !strncmp(b, "CPU part", 8)) {
                note_str("cpu.part", colon + 2);
                have_part = 1;
            } else if (!have_feat && !strncmp(b, "Features", 8)) {
                note_str("cpu.features", colon + 2);
                have_feat = 1;
            } else if (!strncmp(b, "model name", 10)) {
                note_str("cpu.model_name", colon + 2);
            }
        }
        fclose(f);
    }
    note_num("mem.total_mb", meminfo_kb("MemTotal") / 1024.0);
    note_num("mem.available_mb_start", mem_available_mb());
#ifdef __GLIBC__
    note_str("libc.glibc_runtime", gnu_get_libc_version());
#endif
    note_str("build.compiler", __VERSION__);
    note_str("build.git", BENCH_GIT);
    note_str("build.target", BENCH_TARGET);
    note_num("opts.quick", g_opts.quick);
    note_num("opts.reps", g_opts.reps);
    note_num("opts.warmup_s", g_opts.warmup_s);
    temps_note("start");
}

/* ---- threads ------------------------------------------------------------ */

typedef struct {
    thread_fn fn;
    void *arg;
    int index, count, pin;
} thread_ctx;

static void *thread_entry(void *p)
{
    thread_ctx *c = p;
    if (c->pin)
        pin_to_cpu(c->index % cpu_count());
    c->fn(c->arg, c->index, c->count);
    return NULL;
}

void run_threads(int count, thread_fn fn, void *arg, int pin)
{
    pthread_t th[64];
    thread_ctx ctx[64];
    if (count > 64)
        count = 64;
    for (int i = 0; i < count; i++) {
        ctx[i] = (thread_ctx){ fn, arg, i, count, pin };
        if (pthread_create(&th[i], NULL, thread_entry, &ctx[i])) {
            fprintf(stderr, "pthread_create failed\n");
            count = i;
            break;
        }
    }
    for (int i = 0; i < count; i++)
        pthread_join(th[i], NULL);
}

static void spin_fn(void *arg, int index, int count)
{
    (void)index;
    (void)count;
    double until = *(const double *)arg;
    volatile uint64_t x = 1;
    while (now_s() < until)
        for (int i = 0; i < 100000; i++)
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
}

void warmup_all_cores(double seconds)
{
    if (seconds <= 0)
        return;
    printf("warm-up: %.0f s on %d cores\n", seconds, cpu_count());
    double until = now_s() + seconds;
    run_threads(cpu_count(), spin_fn, &until, 1);
}
