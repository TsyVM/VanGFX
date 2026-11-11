#pragma once

/**
 * vangfx/resources/texture.hpp — Texture handle and descriptor.
 *
 * TextureDesc covers D3D9, D3D11, and D3D12 textures.  Fields prefixed with
 * "d3d12_" are only used when the backend is D3D12; they are ignored on D3D9
 * and D3D11.  The D3D12-specific values mirror D3D12_RESOURCE_DESC /
 * D3D12_HEAP_PROPERTIES so they can be forwarded without conversion.
 */

#include "format.hpp"
#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  TextureHandle
// ─────────────────────────────────────────────────────────────────────────────

struct TextureHandle { uint64_t id = 0; };

[[nodiscard]] inline bool is_valid(TextureHandle h) noexcept { return h.id != 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  TextureType
// ─────────────────────────────────────────────────────────────────────────────

enum class TextureType : uint8_t {
    Tex2D,
    TexCube,
    TexVolume,    // 3D texture
    TexArray,     // D3D11 texture array
    TexDynamic,   // CPU-written each frame
    Tex1D,        // D3D12_RESOURCE_DIMENSION_TEXTURE1D
};

// ─────────────────────────────────────────────────────────────────────────────
//  TextureDesc
// ─────────────────────────────────────────────────────────────────────────────

struct TextureDesc {
    TextureType   type              = TextureType::Tex2D;
    SurfaceFormat format            = SurfaceFormat::Unknown;
    uint32_t      width             = 0;
    uint32_t      height            = 0;
    uint32_t      depth             = 1;          // volume textures
    uint32_t      array_size        = 1;          // texture arrays
    uint32_t      mip_levels        = 1;
    uint32_t      msaa_samples      = 1;
    bool          is_render_target  = false;
    bool          is_depth          = false;
    bool          is_uav            = false;      // D3D11/D3D12 — bound as UAV
    bool          is_dynamic        = false;

    // ── D3D12-only fields (ignored on D3D9/D3D11) ─────────────────────────────

    // Initial resource state at creation time.
    // Mirrors D3D12_RESOURCE_STATES; use d3d12::ResourceState cast to uint32_t.
    // Common values: 0 = Common, 0x0004 = RenderTarget, 0x0010 = DepthWrite.
    uint32_t      d3d12_initial_state           = 0;  // ResourceState::Common

    // Heap type for committed resources.
    // 0 = Default (GPU-only), 1 = Upload (CPU→GPU), 2 = Readback (GPU→CPU).
    uint8_t       d3d12_heap_type               = 0;  // HeapType::Default

    // D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS.
    // Required for cross-queue access without explicit barriers.
    bool          d3d12_allow_simultaneous_access = false;

    // D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE.
    // Set when is_depth is true and SRV read-back is not needed; saves memory.
    bool          d3d12_deny_shader_resource    = false;

    // Alignment override: 0 = driver default (64KB for MSAA, 4MB for large),
    // 65536 = 64KB (forced small), 4194304 = 4MB.
    uint64_t      d3d12_alignment               = 0;
};

} // namespace vangfx
