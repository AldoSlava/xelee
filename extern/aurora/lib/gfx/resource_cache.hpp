#pragma once

#include "types.hpp"

namespace aurora::gfx {

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor);
wgpu::BindGroup find_bind_group(BindGroupRef id);
wgpu::Sampler sampler_ref(const wgpu::SamplerDescriptor& descriptor);
// A cached bind group's texture views and samplers by binding (bindless
// batching: a draw's textures, from its texture bind group). References: the
// main thread can expire the bind group while the render worker still holds
// a copy, and a view only that bind group kept would die under it. False if
// it's not cached.
constexpr uint32_t MaxBindGroupResources = 16;
struct BindGroupResources {
  wgpu::TextureView views[MaxBindGroupResources];
  wgpu::Sampler samplers[MaxBindGroupResources];
};
bool bind_group_resources(BindGroupRef id, BindGroupResources& out);

namespace detail {
void clear_bind_group_cache();
void expire_cached_bind_groups();
void shutdown_resource_cache();
} // namespace detail

} // namespace aurora::gfx
