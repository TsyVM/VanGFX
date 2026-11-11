/**
 * src/backend/d3d11/rendertarget.cpp — OMSetRenderTargets and
 * OMSetRenderTargetsAndUnorderedAccessViews.
 *
 * Targets are reported by the resource behind each view, under the id that
 * resource's Create event carried (a resource made before VanGFX attached --
 * the swap chain's buffers, say -- is reported when first bound), with its
 * size and format. The resources are also made readable by those ids.
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>
#include <format_map.hpp>
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::d3d11 {

namespace dcs = ::vangfx::d3d11_slots::device_context;
using hooks::Heal;

namespace {

using OMSetRTFn    = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRTUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                              UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);

hooks::Site s_om_rt    { dcs::OMSetRenderTargets,                        "OMSetRenderTargets",           Heal::SameModule };
hooks::Site s_om_rt_uav{ dcs::OMSetRenderTargetsAndUnorderedAccessViews, "OMSetRenderTargetsAndUAVs",    Heal::SameModule };

// The size and format of the texture behind a view.
void describe(ID3D11View* view, uint32_t& w, uint32_t& h, SurfaceFormat& fmt) noexcept {
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (!res) return;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex))) && tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        w = d.Width; h = d.Height; fmt = fmtmap::from_dxgi(d.Format);
        tex->Release();
    }
    res->Release();
}

// Reports a binding. Returns whether a callback suppressed it.
bool report(ID3D11DeviceContext* self, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (hub_idle() || internal()) return false;
    HubReader reader;
    DcState* st = dc_state(self);
    if (!st) return false;

    RenderTargetEvent ev{};
    ev.count = n < 8 ? n : 8;
    SurfaceFormat first_format = SurfaceFormat::Unknown, depth_format = SurfaceFormat::Unknown;
    (void)first_format;
    for (UINT i = 0; rtvs && i < ev.count; ++i) {
        if (!rtvs[i]) continue;
        ev.targets[i] = { view_resource_id(rtvs[i], st->device) };
        if (ev.width == 0) describe(rtvs[i], ev.width, ev.height, first_format);
    }
    if (dsv) {
        ev.depth = { view_resource_id(dsv, st->device) };
        uint32_t w = 0, h = 0;
        describe(dsv, w, h, depth_format);
        if (ev.width == 0) { ev.width = w; ev.height = h; }
    }

    FrameSurfaces& fs = st->surfaces;
    fs.current_rt_set = {};
    fs.current_rt_set.count  = ev.count;
    fs.current_rt_set.depth  = ev.depth;
    fs.current_rt_set.width  = ev.width;
    fs.current_rt_set.height = ev.height;
    for (uint32_t i = 0; i < ev.count; ++i) fs.current_rt_set.targets[i] = ev.targets[i];
    fs.depth        = ev.depth;
    fs.depth_format = depth_format;
    st->rtv_count = rtvs ? ev.count : 0;
    for (uint32_t i = 0; i < 8; ++i) st->rtv[i] = rtvs && i < ev.count ? rtvs[i] : nullptr;
    st->dsv = dsv;

    bool intercept = false;
    for (D3D11ContextImpl* ctx : st->contexts) {
        for (UINT i = 0; rtvs && i < ev.count; ++i) {
            if (!rtvs[i]) continue;
            ID3D11Resource* res = nullptr;
            rtvs[i]->GetResource(&res);
            ctx->store().adopt_render_target(ev.targets[i].id, res);
            if (res) res->Release();
        }
        if (dsv) {
            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            ctx->store().adopt_depth_buffer(ev.depth.id, res);
            if (res) res->Release();
        }
        if (!ctx->wants_rt()) continue;
        D3D11FrameContextImpl fc(*ctx, ctx->current_frame_index(), self, st);
        ctx->dispatch_render_target(fc, ev);
        if (n == 0 && dsv) {
            DepthEvent dev{};
            dev.depth       = ev.depth;
            dev.format      = depth_format;
            dev.has_stencil = fmtmap::has_stencil(depth_format);
            ctx->dispatch_depth(fc, dev);
            intercept |= dev.intercept;
        }
    }
    return intercept || ev.intercept;
}

// After the binding reached the runtime: a capture's pass ends here.
void bound(ID3D11DeviceContext* self) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    if (DcState* st = dc_state(self)) capture_bind(self, *st);
}

void STDMETHODCALLTYPE hook_om_rt(ID3D11DeviceContext* self, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    const Healer healer{ self };
    if (report(self, n, rtvs, dsv)) return;
    s_om_rt.original<OMSetRTFn>(self)(self, n, rtvs, dsv);
    bound(self);
}

void STDMETHODCALLTYPE hook_om_rt_uav(ID3D11DeviceContext* self, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
                                      UINT uav_start, UINT uav_n, ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    const Healer healer{ self };
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL leaves the targets alone.
    const bool keeps = n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL;
    if (!keeps && report(self, n, rtvs, dsv)) return;
    s_om_rt_uav.original<OMSetRTUAVFn>(self)(self, n, rtvs, dsv, uav_start, uav_n, uavs, counts);
    if (!keeps) bound(self);
}

} // namespace

void install_rendertarget_hooks(ID3D11DeviceContext* context) noexcept {
    s_om_rt.install(context, &hook_om_rt);
    s_om_rt_uav.install(context, &hook_om_rt_uav);
    register_context_site(s_om_rt);
    register_context_site(s_om_rt_uav);
}

}  // namespace vangfx::backend::d3d11
