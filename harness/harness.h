// ps2port trial, step 3: shared declarations for the replay harness.
#pragma once
#include <csetjmp>

#include <cstdint>
#include <map>
#include <string>

namespace harness
{
    // same layout as PS2Recomp's generated table: one slot per guest word
    constexpr uint32_t kTableEnd = 0x01000000u;
    constexpr uint32_t kTableSlots = kTableEnd >> 2;
    extern PS2Runtime::RecompiledFunction g_table[kTableSlots];

    struct Counters
    {
        uint64_t dispatches;
        uint64_t checkpoints;
        uint64_t unexpected;
        uint64_t syscalls;
    };
    extern Counters g_counters;

    bool checkpointDue(uint32_t cycles) noexcept;
    uint64_t iopCyclesCharged();

    void registerFunctions(); // harness_table.cpp, generated
    void loadVu0(const std::string &base); // hwmodel.inc
    void resetHardware();
    void report();
    extern std::jmp_buf g_stopJmp; // HARNESS_STOP_TIMER
    void setDmaRam(uint8_t *ram);
    void reportDma();
    void reportVu0(int iterations); // HARNESS_VU0=1 or 2
    bool vu0Running();
    bool vu0NeedsRecording();
    void vu0FinishRecording();
    bool skipped(uint32_t pc); // HARNESS_SKIP
    extern std::map<uint32_t, uint64_t> *g_calls; // HARNESS_CALLS
    extern std::map<uint32_t, uint64_t> g_skipped;
    void stubCalled(const char *name); // gen/harness_stubs.cpp defaults

    // FPU modes when eegen's float groups are in (eegen_fp.h): which slots
    // hold eegen's functions; calls between them and PS2Recomp's code switch
    // modes (enterMode returns -1 when nothing changed, else the old mode)
    extern bool g_egModes;
    extern bool g_isEg[kTableSlots];
    int enterMode(uint32_t pc);
    void leaveMode(int previous);
}

#include "harness_count.h"

extern uint8_t *ps2portRdram;
