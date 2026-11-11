/**
 * src/backend/d3d11/state.cpp — pipeline state on a device context.
 *
 * Hooked (Heal::SameModule, see d3d11_impl.hpp): RSSetState, RSSetViewports,
 * RSSetScissorRects, OMSetBlendState, OMSetDepthStencilState,
 * IASetPrimitiveTopology, PSSetSamplers, VSSetSamplers.
 *
 * Each call is chained first, then described in a StateChangeEvent, folded into
 * the device context's PipelineState (what FrameContext::pipeline() reports)
 * and dispatched.
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>
#include <state_merge.hpp>
#include <vangfx/events/state.hpp>

namespace vangfx::backend::d3d11 {

namespace dcs = ::vangfx::d3d11_slots::device_context;
using hooks::Heal;

namespace {

// ── D3D11 → VanGFX ───────────────────────────────────────────────────────────

CompareFunc translate_compare(D3D11_COMPARISON_FUNC f) noexcept {
    switch (f) {
    case D3D11_COMPARISON_NEVER:         return CompareFunc::Never;
    case D3D11_COMPARISON_LESS:          return CompareFunc::Less;
    case D3D11_COMPARISON_EQUAL:         return CompareFunc::Equal;
    case D3D11_COMPARISON_LESS_EQUAL:    return CompareFunc::LessEqual;
    case D3D11_COMPARISON_GREATER:       return CompareFunc::Greater;
    case D3D11_COMPARISON_NOT_EQUAL:     return CompareFunc::NotEqual;
    case D3D11_COMPARISON_GREATER_EQUAL: return CompareFunc::GreaterEqual;
    case D3D11_COMPARISON_ALWAYS:        return CompareFunc::Always;
    default:                             return CompareFunc::Never;
    }
}

StencilOpMode translate_stencil_op(D3D11_STENCIL_OP op) noexcept {
    switch (op) {
    case D3D11_STENCIL_OP_ZERO:     return StencilOpMode::Zero;
    case D3D11_STENCIL_OP_REPLACE:  return StencilOpMode::Replace;
    case D3D11_STENCIL_OP_INCR_SAT: return StencilOpMode::IncrSat;
    case D3D11_STENCIL_OP_DECR_SAT: return StencilOpMode::DecrSat;
    case D3D11_STENCIL_OP_INVERT:   return StencilOpMode::Invert;
    case D3D11_STENCIL_OP_INCR:     return StencilOpMode::Incr;
    case D3D11_STENCIL_OP_DECR:     return StencilOpMode::Decr;
    default:                        return StencilOpMode::Keep;
    }
}

BlendFactor translate_blend_factor(D3D11_BLEND b) noexcept {
    switch (b) {
    case D3D11_BLEND_ZERO:             return BlendFactor::Zero;
    case D3D11_BLEND_ONE:              return BlendFactor::One;
    case D3D11_BLEND_SRC_COLOR:        return BlendFactor::SrcColor;
    case D3D11_BLEND_INV_SRC_COLOR:    return BlendFactor::InvSrcColor;
    case D3D11_BLEND_SRC_ALPHA:        return BlendFactor::SrcAlpha;
    case D3D11_BLEND_INV_SRC_ALPHA:    return BlendFactor::InvSrcAlpha;
    case D3D11_BLEND_DEST_ALPHA:       return BlendFactor::DstAlpha;
    case D3D11_BLEND_INV_DEST_ALPHA:   return BlendFactor::InvDstAlpha;
    case D3D11_BLEND_DEST_COLOR:       return BlendFactor::DstColor;
    case D3D11_BLEND_INV_DEST_COLOR:   return BlendFactor::InvDstColor;
    case D3D11_BLEND_SRC_ALPHA_SAT:    return BlendFactor::SrcAlphaSat;
    case D3D11_BLEND_BLEND_FACTOR:     return BlendFactor::BlendFactor;
    case D3D11_BLEND_INV_BLEND_FACTOR: return BlendFactor::InvBlendFactor;
    case D3D11_BLEND_SRC1_COLOR:       return BlendFactor::Src1Color;
    case D3D11_BLEND_INV_SRC1_COLOR:   return BlendFactor::InvSrc1Color;
    case D3D11_BLEND_SRC1_ALPHA:       return BlendFactor::Src1Alpha;
    case D3D11_BLEND_INV_SRC1_ALPHA:   return BlendFactor::InvSrc1Alpha;
    default:                           return BlendFactor::Zero;
    }
}

BlendOp translate_blend_op(D3D11_BLEND_OP op) noexcept {
    switch (op) {
    case D3D11_BLEND_OP_SUBTRACT:     return BlendOp::Subtract;
    case D3D11_BLEND_OP_REV_SUBTRACT: return BlendOp::RevSubtract;
    case D3D11_BLEND_OP_MIN:          return BlendOp::Min;
    case D3D11_BLEND_OP_MAX:          return BlendOp::Max;
    default:                          return BlendOp::Add;
    }
}

FilterMode translate_filter(D3D11_FILTER f) noexcept {
    if (f & 0x40) return FilterMode::Anisotropic;
    if (f == D3D11_FILTER_MIN_MAG_MIP_POINT) return FilterMode::Point;
    return FilterMode::Linear;
}

AddressMode translate_address(D3D11_TEXTURE_ADDRESS_MODE m) noexcept {
    switch (m) {
    case D3D11_TEXTURE_ADDRESS_MIRROR:      return AddressMode::Mirror;
    case D3D11_TEXTURE_ADDRESS_CLAMP:       return AddressMode::Clamp;
    case D3D11_TEXTURE_ADDRESS_BORDER:      return AddressMode::Border;
    case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return AddressMode::MirrorOnce;
    default:                                return AddressMode::Wrap;
    }
}

PrimitiveTopology translate_topology(D3D11_PRIMITIVE_TOPOLOGY t) noexcept {
    switch (t) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:         return PrimitiveTopology::PointList;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:          return PrimitiveTopology::LineList;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:         return PrimitiveTopology::LineStrip;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:      return PrimitiveTopology::TriangleList;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:     return PrimitiveTopology::TriangleStrip;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:      return PrimitiveTopology::LineListAdj;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:     return PrimitiveTopology::LineStripAdj;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:  return PrimitiveTopology::TriangleListAdj;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return PrimitiveTopology::TriangleStripAdj;
    default:
        if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            t <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
            const auto cp = static_cast<uint32_t>(t) - static_cast<uint32_t>(D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
            return static_cast<PrimitiveTopology>(static_cast<uint32_t>(PrimitiveTopology::PatchList_1) + cp);
        }
        return PrimitiveTopology::TriangleList;
    }
}

void fill_samplers(ID3D11SamplerState* const* samplers, UINT start, UINT n, StateChangeEvent& ev) noexcept {
    for (UINT i = 0; i < n && start + i < 16; ++i) {
        if (!samplers[i]) continue;
        D3D11_SAMPLER_DESC d{};
        samplers[i]->GetDesc(&d);
        auto& s = ev.samplers[start + i];
        s.filter         = translate_filter(d.Filter);
        s.address_u      = translate_address(d.AddressU);
        s.address_v      = translate_address(d.AddressV);
        s.address_w      = translate_address(d.AddressW);
        s.mip_bias       = d.MipLODBias;
        s.max_anisotropy = d.MaxAnisotropy;
        s.compare_func   = translate_compare(d.ComparisonFunc);
        for (int c = 0; c < 4; ++c) s.border_color[c] = d.BorderColor[c];
        s.min_lod        = d.MinLOD;
        s.max_lod        = d.MaxLOD;
    }
}

// ── Hooks ────────────────────────────────────────────────────────────────────

using RSSetStateFn     = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSSetScissorsFn  = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using OMSetBlendFn     = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
using OMSetDepthFn     = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using IASetTopoFn      = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY);
using SetSamplersFn    = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);

hooks::Site s_rs_state   { dcs::RSSetState,             "RSSetState",             Heal::SameModule };
hooks::Site s_viewports  { dcs::RSSetViewports,         "RSSetViewports",         Heal::SameModule };
hooks::Site s_scissors   { dcs::RSSetScissorRects,      "RSSetScissorRects",      Heal::SameModule };
hooks::Site s_blend      { dcs::OMSetBlendState,        "OMSetBlendState",        Heal::SameModule };
hooks::Site s_depth      { dcs::OMSetDepthStencilState, "OMSetDepthStencilState", Heal::SameModule };
hooks::Site s_topology   { dcs::IASetPrimitiveTopology, "IASetPrimitiveTopology", Heal::SameModule };
hooks::Site s_ps_samplers{ dcs::PSSetSamplers,          "PSSetSamplers",          Heal::SameModule };
hooks::Site s_vs_samplers{ dcs::VSSetSamplers,          "VSSetSamplers",          Heal::SameModule };

// Builds the event, folds it into the context's state and tells whoever listens.
template <typename Build>
void state_change(ID3D11DeviceContext* self, Build&& build) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    DcState* st = dc_state(self);
    if (!st) return;
    StateChangeEvent ev{};
    build(ev);
    state_merge::merge(st->pipeline, ev);
    for (D3D11ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_state()) continue;
        D3D11FrameContextImpl fc(*ctx, ctx->current_frame_index(), self, st);
        ctx->dispatch_state(fc, ev);
    }
}

void STDMETHODCALLTYPE hook_rs_state(ID3D11DeviceContext* self, ID3D11RasterizerState* rs) {
    const Healer healer{ self };
    s_rs_state.original<RSSetStateFn>(self)(self, rs);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_Rasterizer;
        if (!rs) return;
        D3D11_RASTERIZER_DESC d{};
        rs->GetDesc(&d);
        auto& r = ev.raster;
        r.fill_mode = d.FillMode == D3D11_FILL_WIREFRAME ? FillMode::Wireframe : FillMode::Solid;
        r.cull_mode = d.CullMode == D3D11_CULL_NONE ? CullMode::None : d.CullMode == D3D11_CULL_FRONT ? CullMode::Front : CullMode::Back;
        r.front_ccw               = d.FrontCounterClockwise != FALSE;
        r.depth_bias              = d.DepthBias;
        r.depth_bias_clamp        = d.DepthBiasClamp;
        r.slope_scaled_depth_bias = d.SlopeScaledDepthBias;
        r.depth_clip_enable       = d.DepthClipEnable != FALSE;
        r.scissor_enable          = d.ScissorEnable != FALSE;
        r.multisample_enable      = d.MultisampleEnable != FALSE;
        r.antialiased_line        = d.AntialiasedLineEnable != FALSE;
    });
}

void STDMETHODCALLTYPE hook_viewports(ID3D11DeviceContext* self, UINT n, const D3D11_VIEWPORT* vps) {
    const Healer healer{ self };
    s_viewports.original<RSSetViewportsFn>(self)(self, n, vps);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_Viewports;
        for (UINT i = 0; vps && i < n && i < 8; ++i)
            ev.viewports[i] = { vps[i].TopLeftX, vps[i].TopLeftY, vps[i].Width, vps[i].Height, vps[i].MinDepth, vps[i].MaxDepth };
    });
}

void STDMETHODCALLTYPE hook_scissors(ID3D11DeviceContext* self, UINT n, const D3D11_RECT* rects) {
    const Healer healer{ self };
    s_scissors.original<RSSetScissorsFn>(self)(self, n, rects);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_Scissors;
        for (UINT i = 0; rects && i < n && i < 8; ++i)
            ev.scissors[i] = { static_cast<int32_t>(rects[i].left), static_cast<int32_t>(rects[i].top),
                               static_cast<int32_t>(rects[i].right), static_cast<int32_t>(rects[i].bottom) };
    });
}

void STDMETHODCALLTYPE hook_blend(ID3D11DeviceContext* self, ID3D11BlendState* bs, const FLOAT factor[4], UINT mask) {
    const Healer healer{ self };
    s_blend.original<OMSetBlendFn>(self)(self, bs, factor, mask);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_Blend;
        if (factor) for (int i = 0; i < 4; ++i) ev.blend.blend_factor[i] = factor[i];   // null means white
        if (!bs) return;
        D3D11_BLEND_DESC d{};
        bs->GetDesc(&d);
        ev.blend.alpha_to_coverage = d.AlphaToCoverageEnable != FALSE;
        ev.blend.independent_blend = d.IndependentBlendEnable != FALSE;
        for (int i = 0; i < 8; ++i) {
            const auto& s = d.RenderTarget[i];
            auto& t = ev.blend.targets[i];
            t.blend_enable = s.BlendEnable != FALSE;
            t.src_color    = translate_blend_factor(s.SrcBlend);
            t.dst_color    = translate_blend_factor(s.DestBlend);
            t.color_op     = translate_blend_op(s.BlendOp);
            t.src_alpha    = translate_blend_factor(s.SrcBlendAlpha);
            t.dst_alpha    = translate_blend_factor(s.DestBlendAlpha);
            t.alpha_op     = translate_blend_op(s.BlendOpAlpha);
            t.write_mask   = static_cast<uint8_t>(s.RenderTargetWriteMask);
        }
    });
}

void STDMETHODCALLTYPE hook_depth(ID3D11DeviceContext* self, ID3D11DepthStencilState* dss, UINT ref) {
    const Healer healer{ self };
    s_depth.original<OMSetDepthFn>(self)(self, dss, ref);
    state_change(self, [&](StateChangeEvent& ev) {
        ev.dirty = StateDirty_DepthStencil;
        ev.depth.stencil_ref = ref;
        if (!dss) return;
        D3D11_DEPTH_STENCIL_DESC d{};
        dss->GetDesc(&d);
        auto& o = ev.depth;
        o.depth_enable       = d.DepthEnable != FALSE;
        o.depth_write        = d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        o.depth_func         = translate_compare(d.DepthFunc);
        o.stencil_enable     = d.StencilEnable != FALSE;
        o.stencil_read_mask  = d.StencilReadMask;
        o.stencil_write_mask = d.StencilWriteMask;
        auto face = [](const D3D11_DEPTH_STENCILOP_DESC& s, StencilOp& t) {
            t.fail       = translate_stencil_op(s.StencilFailOp);
            t.depth_fail = translate_stencil_op(s.StencilDepthFailOp);
            t.pass       = translate_stencil_op(s.StencilPassOp);
            t.func       = translate_compare(s.StencilFunc);
        };
        face(d.FrontFace, o.front_face);
        face(d.BackFace, o.back_face);
    });
}

void STDMETHODCALLTYPE hook_topology(ID3D11DeviceContext* self, D3D11_PRIMITIVE_TOPOLOGY t) {
    const Healer healer{ self };
    s_topology.original<IASetTopoFn>(self)(self, t);
    state_change(self, [&](StateChangeEvent& ev) { ev.dirty = StateDirty_Topology; ev.topology = translate_topology(t); });
}

void STDMETHODCALLTYPE hook_ps_samplers(ID3D11DeviceContext* self, UINT start, UINT n, ID3D11SamplerState* const* ss) {
    const Healer healer{ self };
    s_ps_samplers.original<SetSamplersFn>(self)(self, start, n, ss);
    state_change(self, [&](StateChangeEvent& ev) { ev.dirty = StateDirty_Samplers; if (ss) fill_samplers(ss, start, n, ev); });
}

void STDMETHODCALLTYPE hook_vs_samplers(ID3D11DeviceContext* self, UINT start, UINT n, ID3D11SamplerState* const* ss) {
    const Healer healer{ self };
    s_vs_samplers.original<SetSamplersFn>(self)(self, start, n, ss);
    state_change(self, [&](StateChangeEvent& ev) { ev.dirty = StateDirty_Samplers; if (ss) fill_samplers(ss, start, n, ev); });
}

} // namespace

void install_state_hooks(ID3D11DeviceContext* context) noexcept {
    s_rs_state.install(context, &hook_rs_state);
    s_viewports.install(context, &hook_viewports);
    s_scissors.install(context, &hook_scissors);
    s_blend.install(context, &hook_blend);
    s_depth.install(context, &hook_depth);
    s_topology.install(context, &hook_topology);
    s_ps_samplers.install(context, &hook_ps_samplers);
    s_vs_samplers.install(context, &hook_vs_samplers);
    for (hooks::Site* s : { &s_rs_state, &s_viewports, &s_scissors, &s_blend, &s_depth, &s_topology,
                            &s_ps_samplers, &s_vs_samplers })
        register_context_site(*s);
}

}  // namespace vangfx::backend::d3d11
