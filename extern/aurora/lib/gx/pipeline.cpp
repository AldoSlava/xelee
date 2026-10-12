#include "pipeline.hpp"

#include "../gfx/encoding.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/recording.hpp"
#include "../gfx/resource_cache.hpp"
#include "../gfx/frame_packet.hpp"
#include "../webgpu/gpu.hpp"

#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <array>
#include <cstdlib>
#include <string>

#include "../internal.hpp"

#include <tracy/Tracy.hpp>

namespace aurora::gx {
namespace {
constexpr Module Log{"aurora::gx::pipeline"};
} // namespace

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout) {
  ZoneScoped;
  const auto shader = build_shader(config.shaderConfig, layout);
  const auto label = fmt::format("GX Pipeline {:x}",
                                 xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX))));
  DecodedLayout decoded;
  if (config.shaderConfig.decodedAll && decoded_layout(config.shaderConfig, decoded)) {
    std::array<wgpu::VertexAttribute, 16> attrs{};
    for (u8 i = 0; i < decoded.count; ++i) {
      const auto& d = decoded.attrs[i];
      attrs[i] = {.format = d.format == DecodedFormat::U32     ? wgpu::VertexFormat::Uint32
                            : d.format == DecodedFormat::F32x2 ? wgpu::VertexFormat::Float32x2
                            : d.format == DecodedFormat::F32x3 ? wgpu::VertexFormat::Float32x3
                                                               : wgpu::VertexFormat::Float32x4,
                  .offset = d.offset,
                  .shaderLocation = i};
    }
    const std::array buffers{wgpu::VertexBufferLayout{.stepMode = wgpu::VertexStepMode::Vertex,
                                                      .arrayStride = decoded.stride,
                                                      .attributeCount = decoded.count,
                                                      .attributes = attrs.data()}};
    return build_pipeline(config, layout, buffers, shader, label.c_str());
  }
  if (config.shaderConfig.decodedPos) {
    static constexpr wgpu::VertexAttribute posAttr{
        .format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0};
    const std::array buffers{wgpu::VertexBufferLayout{
        .stepMode = wgpu::VertexStepMode::Vertex, .arrayStride = 12, .attributeCount = 1, .attributes = &posAttr}};
    return build_pipeline(config, layout, buffers, shader, label.c_str());
  }
  return build_pipeline(config, layout, {}, shader, label.c_str());
}

// Diagnostics for the untextured-white-quad artifact in melee-pc:
//   AURORA_SKIP_UNTEX=1      drop every draw that binds no texture
//   AURORA_SKIP_UNTEX_VTX=n  drop only untextured draws with n vertices
//   AURORA_LOG_UNTEX=1       report untextured draws, with the breadcrumb
// A draw with no texture bind group falls back to flat colour, which is what
// a solid white quad looks like on screen. aurora_draw_tag lets the game
// record which subsystem is currently rendering, so an offending draw can be
// traced back past the GX boundary.
extern "C" {
unsigned int aurora_draw_tag = 0;
}

static bool env_flag(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != '\0' && *v != '0';
}

