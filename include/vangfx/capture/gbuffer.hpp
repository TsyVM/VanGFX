#pragma once

/**
 * vangfx/capture/gbuffer.hpp — GBuffer layer identifiers and per-layer data.
 *
 * When a capture is active, vangfx identifies and resolves each render target
 * slot as a semantic GBuffer layer (albedo, normals, depth, etc.).  The
 * resolved layers are stored in a GBufferCapture and returned by
 * FrameContext::latest_capture().
 *
 * Layer availability depends on the game's rendering technique; always check
 * GBufferLayer::available before accessing pixel data.
 */

#include <vangfx/resources/buffer.hpp>
#include <vangfx/resources/surface.hpp>
#include <vangfx/resources/texture.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  GBufferLayerKind — semantic label assigned to each captured render target
// ─────────────────────────────────────────────────────────────────────────────

enum class GBufferLayerKind : uint8_t {
    Albedo,             // base colour / diffuse
    Normal,             // world-space or view-space normals
    Depth,              // linearised scene depth (0 = near, 1 = far)
    DepthRaw,           // raw hardware depth buffer contents
    Specular,           // specular colour or roughness/metallic pack
    Emissive,           // emissive / glow layer
    Motion,             // screen-space motion vectors (D3D11 engines)
    AmbientOcclusion,   // SSAO or baked AO
    Velocity,           // velocity buffer (alternate name for motion in some engines)
    Backbuffer,         // resolved final backbuffer image
    Unknown,            // unclassified render target slot
};

// ─────────────────────────────────────────────────────────────────────────────
//  GBufferLayer — a single resolved GBuffer plane
// ─────────────────────────────────────────────────────────────────────────────

struct GBufferLayer {
    GBufferLayerKind   kind      = GBufferLayerKind::Unknown;
    bool               available = false; // false → pixels is empty
    TextureHandle      texture   = {};    // GPU-side texture (may be invalid after frame)
    SurfaceFormat      format    = SurfaceFormat::Unknown;
    uint32_t           width     = 0;
    uint32_t           height    = 0;
    uint32_t           mip       = 0;     // resolved mip level (usually 0)

    // CPU-side readback (populated only when capture resolves CPU readback).
    // Valid for the lifetime of the containing GBufferCapture only.
    std::span<const uint8_t> pixels;
    uint32_t                 row_pitch = 0;  // bytes per scanline (may include padding)
};

// ─────────────────────────────────────────────────────────────────────────────
//  GBufferCapture — all layers from a single captured frame
// ─────────────────────────────────────────────────────────────────────────────

struct GBufferCapture {
    uint32_t frame_index = 0;   // value of FrameContext::frame_index() at capture time
    uint32_t width       = 0;   // backbuffer resolution at capture time
    uint32_t height      = 0;

    // Up to 8 colour layers (MRT slots 0-7) plus the depth buffer.
    static constexpr uint32_t kMaxLayers = 9; // 8 colour + 1 depth
    GBufferLayer layers[kMaxLayers] = {};
    uint32_t     layer_count        = 0;

    // Convenience accessors — return nullptr if the layer is not available.
    [[nodiscard]] const GBufferLayer* find(GBufferLayerKind kind) const noexcept {
        for (uint32_t i = 0; i < layer_count; ++i)
            if (layers[i].kind == kind && layers[i].available)
                return &layers[i];
        return nullptr;
    }
    [[nodiscard]] GBufferLayer* find(GBufferLayerKind kind) noexcept {
        for (uint32_t i = 0; i < layer_count; ++i)
            if (layers[i].kind == kind && layers[i].available)
                return &layers[i];
        return nullptr;
    }

    [[nodiscard]] bool has(GBufferLayerKind kind) const noexcept {
        return find(kind) != nullptr;
    }

    // Human-readable label for logging / UI.
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
