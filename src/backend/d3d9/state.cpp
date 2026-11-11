/**
 * src/backend/d3d9/state.cpp — pipeline state on a device.
 *
 * Hooked on the device's own table (Heal::SameModule): SetRenderState,
 * SetSamplerState, SetViewport, SetScissorRect, SetVertexDeclaration, SetFVF.
 *
 * D3D9 has no state objects: each call sets one value. A StateChangeEvent is a
 * whole block (blend, depth-stencil, rasterizer) with a dirty mask, so each
 * event starts from the device's current state with the one value changed.
 * That is also what FrameContext::pipeline() reports -- read from the device
 * when it is first seen, so what was set before VanGFX attached counts too.
 */

#include "d3d9_impl.hpp"
#include <compat/d3d9_slots.hpp>
#include <hash.hpp>
#include <state_merge.hpp>
#include <vangfx/events/state.hpp>

#include <algorithm>
#include <cstring>

namespace vangfx::backend::d3d9 {

namespace s = ::vangfx::d3d9_slots;
using hooks::Heal;

namespace {

// ── D3D9 → VanGFX ────────────────────────────────────────────────────────────

CompareFunc translate_cmp(DWORD v) noexcept {
    switch (v) {
    case D3DCMP_NEVER:        return CompareFunc::Never;
    case D3DCMP_LESS:         return CompareFunc::Less;
    case D3DCMP_EQUAL:        return CompareFunc::Equal;
    case D3DCMP_LESSEQUAL:    return CompareFunc::LessEqual;
    case D3DCMP_GREATER:      return CompareFunc::Greater;
    case D3DCMP_NOTEQUAL:     return CompareFunc::NotEqual;
    case D3DCMP_GREATEREQUAL: return CompareFunc::GreaterEqual;
    default:                  return CompareFunc::Always;
    }
}

StencilOpMode translate_stencil_op(DWORD v) noexcept {
    switch (v) {
    case D3DSTENCILOP_ZERO:    return StencilOpMode::Zero;
    case D3DSTENCILOP_REPLACE: return StencilOpMode::Replace;
    case D3DSTENCILOP_INCRSAT: return StencilOpMode::IncrSat;
    case D3DSTENCILOP_DECRSAT: return StencilOpMode::DecrSat;
    case D3DSTENCILOP_INVERT:  return StencilOpMode::Invert;
    case D3DSTENCILOP_INCR:    return StencilOpMode::Incr;
    case D3DSTENCILOP_DECR:    return StencilOpMode::Decr;
    default:                   return StencilOpMode::Keep;
    }
}

BlendFactor translate_blend(DWORD v) noexcept {
    switch (v) {
    case D3DBLEND_ZERO:           return BlendFactor::Zero;
    case D3DBLEND_SRCCOLOR:       return BlendFactor::SrcColor;
    case D3DBLEND_INVSRCCOLOR:    return BlendFactor::InvSrcColor;
    case D3DBLEND_SRCALPHA:       return BlendFactor::SrcAlpha;
    case D3DBLEND_INVSRCALPHA:    return BlendFactor::InvSrcAlpha;
    case D3DBLEND_DESTALPHA:      return BlendFactor::DstAlpha;
    case D3DBLEND_INVDESTALPHA:   return BlendFactor::InvDstAlpha;
    case D3DBLEND_DESTCOLOR:      return BlendFactor::DstColor;
    case D3DBLEND_INVDESTCOLOR:   return BlendFactor::InvDstColor;
    case D3DBLEND_SRCALPHASAT:    return BlendFactor::SrcAlphaSat;
    case D3DBLEND_BLENDFACTOR:    return BlendFactor::BlendFactor;
    case D3DBLEND_INVBLENDFACTOR: return BlendFactor::InvBlendFactor;
    default:                      return BlendFactor::One;
    }
}

BlendOp translate_blend_op(DWORD v) noexcept {
    switch (v) {
    case D3DBLENDOP_SUBTRACT:    return BlendOp::Subtract;
    case D3DBLENDOP_REVSUBTRACT: return BlendOp::RevSubtract;
    case D3DBLENDOP_MIN:         return BlendOp::Min;
    case D3DBLENDOP_MAX:         return BlendOp::Max;
    default:                     return BlendOp::Add;
    }
}

FilterMode translate_filter(DWORD v) noexcept {
    switch (v) {
    case D3DTEXF_POINT:       return FilterMode::Point;
    case D3DTEXF_ANISOTROPIC: return FilterMode::Anisotropic;
    default:                  return FilterMode::Linear;
    }
}

AddressMode translate_address(DWORD v) noexcept {
    switch (v) {
    case D3DTADDRESS_MIRROR:     return AddressMode::Mirror;
    case D3DTADDRESS_CLAMP:      return AddressMode::Clamp;
    case D3DTADDRESS_BORDER:     return AddressMode::Border;
    case D3DTADDRESS_MIRRORONCE: return AddressMode::MirrorOnce;
    default:                     return AddressMode::Wrap;
    }
}

float as_float(DWORD v) noexcept { float f; std::memcpy(&f, &v, sizeof f); return f; }

// The render states VanGFX follows, for reading a device's state back.
constexpr D3DRENDERSTATETYPE kTracked[] = {
    D3DRS_ALPHABLENDENABLE, D3DRS_BLENDFACTOR, D3DRS_BLENDOP, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA,
    D3DRS_COLORWRITEENABLE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_STENCILENABLE,
    D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_STENCILFUNC, D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_CCW_STENCILFAIL,
    D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS, D3DRS_FILLMODE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE,
    D3DRS_MULTISAMPLEANTIALIAS, D3DRS_ANTIALIASEDLINEENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS,
};

constexpr D3DSAMPLERSTATETYPE kTrackedSampler[] = {
    D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_BORDERCOLOR, D3DSAMP_MAGFILTER,
    D3DSAMP_MINFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY,
};

// One render state into the event's blocks. Returns the block it touched, or 0
// for a state VanGFX does not follow.
uint32_t fold_render_state(StateChangeEvent& ev, D3DRENDERSTATETYPE state, DWORD v) noexcept {
    auto& rt = ev.blend.targets[0];
    auto& d  = ev.depth;
    auto& r  = ev.raster;
    switch (state) {
    case D3DRS_ALPHABLENDENABLE: rt.blend_enable = v != FALSE;              return StateDirty_Blend;
    case D3DRS_BLENDOP:          rt.color_op  = translate_blend_op(v);       return StateDirty_Blend;
    case D3DRS_SRCBLEND:         rt.src_color = translate_blend(v);          return StateDirty_Blend;
    case D3DRS_DESTBLEND:        rt.dst_color = translate_blend(v);          return StateDirty_Blend;
    case D3DRS_SRCBLENDALPHA:    rt.src_alpha = translate_blend(v);          return StateDirty_Blend;
    case D3DRS_DESTBLENDALPHA:   rt.dst_alpha = translate_blend(v);          return StateDirty_Blend;
    case D3DRS_BLENDOPALPHA:     rt.alpha_op  = translate_blend_op(v);       return StateDirty_Blend;
    case D3DRS_COLORWRITEENABLE: rt.write_mask = static_cast<uint8_t>(v & 0xF); return StateDirty_Blend;
    case D3DRS_SEPARATEALPHABLENDENABLE:                                     return StateDirty_Blend;
    case D3DRS_BLENDFACTOR:      // packed ARGB
        ev.blend.blend_factor[0] = ((v >> 16) & 0xFF) / 255.0f;
        ev.blend.blend_factor[1] = ((v >> 8) & 0xFF) / 255.0f;
        ev.blend.blend_factor[2] = (v & 0xFF) / 255.0f;
        ev.blend.blend_factor[3] = ((v >> 24) & 0xFF) / 255.0f;
        return StateDirty_Blend;

    case D3DRS_ZENABLE:          d.depth_enable = v != D3DZB_FALSE;          return StateDirty_DepthStencil;
    case D3DRS_ZWRITEENABLE:     d.depth_write  = v != FALSE;                return StateDirty_DepthStencil;
    case D3DRS_ZFUNC:            d.depth_func   = translate_cmp(v);          return StateDirty_DepthStencil;
    case D3DRS_STENCILENABLE:    d.stencil_enable = v != FALSE;              return StateDirty_DepthStencil;
    case D3DRS_STENCILREF:       d.stencil_ref  = v;                         return StateDirty_DepthStencil;
    case D3DRS_STENCILMASK:      d.stencil_read_mask  = static_cast<uint8_t>(v); return StateDirty_DepthStencil;
    case D3DRS_STENCILWRITEMASK: d.stencil_write_mask = static_cast<uint8_t>(v); return StateDirty_DepthStencil;
    case D3DRS_STENCILFUNC:      d.front_face.func       = translate_cmp(v);        return StateDirty_DepthStencil;
    case D3DRS_STENCILFAIL:      d.front_face.fail       = translate_stencil_op(v); return StateDirty_DepthStencil;
    case D3DRS_STENCILZFAIL:     d.front_face.depth_fail = translate_stencil_op(v); return StateDirty_DepthStencil;
    case D3DRS_STENCILPASS:      d.front_face.pass       = translate_stencil_op(v); return StateDirty_DepthStencil;
    case D3DRS_CCW_STENCILFUNC:  d.back_face.func        = translate_cmp(v);        return StateDirty_DepthStencil;
    case D3DRS_CCW_STENCILFAIL:  d.back_face.fail        = translate_stencil_op(v); return StateDirty_DepthStencil;
    case D3DRS_CCW_STENCILZFAIL: d.back_face.depth_fail  = translate_stencil_op(v); return StateDirty_DepthStencil;
    case D3DRS_CCW_STENCILPASS:  d.back_face.pass        = translate_stencil_op(v); return StateDirty_DepthStencil;

    case D3DRS_FILLMODE:         r.fill_mode = v == D3DFILL_WIREFRAME ? FillMode::Wireframe : FillMode::Solid; return StateDirty_Rasterizer;
    // D3D9's front faces are clockwise: CCW culls the back, CW the front.
    case D3DRS_CULLMODE:
        r.cull_mode = v == D3DCULL_NONE ? CullMode::None : v == D3DCULL_CW ? CullMode::Front : CullMode::Back;
        r.front_ccw = false;
        return StateDirty_Rasterizer;
    case D3DRS_SCISSORTESTENABLE:     r.scissor_enable     = v != FALSE; return StateDirty_Rasterizer;
    case D3DRS_MULTISAMPLEANTIALIAS:  r.multisample_enable = v != FALSE; return StateDirty_Rasterizer;
    case D3DRS_ANTIALIASEDLINEENABLE: r.antialiased_line   = v != FALSE; return StateDirty_Rasterizer;
    // A float in a DWORD, a fraction of the depth range; reported in D3D11's
    // units, steps of a 24-bit depth buffer.
    case D3DRS_DEPTHBIAS:             r.depth_bias = static_cast<int32_t>(as_float(v) * 16777215.0f); return StateDirty_Rasterizer;
    case D3DRS_SLOPESCALEDEPTHBIAS:   r.slope_scaled_depth_bias = as_float(v); return StateDirty_Rasterizer;
    default: return 0;
    }
}

bool fold_sampler_state(SamplerState& ss, D3DSAMPLERSTATETYPE type, DWORD v) noexcept {
    switch (type) {
    case D3DSAMP_ADDRESSU:      ss.address_u = translate_address(v); return true;
    case D3DSAMP_ADDRESSV:      ss.address_v = translate_address(v); return true;
    case D3DSAMP_ADDRESSW:      ss.address_w = translate_address(v); return true;
    case D3DSAMP_MINFILTER:
    case D3DSAMP_MAGFILTER:     ss.filter = translate_filter(v); return true;
    case D3DSAMP_MIPMAPLODBIAS: ss.mip_bias = as_float(v); return true;
    case D3DSAMP_MAXMIPLEVEL:   ss.min_lod = static_cast<float>(v); return true;   // the most detailed level allowed
    case D3DSAMP_MAXANISOTROPY: ss.max_anisotropy = v; return true;
    case D3DSAMP_BORDERCOLOR:
        ss.border_color[0] = ((v >> 16) & 0xFF) / 255.0f;
        ss.border_color[1] = ((v >> 8) & 0xFF) / 255.0f;
        ss.border_color[2] = (v & 0xFF) / 255.0f;
        ss.border_color[3] = ((v >> 24) & 0xFF) / 255.0f;
        return true;
    default: return false;
    }
}

// The sixteen pixel shader samplers. The four vertex texture samplers
// (D3DVERTEXTEXTURESAMPLER0..3) have no slots of their own in PipelineState.
int sampler_slot(DWORD sampler) noexcept { return sampler < 16 ? static_cast<int>(sampler) : -1; }

StateChangeEvent seeded(const PipelineState& p) noexcept {
    StateChangeEvent ev{};
    ev.blend  = p.blend;
    ev.depth  = p.depth_stencil;
    ev.raster = p.raster;
    std::copy(std::begin(p.samplers), std::end(p.samplers), std::begin(ev.samplers));
    std::copy(std::begin(p.viewports), std::end(p.viewports), std::begin(ev.viewports));
    std::copy(std::begin(p.scissors), std::end(p.scissors), std::begin(ev.scissors));
    ev.topology = p.topology;
    return ev;
}

// A vertex format's identity: the hash of its elements, the same for every
// declaration with the same layout. FVF codes are marked by the top bit.
uint64_t declaration_handle(IDirect3DVertexDeclaration9* decl) noexcept {
    if (!decl) return 0;
    D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (FAILED(decl->GetDeclaration(elems, &n)) || n == 0) return 0;
    return hash_bytes(elems, n * sizeof(D3DVERTEXELEMENT9)) & ~(1ull << 63);
}
uint64_t fvf_handle(DWORD fvf) noexcept { return fvf ? (1ull << 63) | fvf : 0; }

// ── Hooks ────────────────────────────────────────────────────────────────────

using SetRenderStateFn  = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using SetViewportFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetScissorFn      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);
using SetDeclFn         = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetFVFFn          = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD);

