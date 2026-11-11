/**
 * src/backend/d3d12/state.cpp — the dynamic state a D3D12 command list still has.
 *
 * Hooked: IASetPrimitiveTopology, RSSetViewports, RSSetScissorRects,
 * OMSetBlendFactor, OMSetStencilRef.
 *
 * Blend, depth-stencil and rasterizer *descriptions* are baked into a pipeline
 * state object at creation, and binding one is SetPipelineState, not a state
 * call; they reach callbacks through on_pso_create, which carries the whole
 * description. So a D3D12 StateChangeEvent raises the topology, viewport and
 * scissor flags, and the blend and depth-stencil flags only for the two values
 * that really are dynamic. Each change is also folded into the list's own
 * PipelineState, which FrameContext::pipeline() reports.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <state_merge.hpp>
#include <vangfx/events/state.hpp>

#include <algorithm>

namespace vangfx::backend::d3d12 {

namespace lst = ::vangfx::d3d12_slots::graphics_command_list;

namespace {

using TopologyFn  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
using ViewportsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using ScissorsFn  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
using BlendFn     = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const FLOAT[4]);
using StencilFn   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT);

hooks::Site s_topology { lst::IASetPrimitiveTopology, "IASetPrimitiveTopology" };
hooks::Site s_viewports{ lst::RSSetViewports,         "RSSetViewports" };
hooks::Site s_scissors { lst::RSSetScissorRects,      "RSSetScissorRects" };
hooks::Site s_blend    { lst::OMSetBlendFactor,       "OMSetBlendFactor" };
hooks::Site s_stencil  { lst::OMSetStencilRef,        "OMSetStencilRef" };

PrimitiveTopology to_topology(D3D12_PRIMITIVE_TOPOLOGY t) noexcept {
    switch (t) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:     return PrimitiveTopology::PointList;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:      return PrimitiveTopology::LineList;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:     return PrimitiveTopology::LineStrip;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return PrimitiveTopology::TriangleStrip;
    default:                                    return PrimitiveTopology::TriangleList;
    }
}

// Folds the change into the list's state and tells whoever listens.
template <typename Build>
void state_change(ID3D12GraphicsCommandList* self, Build&& build) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    ListState* st = list_state(self);
    if (!st) return;
    StateChangeEvent ev{};
    build(ev);
    state_merge::merge(st->pipeline, ev);
    for (D3D12ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_state()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index(), nullptr, self, st);
        ctx->dispatch_state(fc, ev);
    }
}

void STDMETHODCALLTYPE hook_topology(ID3D12GraphicsCommandList* self, D3D12_PRIMITIVE_TOPOLOGY t) {
    s_topology.original<TopologyFn>(self)(self, t);
    state_change(self, [&](StateChangeEvent& ev) { ev.dirty = StateDirty_Topology; ev.topology = to_topology(t); });
}

void STDMETHODCALLTYPE hook_viewports(ID3D12GraphicsCommandList* self, UINT n, const D3D12_VIEWPORT* vps) {
    s_viewports.original<ViewportsFn>(self)(self, n, vps);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_Viewports;
        const UINT count = std::min<UINT>(n, static_cast<UINT>(std::size(ev.viewports)));
        for (UINT i = 0; vps && i < count; ++i) {
            ev.viewports[i].x = vps[i].TopLeftX;       ev.viewports[i].y = vps[i].TopLeftY;
            ev.viewports[i].width = vps[i].Width;      ev.viewports[i].height = vps[i].Height;
            ev.viewports[i].min_depth = vps[i].MinDepth; ev.viewports[i].max_depth = vps[i].MaxDepth;
        }
    });
}

void STDMETHODCALLTYPE hook_scissors(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RECT* rects) {
    s_scissors.original<ScissorsFn>(self)(self, n, rects);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_Scissors;
        const UINT count = std::min<UINT>(n, static_cast<UINT>(std::size(ev.scissors)));
        for (UINT i = 0; rects && i < count; ++i) {
            ev.scissors[i].left = rects[i].left;   ev.scissors[i].top = rects[i].top;
            ev.scissors[i].right = rects[i].right; ev.scissors[i].bottom = rects[i].bottom;
        }
    });
}

void STDMETHODCALLTYPE hook_blend(ID3D12GraphicsCommandList* self, const FLOAT factor[4]) {
    s_blend.original<BlendFn>(self)(self, factor);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_Blend;
        for (int i = 0; i < 4; ++i) ev.blend.blend_factor[i] = factor ? factor[i] : 1.0f;
    });
}

void STDMETHODCALLTYPE hook_stencil(ID3D12GraphicsCommandList* self, UINT ref) {
    s_stencil.original<StencilFn>(self)(self, ref);
    state_change(self, [&](StateChangeEvent& ev) { ev.dirty = StateDirty_DepthStencil; ev.depth.stencil_ref = ref; });
}

} // namespace

void install_state_hooks(ID3D12GraphicsCommandList* list) noexcept {
    s_topology.install(list, &hook_topology);
    s_viewports.install(list, &hook_viewports);
    s_scissors.install(list, &hook_scissors);
    s_blend.install(list, &hook_blend);
    s_stencil.install(list, &hook_stencil);
}

void uninstall_state_hooks() noexcept {
    for (hooks::Site* s : { &s_topology, &s_viewports, &s_scissors, &s_blend, &s_stencil }) s->uninstall();
}

} // namespace vangfx::backend::d3d12