#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
// AURORA_XR_LINK_STATS=1 (with the game's link markers): the eye replay's
// draws and indices per GX link, every 20000 draws. AURORA_XR_SKIP_LINKS=a,b:
// leave those links out of the eyes (measuring what they cost).
static bool xr_link_filter(const DrawData& data) {
  static const bool stats = env_flag("AURORA_XR_LINK_STATS");
  static const uint64_t skip = [] {
    uint64_t mask = 0;
    if (const char* v = std::getenv("AURORA_XR_SKIP_LINKS"))
      for (const char* p = v; *p != '\0';) {
        char* end = nullptr;
        const unsigned long n = std::strtoul(p, &end, 10);
        if (end == p)
          break;
        if (n < 64)
          mask |= uint64_t{1} << n;
        p = *end == ',' ? end + 1 : end;
      }
    return mask;
  }();
  const uint32_t link = (data.tag >> 16) - 1; // UINT32_MAX: none
  if (stats) {
    static std::array<uint64_t, 34> draws{}, indices{};
    static uint32_t n = 0, pipelineSwitches = 0, textureSwitches = 0;
    static gfx::PipelineRef lastPipeline = 0;
    static decltype(data.bindGroups.textureBindGroup) lastTextures{};
    pipelineSwitches += data.xrPipeline != lastPipeline ? 1 : 0;
    textureSwitches += data.bindGroups.textureBindGroup != lastTextures ? 1 : 0;
    lastPipeline = data.xrPipeline;
    lastTextures = data.bindGroups.textureBindGroup;
    const size_t i = std::min<uint32_t>(link + 1, 33);
    ++draws[i];
    indices[i] += data.xrIndexCount != 0 ? data.xrIndexCount : data.indexCount != 0 ? data.indexCount : data.vtxCount;
    if (++n % 20000 == 0) {
      std::string line;
      for (size_t k = 0; k < draws.size(); ++k)
        if (draws[k] != 0)
          line += fmt::format(" link {}: {} draws {} idx;", k == 0 ? -1 : static_cast<int>(k) - 1, draws[k], indices[k]);
      Log.info("XR eye draws by GX link (last 20000):{}; pipeline changes {}, texture changes {}", line,
               pipelineSwitches, textureSwitches);
      draws = {};
      indices = {};
      pipelineSwitches = textureSwitches = 0;
    }
  }
  return link < 64 && (skip >> link & 1) != 0;
}
#endif

