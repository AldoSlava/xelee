#pragma once

#include <string>

namespace dawn::platform {
class Platform;
} // namespace dawn::platform

namespace aurora::webgpu {
// Dawn's platform: Tracy zones (TRACY_ENABLE) and compile-time totals.
dawn::platform::Platform* tracy_dawn_platform();
// Adds a sample (microseconds) to a compile timer by its Dawn histogram name.
void record_compile_stat(const char* name, int us);
// "wgsl parse 1234 ms/567 (2.18), ..." over the session; empty if none.
std::string compile_stats_line();
} // namespace aurora::webgpu
