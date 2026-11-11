#pragma once

/**
 * vangfx/resources/surface.hpp — Render target and depth buffer types.
 *
 * All GPU surface resources are represented as opaque handles.
 * The backend manages lifetime — a handle is valid as long as the
 * underlying D3D resource exists.
 */

#include "format.hpp"
#include <array>
#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  Opaque handle types
// ─────────────────────────────────────────────────────────────────────────────

struct RenderTargetHandle { uint64_t id = 0; };
struct DepthBufferHandle  { uint64_t id = 0; };
struct SurfaceHandle      { uint64_t id = 0; }; // generic — RT or depth

[[nodiscard]] inline bool is_valid(RenderTargetHandle h) noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(DepthBufferHandle h)  noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(SurfaceHandle h)      noexcept { return h.id != 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  GBufferSet — a Multiple-Render-Target set with up to 8 color targets
// ─────────────────────────────────────────────────────────────────────────────

struct GBufferSet {
    std::array<RenderTargetHandle, 8> targets  = {};
    uint32_t                          count    = 0;    // active target count
    DepthBufferHandle                 depth    = {};
    uint32_t                          width    = 0;
    uint32_t                          height   = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  FrameSurfaces — snapshot of all surfaces currently bound on the device
// ─────────────────────────────────────────────────────────────────────────────

struct FrameSurfaces {
    RenderTargetHandle backbuffer         = {};
    DepthBufferHandle  depth              = {};
    GBufferSet         current_rt_set     = {};
    uint32_t           width              = 0;
    uint32_t           height             = 0;
    uint32_t           msaa_samples       = 1;   // 1 = no MSAA
    SurfaceFormat      backbuffer_format  = SurfaceFormat::Unknown;
    SurfaceFormat      depth_format       = SurfaceFormat::Unknown;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Descriptors
// ─────────────────────────────────────────────────────────────────────────────

struct RenderTargetDesc {
    uint32_t      width        = 0;
    uint32_t      height       = 0;
    SurfaceFormat format       = SurfaceFormat::Unknown;
    uint32_t      msaa_samples = 1;
    bool          is_cube      = false;
    uint32_t      cube_face    = 0;   // 0-5 (+X/-X/+Y/-Y/+Z/-Z) when is_cube
    uint32_t      array_slice  = 0;   // D3D11 texture arrays
    uint32_t      mip_level    = 0;
};

struct DepthBufferDesc {
    uint32_t      width        = 0;
    uint32_t      height       = 0;
    SurfaceFormat format       = SurfaceFormat::Unknown;
    uint32_t      msaa_samples = 1;
    bool          has_stencil  = false;
};

} // namespace vangfx
