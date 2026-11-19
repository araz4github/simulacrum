/* Host-test stub of daisy::CpuLoadMeter (no cycle counter on the host). */
#pragma once
#include <cstddef>
namespace daisy {
class CpuLoadMeter
{
  public:
    void  Init(float, size_t, float = 1.f) {}
    void  OnBlockStart() {}
    void  OnBlockEnd() {}
    float GetAvgCpuLoad() const { return 0.f; }
    float GetMaxCpuLoad() const { return 0.f; }
};
} // namespace daisy
