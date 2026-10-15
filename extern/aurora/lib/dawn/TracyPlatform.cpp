#include "TracyPlatform.hpp"

#if __has_include(<dawn/platform/DawnPlatform.h>)

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <dawn/platform/DawnPlatform.h>
#include <fmt/format.h>
#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>
#endif

namespace aurora::webgpu {
namespace {
// Dawn's own compile timers (histograms in microseconds), summed so the
// pipeline stats line can say where a cold pipeline's time goes: WGSL
// parse, Tint IR and SPIR-V, the driver's shader module and pipeline.
struct CompileStat {
  const char* dawnName;
  const char* label;
  std::atomic_uint32_t count{0};
  std::atomic_uint64_t us{0};
};
std::array<CompileStat, 10> g_compileStats{{
    {"BlobCacheHashValidationFailed", "blob hash failed"},
    {"aurora.BuildWGSL", "wgsl gen"},
    {"CreateShaderModuleUS", "wgsl parse"},
    {"ShaderModuleProgramToIR", "tint ir"},
    {"ShaderModuleGenerateSPIRV", "tint spirv"},
    {"Vulkan.ValidateSpirv", "spirv validate"},
    {"Vulkan.CreateShaderModule", "vk module"},
    {"Vulkan.CreateGraphicsPipelines.CacheMiss", "vk pipeline miss"},
    {"Vulkan.CreateGraphicsPipelines.CacheHit", "vk pipeline hit"},
    {"CreateRenderPipelineUS", "dawn pipeline total"},
}};

void record(const char* name, int us) {
  for (auto& s : g_compileStats) {
    if (std::strcmp(s.dawnName, name) == 0) {
      ++s.count;
      s.us += static_cast<uint64_t>(us);
      return;
    }
  }
}

class AuroraDawnPlatform final : public dawn::platform::Platform {
public:
  void HistogramCustomCounts(const char* name, int sample, int, int, int) override { record(name, sample); }
  void HistogramCustomCountsHPC(const char* name, int sample, int, int, int) override { record(name, sample); }
  // Counted, not timed: a cached blob whose hash didn't match, so Dawn compiled again.
  void HistogramBoolean(const char* name, bool sample) override {
    if (sample)
      record(name, 0);
  }
  // Dawn's scoped timers record nothing while this returns 0 (the default).
  double MonotonicallyIncreasingTime() override {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
#ifdef TRACY_ENABLE
  const unsigned char* GetTraceCategoryEnabledFlag(dawn::platform::TraceCategory) override {
    static const unsigned char enabled = 1;
    return &enabled;
  }
  uint64_t AddTraceEvent(char phase, const unsigned char*, const char* name, uint64_t, double, int, const char**,
                         const unsigned char*, const uint64_t*, unsigned char) override {
    thread_local std::vector<TracyCZoneCtx> zoneStack;
    if (phase == 'B') {
      const uint64_t srcloc = ___tracy_alloc_srcloc_name(0, "dawn", 4, "", 0, name, std::strlen(name), 0);
      zoneStack.push_back(___tracy_emit_zone_begin_alloc(srcloc, 1));
    } else if (phase == 'E') {
      if (!zoneStack.empty()) {
        ___tracy_emit_zone_end(zoneStack.back());
        zoneStack.pop_back();
      }
    }
    return 0;
  }
#endif
};
AuroraDawnPlatform g_dawnPlatform;
} // namespace

dawn::platform::Platform* tracy_dawn_platform() { return &g_dawnPlatform; }

void record_compile_stat(const char* name, int us) { record(name, us); }

std::string compile_stats_line() {
  std::string out;
  for (const auto& s : g_compileStats) {
    const uint32_t n = s.count.load(std::memory_order_relaxed);
    if (n == 0)
      continue;
    const double ms = static_cast<double>(s.us.load(std::memory_order_relaxed)) / 1000.0;
    out += fmt::format("{}{} {:.0f} ms/{} ({:.2f})", out.empty() ? "" : ", ", s.label, ms, n, ms / n);
  }
  return out;
}
} // namespace aurora::webgpu

#else

namespace aurora::webgpu {
dawn::platform::Platform* tracy_dawn_platform() { return nullptr; }
void record_compile_stat(const char*, int) {}
std::string compile_stats_line() { return {}; }
} // namespace aurora::webgpu

#endif
