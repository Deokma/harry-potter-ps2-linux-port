// ps2port trial, step 3: guest instruction counter, force-included into the
// generated files (no other dependencies).
#pragma once

#include <cstdint>

// counting build: make_harness.py puts HARNESS_COUNT() after every guest
// instruction; the timing build compiles it away
extern uint64_t harness_icount;
extern uint64_t harness_classes[16];
#ifdef HARNESS_COUNTING
#define HARNESS_COUNT() (++harness_icount)
#define HARNESS_COUNT_K(k) (++harness_icount, ++harness_classes[k])
#else
#define HARNESS_COUNT() ((void)0)
#define HARNESS_COUNT_K(k) ((void)0)
#endif
