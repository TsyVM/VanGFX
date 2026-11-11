#pragma once

/**
 * vangfx/capture/gbuffer.hpp — the G-buffer view of a captured frame.
 *
 * A capture reads back the surfaces a frame rendered into (see capture.hpp);
 * this is the same frame sorted into the semantic layers of a deferred
 * renderer -- albedo, normals, depth and so on -- at most one target per
 * layer, chosen by format, size and where each target sat in the frame's
 * multiple-render-target bindings. Games do not label their G-buffers, so the
 * sorting is a heuristic; FrameCapture has every surface whatever this made of
 * it.
 *
 * Always check GBufferLayer::available before reading pixels.
 */

#include <vangfx/resources/buffer.hpp>
#include <vangfx/resources/surface.hpp>
#include <vangfx/resources/texture.hpp>
#include <cstdint>
#include <memory>
#include <span>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  GBufferLayerKind — semantic label assigned to a captured render target
// ─────────────────────────────────────────────────────────────────────────────

enum class GBufferLayerKind : uint8_t {
    Albedo,             // base colour / diffuse
    Normal,             // world-space or view-space normals
    Depth,              // linearised scene depth written by a shader
    DepthRaw,           // the depth-stencil buffer itself
    Specular,           // specular colour or roughness/metallic pack
    Emissive,           // emissive / glow layer
    Motion,             // screen-space motion vectors
    AmbientOcclusion,   // SSAO or baked AO
    Velocity,           // velocity buffer (alternate name for motion in some engines)
    Backbuffer,         // the final image presented
    Unknown,            // not recognised as a G-buffer layer
};

// ─────────────────────────────────────────────────────────────────────────────
//  GBufferLayer — a single resolved G-buffer plane
// ─────────────────────────────────────────────────────────────────────────────

struct GBufferLayer {
    GBufferLayerKind   kind        = GBufferLayerKind::Unknown;
    bool               available   = false; // false → pixels is empty
    TextureHandle      texture     = {};    // the resource's id, as resource events name it
    SurfaceFormat      format      = SurfaceFormat::Unknown;
    uint32_t           width       = 0;
    uint32_t           height      = 0;
    uint32_t           mip         = 0;     // always 0: the top level is what is rendered into

    // The pixels, rows `row_pitch` bytes apart. Valid for as long as the
    // GBufferCapture (or a copy of it) is.
    std::span<const uint8_t> pixels;
    uint32_t                 row_pitch = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  GBufferCapture — all layers from a single captured frame
// ─────────────────────────────────────────────────────────────────────────────

struct GBufferCapture {
    uint32_t frame_index = 0;   // FrameContext::frame_index() of the frame captured
    uint32_t width       = 0;   // back buffer size
    uint32_t height      = 0;

    static constexpr uint32_t kMaxLayers = 11;   // one per GBufferLayerKind
    GBufferLayer layers[kMaxLayers] = {};
    uint32_t     layer_count        = 0;

    // Keeps the pixel memory the layers point into alive; shared with the
    // FrameCapture the layers came from.
    std::shared_ptr<const void> storage;

    // The layer of a kind, or null when the frame has none (or it could not
    // be read back).
    [[nodiscard]] const GBufferLayer* find(GBufferLayerKind kind) const noexcept {
        for (uint32_t i = 0; i < layer_count; ++i)
            if (layers[i].kind == kind && layers[i].available)
                return &layers[i];
        return nullptr;
    }
    [[nodiscard]] bool has(GBufferLayerKind kind) const noexcept { return find(kind) != nullptr; }

    // Human-readable label for logging / UI, and the name files are written under.
    [[nodiscard]] static const char* layer_name(GBufferLayerKind kind) noexcept {
        switch (kind) {
            case GBufferLayerKind::Albedo:            return "Albedo";
            case GBufferLayerKind::Normal:            return "Normal";
            case GBufferLayerKind::Depth:             return "Depth";
            case GBufferLayerKind::DepthRaw:          return "DepthRaw";
            case GBufferLayerKind::Specular:          return "Specular";
            case GBufferLayerKind::Emissive:          return "Emissive";
            case GBufferLayerKind::Motion:            return "Motion";
            case GBufferLayerKind::AmbientOcclusion:  return "AmbientOcclusion";
            case GBufferLayerKind::Velocity:          return "Velocity";
            case GBufferLayerKind::Backbuffer:        return "Backbuffer";
            default:                                  return "Unknown";
        }
    }
};

} // namespace vangfx
