/* CPU affinity and user-space hardware counters. This is the only file
   built with _GNU_SOURCE: elsewhere that would redirect libc calls to newer
   glibc symbol versions and raise the glibc the binaries need. */
#define _GNU_SOURCE
#include "bench.h"

#include <linux/perf_event.h>
#include <sched.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set) ? -1 : 0;
}

static int perf_fd(uint64_t config)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE;
    a.size = sizeof a;
    a.config = config;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    return (int)syscall(__NR_perf_event_open, &a, 0, -1, -1, 0);
}

int perf_open(perf_ctr *p)
{
    p->fd_ins = perf_fd(PERF_COUNT_HW_INSTRUCTIONS);
    p->fd_cyc = perf_fd(PERF_COUNT_HW_CPU_CYCLES);
    if (p->fd_ins < 0 || p->fd_cyc < 0) {
        perf_close(p);
        return -1;
    }
    return 0;
}

void perf_start(perf_ctr *p)
{
    ioctl(p->fd_ins, PERF_EVENT_IOC_RESET, 0);
    ioctl(p->fd_cyc, PERF_EVENT_IOC_RESET, 0);
    ioctl(p->fd_ins, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(p->fd_cyc, PERF_EVENT_IOC_ENABLE, 0);
}

void perf_stop(perf_ctr *p, uint64_t *ins, uint64_t *cyc)
{
    ioctl(p->fd_ins, PERF_EVENT_IOC_DISABLE, 0);
    ioctl(p->fd_cyc, PERF_EVENT_IOC_DISABLE, 0);
    if (read(p->fd_ins, ins, sizeof *ins) != sizeof *ins)
        *ins = 0;
    if (read(p->fd_cyc, cyc, sizeof *cyc) != sizeof *cyc)
        *cyc = 0;
}

void perf_close(perf_ctr *p)
{
    if (p->fd_ins >= 0)
        close(p->fd_ins);
    if (p->fd_cyc >= 0)
        close(p->fd_cyc);
    p->fd_ins = p->fd_cyc = -1;
}
