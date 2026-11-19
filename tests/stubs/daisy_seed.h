/* Host-test stub: just enough of libDaisy for engine.cpp to compile. */
#pragma once
#define DSY_SDRAM_BSS

#include <chrono>
#include <cstdint>
namespace daisy {
struct System
{
    static uint32_t GetTick()
    {
        using namespace std::chrono;
        return static_cast<uint32_t>(duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count() / 5);
    }
    static uint32_t GetTickFreq() { return 200000000u; }
};
} // namespace daisy
