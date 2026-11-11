/**
 * src/backend/d3d12/rendertarget.cpp — ID3D12GraphicsCommandList::OMSetRenderTargets
 * and ID3D12GraphicsCommandList4::BeginRenderPass / EndRenderPass.
 *
 * D3D12 binds render targets by CPU descriptor handle, and nothing in D3D12
 * relates a descriptor back to the resource it describes. VanGFX learns the pair
 * where they meet — CreateRenderTargetView / CreateDepthStencilView
 * (descriptor_heap.cpp) — so the event can name the resources being bound by the
 * same ids resource events use, with their size and format. A view created
 * before VanGFX attached cannot be resolved; its target is reported with id 0
 * and its raw descriptor in d3d12_rtv_handles[]. (AutoAttach attaches before the
 * game creates any, which is one reason to prefer it.)
 *
 * A render pass binds its targets the same way and is reported the same way;
 * its event cannot be intercepted (a BeginRenderPass left out would leave its
 * EndRenderPass unpaired). EndRenderPass ends a capture's pass, except when the
 * render pass is suspended to be resumed in another list: nothing may be
 * recorded after one of those.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <format_map.hpp>
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::d3d12 {

namespace lst = ::vangfx::d3d12_slots::graphics_command_list;

namespace {

using OMSetRTFn   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                             BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
using BeginPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                             const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
using EndPassFn   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);

hooks::Site s_om_set_rt { lst::OMSetRenderTargets, "OMSetRenderTargets" };
hooks::Site s_begin_pass{ lst::BeginRenderPass,    "ID3D12GraphicsCommandList4::BeginRenderPass" };
hooks::Site s_end_pass  { lst::EndRenderPass,      "ID3D12GraphicsCommandList4::EndRenderPass" };

// Records the binding in the list's state and reports it. `rtv(i)` gives the
// i-th target's CPU descriptor, `dsv` the depth buffer's (0: none). Returns
// whether a callback suppressed it.
template <typename RtvAt>
bool bind(ID3D12GraphicsCommandList* self, ListState& st, uint32_t count, RtvAt&& rtv, uint64_t dsv,
          bool single_range) {
    RenderTargetEvent ev{};
    ev.count                     = count < 8 ? count : 8;
    ev.d3d12_single_handle_range = single_range;
    ev.d3d12_command_list        = { st.id };

    st.bound_count = ev.count;
    for (uint32_t i = 0; i < 8; ++i) st.bound[i] = {};
    st.bound_depth = {};
    for (uint32_t i = 0; i < ev.count; ++i) {
        const uint64_t cpu = rtv(i);
        ev.d3d12_rtv_handles[i] = { cpu };
        const BoundTarget b = view_target(cpu);
        st.bound[i] = b;
        if (!b.res) continue;
        ev.targets[i] = { resource_id(b.res) };
        if (ev.width == 0) {
            const D3D12_RESOURCE_DESC d = b.res->GetDesc();
            ev.width  = static_cast<uint32_t>(d.Width);
            ev.height = d.Height;
        }
    }
    if (dsv) {
        ev.d3d12_dsv_handle = { dsv };
        const BoundTarget b = view_target(dsv);
        st.bound_depth = b;
        if (b.res) {
            ev.depth = { resource_id(b.res) };
            if (ev.width == 0) {
                const D3D12_RESOURCE_DESC d = b.res->GetDesc();
                ev.width  = static_cast<uint32_t>(d.Width);
                ev.height = d.Height;
            }
        }
    }

    // The list's surfaces, as FrameContext::surfaces() reports them.
    FrameSurfaces& fs = st.surfaces;
    fs.current_rt_set = {};
    fs.current_rt_set.count  = ev.count;
    fs.current_rt_set.depth  = ev.depth;
    fs.current_rt_set.width  = ev.width;
    fs.current_rt_set.height = ev.height;
    for (uint32_t i = 0; i < ev.count; ++i) fs.current_rt_set.targets[i] = ev.targets[i];
    fs.depth  = ev.depth;
    fs.width  = ev.width;
    fs.height = ev.height;

    for (D3D12ContextImpl* ctx : st.contexts) {
        ctx->note_surfaces(fs);
        if (!ctx->wants_rt()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index(), nullptr, self, &st);
        ctx->dispatch_render_target(fc, ev);
    }
    if (ev.count == 0 && dsv) {
        DepthEvent dev{};
        dev.depth = ev.depth;
        for (D3D12ContextImpl* ctx : st.contexts) {
            if (!ctx->wants_rt()) continue;
            D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index(), nullptr, self, &st);
            ctx->dispatch_depth(fc, dev);
        }
    }
    return ev.intercept;
}

void STDMETHODCALLTYPE hook_om_set_rt(ID3D12GraphicsCommandList* self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                      BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    auto orig = s_om_set_rt.original<OMSetRTFn>(self);
    if (hub_idle() || internal()) return orig(self, count, rtvs, single_range, dsv);

    HubReader reader;
    ListState* st = list_state(self);
    if (!st) return orig(self, count, rtvs, single_range, dsv);

    // With a single range, target i is the i-th descriptor after the first one,
    // not the first one again.
    const uint32_t stride = single_range ? descriptor_increment(st->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
    const bool intercept = bind(self, *st, rtvs ? count : 0,
        [&](uint32_t i) { return single_range ? rtvs[0].ptr + uint64_t(i) * stride : uint64_t(rtvs[i].ptr); },
        dsv ? uint64_t(dsv->ptr) : 0, single_range != FALSE);
    if (intercept) return;
    orig(self, count, rtvs, single_range, dsv);
    capture_bind(self, *st);
}

void STDMETHODCALLTYPE hook_begin_pass(ID3D12GraphicsCommandList4* self, UINT count,
                                       const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                                       const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags) {
    auto orig = s_begin_pass.original<BeginPassFn>(self);
    if (hub_idle() || internal()) return orig(self, count, rts, ds, flags);

    HubReader reader;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self);
    ListState* st = list_state(list);
    if (!st) return orig(self, count, rts, ds, flags);

    (void)bind(list, *st, rts ? count : 0, [&](uint32_t i) { return uint64_t(rts[i].cpuDescriptor.ptr); },
               ds ? uint64_t(ds->cpuDescriptor.ptr) : 0, false);
    // Before the render pass opens: the pass this binding ends is copied
    // outside it (nothing can be copied inside one).
    capture_bind(list, *st);
    orig(self, count, rts, ds, flags);
    st->in_render_pass    = true;
    st->suspended         = false;
    st->render_pass_flags = static_cast<uint32_t>(flags);
}

void STDMETHODCALLTYPE hook_end_pass(ID3D12GraphicsCommandList4* self) {
    auto orig = s_end_pass.original<EndPassFn>(self);
    if (hub_idle() || internal()) return orig(self);

    HubReader reader;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self);
    ListState* st = list_state(list);
    orig(self);
    if (!st) return;
    st->in_render_pass = false;
    st->suspended = (st->render_pass_flags & D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS) != 0;
    if (!st->suspended) capture_pass_break(list, *st);
}

} // namespace

void install_rendertarget_hooks(ID3D12GraphicsCommandList* list) noexcept {
    s_om_set_rt.install(list, &hook_om_set_rt);
    ID3D12GraphicsCommandList4* l4 = nullptr;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l4))) && l4) {
        s_begin_pass.install(l4, &hook_begin_pass);
        s_end_pass.install(l4, &hook_end_pass);
        l4->Release();
    }
}

void uninstall_rendertarget_hooks() noexcept {
    s_om_set_rt.uninstall();
    s_begin_pass.uninstall();
    s_end_pass.uninstall();
}

} // namespace vangfx::backend::d3d12
