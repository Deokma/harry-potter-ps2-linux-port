// No translated code: vu1play with the interpreter only.
#include "vu1_rt.h"

uint64_t g_egVu1Notes[2048];
uint64_t g_egVu1Counts[2048];
void egVu1SetKickSink(EgVu1KickSink) {}
bool egVu1Run(EgVu1State &, uint8_t *, uint32_t, uint32_t, uint32_t) { return false; }
