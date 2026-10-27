/* T8 (CPU side): free memory, SD card read rates, thermal throttling under
   a 4-core load. The audio part of T8 needs SDL and lives in bench_gl. */
#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int write_test_file(const char *path, long bytes)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    const size_t chunk = 1 << 20;
    uint8_t *buf = malloc(chunk);
    uint64_t seed = 0x5D;
    for (long done = 0; done < bytes; done += (long)chunk) {
        for (size_t i = 0; i < chunk; i += 8) {
            uint64_t r = rng_next(&seed);
            memcpy(buf + i, &r, 8);
        }
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            free(buf);
            close(fd);
            return -1;
        }
    }
    free(buf);
    fsync(fd);
    close(fd);
    return 0;
}

/* Evict the file from the page cache (no root needed). */
static int drop_cache(int fd)
{
    return posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

static void sd_test(const char *path_arg, int mb)
{
    char path[512];
    int own = !path_arg;
    struct stat st;
    if (own)
        snprintf(path, sizeof path, "%s/sd_test.bin", g_opts.out_dir);
    else
        snprintf(path, sizeof path, "%s", path_arg);
    long bytes = (long)mb << 20;
    if (own && (mkdir_p(g_opts.out_dir) || write_test_file(path, bytes))) {
        printf("  T8 sd: cannot write %s: %s\n", path, strerror(errno));
        return;
    }
    if (stat(path, &st) || st.st_size < (1 << 20)) {
        printf("  T8 sd: %s missing or < 1 MiB\n", path);
        return;
    }
    bytes = (long)st.st_size;
    char par[600];
    snprintf(par, sizeof par, "\"file_mb\":%.0f,\"own_file\":%s,\"a01_mean_kBps\":85,\"a01_peak_kBps\":730",
             (double)bytes / (1 << 20), own ? "true" : "false");
    const size_t seqchunk = 1 << 20, rndchunk = 32 << 10; /* A01 reads were ~34 KB */
    uint8_t *buf = malloc(seqchunk);
    samples seq = { { 0 }, 0 }, rnd = { { 0 }, 0 }, iops = { { 0 }, 0 };
    int dropped = 1;
    for (int r = 0; r < g_opts.reps; r++) {
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            break;
        dropped &= drop_cache(fd) == 0;
        double t0 = now_s();
        long total = 0;
        ssize_t got;
        while ((got = read(fd, buf, seqchunk)) > 0)
            total += got;
        samples_add(&seq, (double)total / (now_s() - t0) / 1e6);
        dropped &= drop_cache(fd) == 0;
        uint64_t seed = 0xD15C + (uint64_t)r;
        long blocks = bytes / (long)rndchunk, nread = 0;
        double t1 = now_s(), limit = g_opts.quick ? 0.2 : 2.0;
        while (now_s() - t1 < limit) {
            off_t off = (off_t)(rng_next(&seed) % (uint64_t)blocks) * (off_t)rndchunk;
            if (pread(fd, buf, rndchunk, off) != (ssize_t)rndchunk)
                break;
            nread++;
        }
        double dt = now_s() - t1;
        samples_add(&rnd, (double)nread * rndchunk / dt / 1e6);
        samples_add(&iops, nread / dt);
        close(fd);
    }
    free(buf);
    if (own)
        unlink(path);
    note_num("t8.sd_cache_dropped", dropped);
    result_add("T8", "sd_seq_1MiB", "read_MBps", "MB/s", &seq, par);
    result_add("T8", "sd_rand_32KiB", "read_MBps", "MB/s", &rnd, par);
    result_add("T8", "sd_rand_32KiB", "reads_per_s", "1/s", &iops, par);
}

void t8_sys_run(const char *sd_path, int sd_mb)
{
    printf("T8: system\n");
    char par[64];
    snprintf(par, sizeof par, "\"threshold_mb\":400");
    result_add1("T8", "memory", "mem_available_mb", "MB", mem_available_mb(), par);
    sd_test(sd_path, sd_mb);
}

/* 4-core load for `seconds`, sampling every CPU's frequency and the SoC
   temperatures every 5 s. */
void t8_thermal_run(double seconds)
{
    printf("T8: thermal, %.0f s of load on %d cores\n", seconds, cpu_count());
    int n = cpu_count();
    temps_note("thermal_before");
    load_start(n);
    double t0 = now_s(), fmin = INFINITY, fsum = 0, tmax = -INFINITY;
    int nf = 0;
    char *series = malloc(65536);
    size_t len = 0;
    series[0] = 0;
    for (;;) {
        double el = now_s() - t0;
        len += (size_t)snprintf(series + len, 65536 - len, "%s%.0f:", len ? ";" : "", el);
        for (int c = 0; c < n && len < 65000; c++) {
            double f = cpu_cur_freq_mhz(c);
            if (f > 0) {
                fmin = f < fmin ? f : fmin;
                fsum += f;
                nf++;
            }
            len += (size_t)snprintf(series + len, 65536 - len, "%s%.0f", c ? "," : "", f);
        }
        char p[96], b[32];
        for (int z = 0; z < 16; z++) {
            snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
            FILE *f = fopen(p, "r");
            if (!f)
                continue;
            if (fgets(b, sizeof b, f)) {
                double tc = strtod(b, NULL) / 1000.0;
                tmax = tc > tmax ? tc : tmax;
                if (len < 65000)
                    len += (size_t)snprintf(series + len, 65536 - len, "/z%d=%.1f", z, tc);
            }
            fclose(f);
        }
        if (el >= seconds)
            break;
        double next = t0 + 5.0 * (double)((long)(el / 5.0) + 1);
        while (now_s() < next && now_s() - t0 < seconds)
            nanosleep(&(struct timespec){ 0, 100000000 }, NULL);
    }
    load_stop();
    temps_note("thermal_after");
    note_str("t8.thermal_series", series);
    free(series);
    char par[96];
    snprintf(par, sizeof par, "\"seconds\":%.0f,\"threads\":%d,\"threshold_mhz\":1500", seconds, n);
    result_add1("T8", "thermal", "freq_min_mhz", "MHz", isfinite(fmin) ? fmin : NAN, par);
    result_add1("T8", "thermal", "freq_mean_mhz", "MHz", nf ? fsum / nf : NAN, par);
    result_add1("T8", "thermal", "temp_max_c", "C", isfinite(tmax) ? tmax : NAN, par);
}