hooks::Site s_render_state { s::SetRenderState,       "SetRenderState",       Heal::SameModule };
hooks::Site s_sampler_state{ s::SetSamplerState,      "SetSamplerState",      Heal::SameModule };
hooks::Site s_viewport     { s::SetViewport,          "SetViewport",          Heal::SameModule };
hooks::Site s_scissor      { s::SetScissorRect,       "SetScissorRect",       Heal::SameModule };
hooks::Site s_decl         { s::SetVertexDeclaration, "SetVertexDeclaration", Heal::SameModule };
hooks::Site s_fvf          { s::SetFVF,               "SetFVF",               Heal::SameModule };

// Builds the event from the current state, folds it back in and tells whoever
// listens. `build` returns the dirty mask; 0 means nothing followed changed.
template <typename Build>
void state_change(IDirect3DDevice9* self, Build&& build) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    DevState* st = dev_state(self);
    if (!st || st->recording) return;
    StateChangeEvent ev = seeded(st->pipeline);
    ev.dirty = build(ev, *st);
    if (!ev.dirty) return;
    state_merge::merge(st->pipeline, ev);
    for (D3D9ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_state()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), st);
        ctx->dispatch_state(fc, ev);
    }
}

HRESULT STDMETHODCALLTYPE hook_render_state(IDirect3DDevice9* self, D3DRENDERSTATETYPE state, DWORD v) {
    const Healer healer{ self };
    const HRESULT hr = s_render_state.original<SetRenderStateFn>(self)(self, state, v);
    if (SUCCEEDED(hr))
        state_change(self, [&](StateChangeEvent& ev, DevState&) { return fold_render_state(ev, state, v); });
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_sampler_state(IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD v) {
    const Healer healer{ self };
    const HRESULT hr = s_sampler_state.original<SetSamplerStateFn>(self)(self, sampler, type, v);
    const int slot = sampler_slot(sampler);
    if (SUCCEEDED(hr) && slot >= 0) {
        state_change(self, [&](StateChangeEvent& ev, DevState&) -> uint32_t {
            return fold_sampler_state(ev.samplers[slot], type, v) ? StateDirty_Samplers : 0;
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_viewport(IDirect3DDevice9* self, const D3DVIEWPORT9* vp) {
    const Healer healer{ self };
    const HRESULT hr = s_viewport.original<SetViewportFn>(self)(self, vp);
    if (SUCCEEDED(hr) && vp) {
        state_change(self, [&](StateChangeEvent& ev, DevState&) -> uint32_t {
            ev.viewports[0] = { static_cast<float>(vp->X), static_cast<float>(vp->Y), static_cast<float>(vp->Width),
                                static_cast<float>(vp->Height), vp->MinZ, vp->MaxZ };
            return StateDirty_Viewports;
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_scissor(IDirect3DDevice9* self, const RECT* rect) {
    const Healer healer{ self };
    const HRESULT hr = s_scissor.original<SetScissorFn>(self)(self, rect);
    if (SUCCEEDED(hr) && rect) {
        state_change(self, [&](StateChangeEvent& ev, DevState&) -> uint32_t {
            ev.scissors[0] = { static_cast<int32_t>(rect->left), static_cast<int32_t>(rect->top),
                               static_cast<int32_t>(rect->right), static_cast<int32_t>(rect->bottom) };
            return StateDirty_Scissors;
        });
    }
    return hr;
}

// The declaration has no field on the event; it goes on the pipeline directly.
HRESULT STDMETHODCALLTYPE hook_decl(IDirect3DDevice9* self, IDirect3DVertexDeclaration9* decl) {
    const Healer healer{ self };
    const HRESULT hr = s_decl.original<SetDeclFn>(self)(self, decl);
    if (SUCCEEDED(hr)) {
        state_change(self, [&](StateChangeEvent&, DevState& st) -> uint32_t {
            st.pipeline.vertex_decl.handle = declaration_handle(decl);
            return StateDirty_VertexDecl;
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_fvf(IDirect3DDevice9* self, DWORD fvf) {
    const Healer healer{ self };
    const HRESULT hr = s_fvf.original<SetFVFFn>(self)(self, fvf);
    if (SUCCEEDED(hr)) {
        state_change(self, [&](StateChangeEvent&, DevState& st) -> uint32_t {
            st.pipeline.vertex_decl.handle = fvf_handle(fvf);
            return StateDirty_VertexDecl;
        });
    }
    return hr;
}

} // namespace

void target_resets_viewport(DevState& st, uint32_t width, uint32_t height) noexcept {
    StateChangeEvent ev = seeded(st.pipeline);
    ev.viewports[0] = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    ev.scissors[0]  = { 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height) };
    ev.dirty = StateDirty_Viewports | StateDirty_Scissors;
    state_merge::merge(st.pipeline, ev);
    for (D3D9ContextImpl* ctx : st.contexts) {
        if (!ctx->wants_state()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), &st);
        ctx->dispatch_state(fc, ev);
    }
}

void read_pipeline(IDirect3DDevice9* device, PipelineState& out) noexcept {
    // A pure device answers no Get*: what cannot be read keeps its value.
    StateChangeEvent ev = seeded(out);
    uint32_t dirty = 0;
    for (D3DRENDERSTATETYPE rs : kTracked) {
        DWORD v = 0;
        if (SUCCEEDED(device->GetRenderState(rs, &v))) dirty |= fold_render_state(ev, rs, v);
    }
    for (DWORD i = 0; i < 16; ++i) {
        for (D3DSAMPLERSTATETYPE t : kTrackedSampler) {
            DWORD v = 0;
            if (SUCCEEDED(device->GetSamplerState(i, t, &v)) && fold_sampler_state(ev.samplers[i], t, v))
                dirty |= StateDirty_Samplers;
        }
    }
    D3DVIEWPORT9 vp{};
    if (SUCCEEDED(device->GetViewport(&vp))) {
        ev.viewports[0] = { static_cast<float>(vp.X), static_cast<float>(vp.Y), static_cast<float>(vp.Width),
                            static_cast<float>(vp.Height), vp.MinZ, vp.MaxZ };
        dirty |= StateDirty_Viewports;
    }
    RECT rc{};
    if (SUCCEEDED(device->GetScissorRect(&rc))) {
        ev.scissors[0] = { static_cast<int32_t>(rc.left), static_cast<int32_t>(rc.top),
                           static_cast<int32_t>(rc.right), static_cast<int32_t>(rc.bottom) };
        dirty |= StateDirty_Scissors;
    }
    ev.dirty = dirty;
    state_merge::merge(out, ev);

    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    if (SUCCEEDED(device->GetVertexDeclaration(&decl)) && decl) {
        out.vertex_decl.handle = declaration_handle(decl);
        decl->Release();
    } else if (SUCCEEDED(device->GetFVF(&fvf))) {
        out.vertex_decl.handle = fvf_handle(fvf);
    }
}

void install_state_hooks(IDirect3DDevice9* device) noexcept {
    s_render_state.install(device, &hook_render_state);
    s_sampler_state.install(device, &hook_sampler_state);
    s_viewport.install(device, &hook_viewport);
    s_scissor.install(device, &hook_scissor);
    s_decl.install(device, &hook_decl);
    s_fvf.install(device, &hook_fvf);
    for (hooks::Site* site : { &s_render_state, &s_sampler_state, &s_viewport, &s_scissor, &s_decl, &s_fvf })
        register_device_site(*site);
}

}  // namespace vangfx::backend::d3d9
