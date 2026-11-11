#pragma once

/**
 * vangfx/events/state.hpp — Pipeline state change event.
 *
 * Fired whenever the game changes blend, depth/stencil, rasterizer, sampler,
 * viewport, scissor, topology, or vertex declaration state.
 *
 * event.dirty is a bitmask of StateDirtyFlags indicating which fields changed.
 * Only the flagged fields are valid; others carry their previous values.
 */

#include <vangfx/pipeline/state.hpp>
#include <vangfx/pipeline/viewport.hpp>
#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  StateDirtyFlags — bitmask of which state blocks changed
// ─────────────────────────────────────────────────────────────────────────────

enum StateDirtyFlags : uint32_t {
    StateDirty_Blend        = 1u << 0,
    StateDirty_DepthStencil = 1u << 1,
    StateDirty_Rasterizer   = 1u << 2,
    StateDirty_Samplers     = 1u << 3,
    StateDirty_Viewports    = 1u << 4,
    StateDirty_Scissors     = 1u << 5,
    StateDirty_Topology     = 1u << 6,
    StateDirty_VertexDecl   = 1u << 7,
};

// ─────────────────────────────────────────────────────────────────────────────
//  StateChangeEvent
// ─────────────────────────────────────────────────────────────────────────────

struct StateChangeEvent {
    uint32_t          dirty    = 0;     // bitmask of StateDirtyFlags
    BlendState        blend    = {};
    DepthStencilState depth    = {};
    RasterizerState   raster   = {};
    SamplerState      samplers[16] = {};
    Viewport          viewports[8] = {};
    ScissorRect       scissors[8]  = {};
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
};

} // namespace vangfx
