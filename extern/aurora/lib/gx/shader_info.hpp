#pragma once

#include "gx.hpp"

namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
// sharedKey: a hash of the uniform without its position and normal
// matrices (draws differing only in those can be batched).
gfx::Range build_uniform(const ShaderInfo& info, u64* sharedKey = nullptr) noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