static void render_direct(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  if (gfx::xr_multiview_replay() && !gfx::xr_motion_replay() && xr_link_filter(data))
    return;
  // A multiview replay can only bind multiview pipelines.
  if (gfx::xr_motion_replay()) {
    if (data.xrMotionPipeline == 0 || !gfx::bind_pipeline(data.xrMotionPipeline, pass))
      return;
  } else if (gfx::xr_multiview_replay() &&
             (data.xrPipeline == 0 || !gfx::bind_pipeline(data.xrPipeline, pass))) {
    return;
  }
  if (!gfx::xr_multiview_replay() && !gfx::bind_pipeline(data.pipeline, pass)) {
#else
  if (!gfx::bind_pipeline(data.pipeline, pass)) {
#endif
    /* AURORA_LOG_SKIPPED=1: report draws dropped because their pipeline is
     * still compiling. Pipelines are built asynchronously and an unready one
     * silently skips its draw, so a short-lived screen can miss its geometry
     * entirely on a cold cache. */
    if (env_flag("AURORA_LOG_SKIPPED")) {
      static u32 count = 0;
      if (++count <= 4000) {
        std::fprintf(stderr, "[WARN] aurora::gx: skipped draw: pipeline not ready (vtx %u, tag %u)\n",
                     static_cast<unsigned>(data.vtxCount), aurora_draw_tag);
      }
    }
    return;
  }

  // AURORA_ONLY_UNTEX_VTX=<n>: render ONLY untextured draws with n vertices
  // and drop everything else, so the offending geometry can be seen in
  // isolation on a black frame. Suppressing a draw proves it is responsible;
  // isolating it shows WHAT it is.
  {
    static const long onlyN = [] {
      const char* v = std::getenv("AURORA_ONLY_UNTEX_VTX");
      return v != nullptr ? std::strtol(v, nullptr, 10) : 0L;
    }();
    if (onlyN != 0 &&
        !(!data.bindGroups.textureBindGroup && static_cast<long>(data.vtxCount) == onlyN)) {
      return;
    }
  }

  if (!data.bindGroups.textureBindGroup) {
    static const bool skip = env_flag("AURORA_SKIP_UNTEX");
    static const bool log = env_flag("AURORA_LOG_UNTEX");
    // AURORA_SKIP_UNTEX_VTX=<n>: only drop untextured draws with exactly n
    // vertices, so a suspect quad can be removed without also removing the
    // legitimate untextured geometry that shares this path.
    static const long onlyVtx = [] {
      const char* v = std::getenv("AURORA_SKIP_UNTEX_VTX");
      return v != nullptr ? std::strtol(v, nullptr, 10) : 0L;
    }();
    // Untextured quads are the shape the artifact takes, so report those
    // individually and the rest only sparsely. The per-vertex byte stride
    // discriminates a 2D sprite quad (XY + ST floats = 16B) from 3D geometry.
    if (log) {
      static uint64_t n = 0;
      const bool quad = data.vtxCount == 4;
      if (quad || (n % 2000) == 0) {
        const uint32_t stride = data.vtxCount != 0 ? data.vertRange.size / data.vtxCount : 0;
        fmt::print(stderr, "untex draw #{}{}: idx={} vtx={} stride={} tag={}\n", n, quad ? " QUAD" : "",
                   data.indexCount, data.vtxCount, stride, data.tag);
      }
      ++n;
    }
    if (onlyVtx != 0) {
      if (static_cast<long>(data.vtxCount) == onlyVtx) {
        return;
      }
    } else if (skip) {
      return;
    }
  }

  const auto& resources = gfx::detail::resources();
#ifdef __EMSCRIPTEN__
  const std::array immediateOffsets{data.immediateRange.offset};
  pass.SetBindGroup(3, resources.uniformBindGroup, immediateOffsets.size(), immediateOffsets.data());
#else
  pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
#endif
  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  // The motion pass draws a clipped draw whole: its clip test discards what
  // the eyes leave out, and the eyes' solid part (xrSolid*) is in it too.
  const bool xrCulled = gfx::xr_multiview_replay() && !gfx::xr_motion_replay() && data.xrIndexCount != 0;
#else
  constexpr bool xrCulled = false;
#endif
  const gfx::Range& idxRange = xrCulled ? data.xrIdxRange : data.idxRange;
  pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, idxRange.offset, idxRange.size);
  if (data.posRange.size != 0) {
    pass.SetVertexBuffer(0, resources.storageBuffer, data.posRange.offset, data.posRange.size);
  }
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  if (xrCulled) {
    pass.DrawIndexed(data.xrIndexCount, data.instanceCount);
  } else if (data.indexCount == 0) {
    pass.Draw(data.vtxCount, data.instanceCount);
  } else {
    pass.DrawIndexed(data.indexCount, data.instanceCount);
  }
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  // The same draw's triangles inside the clip's solid region, without the
  // discard (same bind groups and buffers; the pipelines share a layout).
  if (gfx::xr_multiview_replay() && !gfx::xr_motion_replay() && data.xrSolidIndexCount != 0 &&
      gfx::bind_pipeline(data.xrSolidPipeline, pass)) {
    pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
    if (data.dstAlpha != UINT32_MAX) {
      const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
      pass.SetBlendConstant(&color);
    }
    pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.xrSolidIdxRange.offset,
                        data.xrSolidIdxRange.size);
    pass.DrawIndexed(data.xrSolidIndexCount, data.instanceCount);
  }
#endif
}

