// What vu1gen's output runs on, and the interface vu1play sees.
#pragma once

#include <cstdint>

struct EgVu1State
{
    float vf[32][4];
    float acc[4];
    uint16_t vi[16];
    float q, p, i;
    uint32_t clip, mac;
};

typedef void (*EgVu1KickSink)(const uint8_t *data, uint32_t size);
void egVu1SetKickSink(EgVu1KickSink sink);

// Runs the microprogram from an entry (a pair index) until its E-bit. Registers persist in state between calls.
// False: that entry isn't translated, or a JR went somewhere the translator hadn't seen.
bool egVu1Run(EgVu1State &state, uint8_t *data, uint32_t entry, uint32_t top, uint32_t itop);
