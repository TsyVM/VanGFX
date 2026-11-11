#pragma once

/**
 * src/internal/state_merge.hpp — folding a StateChangeEvent into the running
 * PipelineState a context tracks.
 *
 * FrameContext::pipeline() is meant to report the pipeline state as it stands
 * right now. Every backend returned an empty struct, because nothing ever
 * accumulated one: the state hooks built a StateChangeEvent, dispatched it, and
 * dropped it. A callback could see that blending had changed and never what it
 * had changed to unless it happened to be in the state callback at the time.
 *
 * A StateChangeEvent is a delta with a dirty mask, so folding it in is only a
 * matter of copying the blocks the mask names. Shared by all three backends
 * because the event type is, and because three copies of this would drift.
 */

#include <vangfx/events/state.hpp>
#include <vangfx/pipeline/state.hpp>

#include <algorithm>

namespace vangfx::state_merge {

// Copies the blocks `ev.dirty` names from the event into `out`, leaving the
// rest as they were. The event's undirty fields carry stale values by design,
// so copying wholesale would overwrite good state with meaningless state.
inline void merge(PipelineState& out, const StateChangeEvent& ev) noexcept
{
    if (ev.dirty & StateDirty_Blend)        out.blend         = ev.blend;
    if (ev.dirty & StateDirty_DepthStencil) out.depth_stencil = ev.depth;
    if (ev.dirty & StateDirty_Rasterizer)   out.raster        = ev.raster;
    if (ev.dirty & StateDirty_Topology)     out.topology      = ev.topology;

    if (ev.dirty & StateDirty_Samplers)
        std::copy(std::begin(ev.samplers), std::end(ev.samplers),
                  std::begin(out.samplers));

    if (ev.dirty & StateDirty_Viewports)
        std::copy(std::begin(ev.viewports), std::end(ev.viewports),
                  std::begin(out.viewports));

    if (ev.dirty & StateDirty_Scissors)
        std::copy(std::begin(ev.scissors), std::end(ev.scissors),
                  std::begin(out.scissors));

    // StateDirty_VertexDecl has no counterpart on the event: PipelineState
    // carries a VertexDeclaration and StateChangeEvent does not, so the flag
    // can be raised but nothing can be read from it. A backend that tracks the
    // declaration sets it on the context directly.
}

}  // namespace vangfx::state_merge