#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
// XR batching (AURORA_XR_BATCH): in an eye or motion replay, a run of
// consecutive draws that differ only in their matrices and per-draw
// constants goes as one draw. Their indices, rebased onto the storage pool's
// start (decoded vertices are aligned to their stride for this), are joined
// into one 32-bit list; each draw's push constants and uniform offset go in a
// record, and a table of (first vertex, record) lets each vertex find its
// draw (the batched shader variant, gx/shader.cpp). Eye draws batch with the
// same pipeline, textures, blend constant, fog and uniform but for the
// position and normal matrices; motion draws with the same pipeline. The data
// goes in this frame's slot of the batch buffer, uploaded at the replay's end.
namespace {
struct XrBatcher {
  bool active = false;
  bool motion = false;
  const uint8_t* shadow = nullptr; // the frame's CPU copy of its indices
  uint64_t frameId = UINT64_MAX;
  uint64_t used = 0, uploaded = 0; // bytes of this frame's slot
  std::vector<uint8_t> staging = std::vector<uint8_t>(gfx::detail::XrBatchSlotSize);
  std::vector<const DrawData*> members;
  uint32_t memberIndices = 0;
  uint64_t draws = 0, batches = 0, batchedDraws = 0;
} sXrBatch;

gfx::PipelineRef batch_pipeline(const DrawData& d) {
  return sXrBatch.motion ? d.xrMotionBatchPipeline : d.xrBatchPipeline;
}

uint32_t draw_index_count(const DrawData& d) {
  return d.xrIndexCount != 0 ? d.xrIndexCount : d.indexCount != 0 ? d.indexCount : d.vtxCount;
}

bool batchable(const DrawData& d) {
  return batch_pipeline(d) != 0 && d.instanceCount == 1 && d.posRange.size != 0 && d.vtxCount != 0 &&
         d.posRange.size % d.vtxCount == 0 && d.posRange.offset % (d.posRange.size / d.vtxCount) == 0 &&
         (sXrBatch.motion || d.xrSolidIndexCount == 0);
}

bool joins(const DrawData& first, const DrawData& d) {
  if (batch_pipeline(d) != batch_pipeline(first) || sXrBatch.members.size() >= 128 ||
      sXrBatch.memberIndices + draw_index_count(d) > 65536 * 4)
    return false;
  if (sXrBatch.motion)
    return true;
  return d.bindGroups.textureBindGroup == first.bindGroups.textureBindGroup && d.dstAlpha == first.dstAlpha &&
         d.uniformSharedKey == first.uniformSharedKey &&
         d.immediateData.fogRangeBase == first.immediateData.fogRangeBase;
}

// Bytes in this frame's slot, 16-aligned (null: full).
uint8_t* slot_alloc(uint64_t bytes, uint64_t& offset) {
  auto& b = sXrBatch;
  const uint64_t at = (b.used + 15) & ~uint64_t{15};
  if (at + bytes > gfx::detail::XrBatchSlotSize)
    return nullptr;
  b.used = at + bytes;
  offset = (b.frameId % gfx::detail::XrBatchSlots) * gfx::detail::XrBatchSlotSize + at;
  return b.staging.data() + at;
}

void emit_batch(const wgpu::RenderPassEncoder& pass) {
  auto& b = sXrBatch;
  const auto& first = *b.members.front();
  const auto n = static_cast<uint32_t>(b.members.size());
  constexpr uint32_t kRecordWords = 17;
  uint64_t idxOffset = 0, tableOffset = 0;
  auto* idx = reinterpret_cast<uint32_t*>(slot_alloc(uint64_t{b.memberIndices} * 4, idxOffset));
  auto* table = idx != nullptr ? reinterpret_cast<uint32_t*>(slot_alloc(uint64_t{n} * (2 + kRecordWords) * 4, tableOffset))
                               : nullptr;
  if (table == nullptr || !gfx::bind_pipeline(batch_pipeline(first), pass)) {
    for (const DrawData* d : b.members)
      render_direct(*d, pass);
    return;
  }
  const auto tableWord = static_cast<uint32_t>(tableOffset / 4);
  uint32_t* records = table + n * 2;
  uint32_t written = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const DrawData& d = *b.members[i];
    const uint32_t stride = d.posRange.size / d.vtxCount;
    const uint32_t vbase = d.posRange.offset / stride;
    uint32_t* rec = records + i * kRecordWords;
    static_assert(sizeof(d.immediateData) == 16 * 4);
    std::memcpy(rec, &d.immediateData, sizeof(d.immediateData));
    rec[16] = d.uniformRange.offset / 4;
    table[i * 2] = vbase;
    table[i * 2 + 1] = tableWord + n * 2 + i * kRecordWords;
    if (d.indexCount == 0) {
      for (uint32_t v = 0; v < d.vtxCount; ++v)
        idx[written++] = vbase + v;
    } else {
      const gfx::Range& r = d.xrIndexCount != 0 ? d.xrIdxRange : d.idxRange;
      const uint32_t count = d.xrIndexCount != 0 ? d.xrIndexCount : d.indexCount;
      const auto* src = reinterpret_cast<const uint16_t*>(b.shadow + r.offset);
      for (uint32_t k = 0; k < count; ++k)
        idx[written++] = vbase + src[k];
    }
  }
  // Table entries by first vertex (draws push their vertices in order, so
  // this is almost always already so).
  bool sorted = true;
  for (uint32_t i = 1; i < n; ++i)
    sorted &= table[i * 2] > table[(i - 1) * 2];
  if (!sorted) {
    std::vector<std::pair<uint32_t, uint32_t>> entries(n);
    for (uint32_t i = 0; i < n; ++i)
      entries[i] = {table[i * 2], table[i * 2 + 1]};
    std::sort(entries.begin(), entries.end());
    for (uint32_t i = 0; i < n; ++i) {
      table[i * 2] = entries[i].first;
      table[i * 2 + 1] = entries[i].second;
    }
  }
  const auto& resources = gfx::detail::resources();
  const struct {
    uint32_t table, count, fogRangeBase, pad;
  } bimm{tableWord, n, first.immediateData.fogRangeBase, 0};
  pass.SetImmediates(0, &bimm, sizeof(bimm));
  const std::array offsets{first.uniformRange.offset};
  pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (first.bindGroups.textureBindGroup)
    pass.SetBindGroup(2, gfx::find_bind_group(first.bindGroups.textureBindGroup));
  pass.SetIndexBuffer(resources.xrBatchBuffer, wgpu::IndexFormat::Uint32, idxOffset, uint64_t{written} * 4);
  pass.SetVertexBuffer(0, resources.storageBuffer);
  if (first.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, first.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  pass.DrawIndexed(written);
  ++b.batches;
  b.batchedDraws += n;
}
} // namespace

