/**
 * src/backend/d3d9/rendertarget.cpp — SetRenderTarget and
 * SetDepthStencilSurface.
 *
 * Direct3D 9 binds one target per call: SetRenderTarget(i, surface) changes
 * slot i (of up to four) and leaves the others, and the depth-stencil surface
 * has a call of its own. So the device's bound set is kept (DevState::surfaces)
 * and each call is reported as the whole set it leaves behind -- the same
 * RenderTargetEvent the other backends raise -- while SetDepthStencilSurface
 * raises a DepthEvent. Targets are named by their Create event's id; the top
 * level of a render-target texture by the texture's (see surface_id). They are
 * also made readable by those ids.
 *
 * SetRenderTarget(0, ...) also sets the viewport and scissor rectangle to the
 * whole target, without calling either setter; that is folded into the
 * pipeline and reported as a state change.
 */

#include "d3d9_impl.hpp"
#include <compat/d3d9_slots.hpp>
#include <format_map.hpp>
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::d3d9 {

namespace s = ::vangfx::d3d9_slots;
using hooks::Heal;

namespace {

using SetRTFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetDSFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*);

hooks::Site s_set_rt{ s::SetRenderTarget,        "SetRenderTarget",        Heal::SameModule };
hooks::Site s_set_ds{ s::SetDepthStencilSurface, "SetDepthStencilSurface", Heal::SameModule };

constexpr DWORD kMaxTargets = 4;   // D3DCAPS9::NumSimultaneousRTs is at most 4

struct Described {
    uint32_t      width = 0, height = 0;
    SurfaceFormat format = SurfaceFormat::Unknown;
    bool          stencil = false;
};

Described describe(IDirect3DSurface9* surface) noexcept {
    Described d;
    D3DSURFACE_DESC sd{};
    if (!surface || FAILED(surface->GetDesc(&sd))) return d;
    d.width   = sd.Width;
    d.height  = sd.Height;
    d.format  = fmtmap::from_d3d9(sd.Format);
    d.stencil = fmtmap::has_stencil(d.format) || sd.Format == D3DFMT_D24X4S4 || sd.Format == D3DFMT_S8_LOCKABLE;
    return d;
}

uint32_t count_of(const GBufferSet& set) noexcept {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxTargets; ++i) if (set.targets[i].id) n = i + 1;
    return n;
}

HRESULT STDMETHODCALLTYPE hook_set_rt(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface) {
    const Healer healer{ self };
    auto orig = s_set_rt.original<SetRTFn>(self);
    // Target 0 cannot be unbound: the call fails, and is passed on unreported.
    if (hub_idle() || internal() || index >= kMaxTargets || (index == 0 && !surface))
        return orig(self, index, surface);
    HubReader reader;
    DevState* st = dev_state(self);
    if (!st) return orig(self, index, surface);

    const uint64_t  id = surface_id(surface, self);
    const Described d  = describe(surface);
    GBufferSet next = st->surfaces.current_rt_set;
    next.targets[index] = { id };
    next.count = count_of(next);
    next.depth = st->surfaces.depth;
    if (index == 0) { next.width = d.width; next.height = d.height; }

    RenderTargetEvent ev{};
    for (uint32_t i = 0; i < kMaxTargets; ++i) ev.targets[i] = next.targets[i];
    ev.count  = next.count;
    ev.depth  = next.depth;
    ev.width  = next.width;
    ev.height = next.height;

    for (D3D9ContextImpl* ctx : st->contexts) {
        if (surface) ctx->store().adopt_render_target(id, surface);
        if (!ctx->wants_rt()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), st);
        ctx->dispatch_render_target(fc, ev);
    }
    if (ev.intercept) return S_OK;

    const HRESULT hr = orig(self, index, surface);
    if (SUCCEEDED(hr)) {
        st->surfaces.current_rt_set = next;
        if (index == 0) target_resets_viewport(*st, d.width, d.height);
        capture_bind(self, *st);   // a capture's pass ends here
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_set_ds(IDirect3DDevice9* self, IDirect3DSurface9* surface) {
    const Healer healer{ self };
    auto orig = s_set_ds.original<SetDSFn>(self);
    if (hub_idle() || internal()) return orig(self, surface);
    HubReader reader;
    DevState* st = dev_state(self);
    if (!st) return orig(self, surface);

    const uint64_t  id = surface_id(surface, self);
    const Described d  = describe(surface);
    DepthEvent ev{};
    ev.depth       = { id };
    ev.format      = d.format;
    ev.has_stencil = d.stencil;
    for (D3D9ContextImpl* ctx : st->contexts) {
        if (surface) ctx->store().adopt_depth_buffer(id, surface);
        if (!ctx->wants_rt()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), st);
        ctx->dispatch_depth(fc, ev);
    }
    if (ev.intercept) return S_OK;

    const HRESULT hr = orig(self, surface);
    if (SUCCEEDED(hr)) {
        st->surfaces.depth                = { id };
        st->surfaces.depth_format         = d.format;
        st->surfaces.current_rt_set.depth = { id };
        capture_bind(self, *st);
    }
    return hr;
}

} // namespace

void read_surfaces(IDirect3DDevice9* device, DevState& st) noexcept {
    GBufferSet set{};
    for (DWORD i = 0; i < kMaxTargets; ++i) {
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(device->GetRenderTarget(i, &rt)) || !rt) continue;   // D3DERR_NOTFOUND when unbound
        set.targets[i] = { surface_id(rt, device) };
        if (i == 0) {
            const Described d = describe(rt);
            set.width = d.width; set.height = d.height;
        }
        for (D3D9ContextImpl* ctx : st.contexts) ctx->store().adopt_render_target(set.targets[i].id, rt);
        rt->Release();
    }
    set.count = count_of(set);
    IDirect3DSurface9* ds = nullptr;
    if (SUCCEEDED(device->GetDepthStencilSurface(&ds)) && ds) {
        set.depth = { surface_id(ds, device) };
        st.surfaces.depth_format = describe(ds).format;
        for (D3D9ContextImpl* ctx : st.contexts) ctx->store().adopt_depth_buffer(set.depth.id, ds);
        ds->Release();
    } else {
        st.surfaces.depth_format = SurfaceFormat::Unknown;
    }
    st.surfaces.depth          = set.depth;
    st.surfaces.current_rt_set = set;
}

void install_rendertarget_hooks(IDirect3DDevice9* device) noexcept {
    s_set_rt.install(device, &hook_set_rt);
    s_set_ds.install(device, &hook_set_ds);
    register_device_site(s_set_rt);
    register_device_site(s_set_ds);
}

}  // namespace vangfx::backend::d3d9