void xr_batch_begin(const gfx::detail::FramePacket& frame, bool multiview, bool motion) {
  auto& b = sXrBatch;
  b.active = multiview && gfx::detail::xr_batching() && frame.indexShadow != nullptr;
  b.motion = motion;
  b.shadow = frame.indexShadow.get();
  if (frame.frameId != b.frameId) {
    b.frameId = frame.frameId;
    b.used = b.uploaded = 0;
  }
  b.members.clear();
  b.memberIndices = 0;
}

void xr_batch_flush(const wgpu::RenderPassEncoder& pass) {
  auto& b = sXrBatch;
  if (b.members.empty())
    return;
  if (b.members.size() == 1)
    render_direct(*b.members.front(), pass);
  else
    emit_batch(pass);
  b.members.clear();
  b.memberIndices = 0;
}

void xr_batch_end() {
  auto& b = sXrBatch;
  if (!b.active)
    return;
  b.active = false;
  if (b.used > b.uploaded) {
    const uint64_t base = (b.frameId % gfx::detail::XrBatchSlots) * gfx::detail::XrBatchSlotSize;
    webgpu::g_queue.WriteBuffer(gfx::detail::resources().xrBatchBuffer, base + b.uploaded, b.staging.data() + b.uploaded,
                             b.used - b.uploaded);
    b.uploaded = b.used;
  }
  // AURORA_XR_BATCH_STATS=1: replayed draws against draws actually issued.
  static const bool stats = env_flag("AURORA_XR_BATCH_STATS");
  if (stats && b.draws >= 50000) {
    Log.info("XR batching: {} replayed draws in {} batches of {} draws, the rest alone", b.draws, b.batches,
             b.batchedDraws);
    b.draws = b.batches = b.batchedDraws = 0;
  }
}
#endif

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
#if defined(AURORA_ENABLE_OPENXR) && !defined(__EMSCRIPTEN__)
  auto& b = sXrBatch;
  if (b.active) {
    ++b.draws;
    if (!gfx::xr_multiview_replay() || !batchable(data) ||
        (!gfx::xr_motion_replay() && xr_link_filter(data))) {
      xr_batch_flush(pass);
      render_direct(data, pass);
      return;
    }
    if (!b.members.empty() && !joins(*b.members.front(), data))
      xr_batch_flush(pass);
    b.members.push_back(&data);
    b.memberIndices += draw_index_count(data);
    return;
  }
#endif
  render_direct(data, pass);
}

} // namespace aurora::gx
