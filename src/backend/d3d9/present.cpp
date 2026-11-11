/**
 * src/backend/d3d9/present.cpp — the frame boundary and the device reset.
 *
 * Hooked on the device's own table (Heal::SameModule): Present and Reset, and
 * on an Ex device PresentEx and ResetEx. Hooked on the shared swap chain table:
 * IDirect3DSwapChain9::Present, which a game with more than one swap chain (an
 * editor's views, say) presents each through. The three presents are separate
 * paths in the runtime -- none calls another -- so each is one frame.
 *
 * A present runs the frame's work for each context on the device: the frame
 * counter, the end of the frame a capture was taking (and auto-capture), the
 * present callbacks and the post passes, in that order. (EndScene is no longer treated as a present: a
 * game may call BeginScene/EndScene many times a frame.)
 *
 * Reset is reported as device lost, then device reset. On a plain device Reset
 * fails while anything in the default pool is alive -- VanGFX's own resources
 * included -- so those go first, after device_lost gives callbacks the chance
 * to drop their handles. Reset puts all device state back to its defaults and
 * rewrites the setters in the table, so afterwards the table is healed and the
 * state read again.
 */

#include "d3d9_impl.hpp"
#include <compat/d3d9_slots.hpp>
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::d3d9 {

namespace s  = ::vangfx::d3d9_slots;
namespace sx = ::vangfx::d3d9_slots::device_ex;
using hooks::Heal;

namespace {

using PresentFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using ScPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using ResetFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ResetExFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

hooks::Site s_present   { s::Present,             "IDirect3DDevice9::Present",     Heal::SameModule };
hooks::Site s_reset     { s::Reset,               "IDirect3DDevice9::Reset",       Heal::SameModule };
hooks::Site s_present_ex{ sx::PresentEx,          "IDirect3DDevice9Ex::PresentEx", Heal::SameModule };
hooks::Site s_reset_ex  { sx::ResetEx,            "IDirect3DDevice9Ex::ResetEx",   Heal::SameModule };
hooks::Site s_sc_present{ s::swap_chain::Present, "IDirect3DSwapChain9::Present" };

// The swap chain's presentation interval as a sync interval: 0 immediate,
// 1..4 vertical blanks. `swap_chain` null means the device's own.
uint32_t sync_interval(IDirect3DDevice9* device, IDirect3DSwapChain9* swap_chain) noexcept {
    IDirect3DSwapChain9* chain = swap_chain;
    if (!chain && (FAILED(device->GetSwapChain(0, &chain)) || !chain)) return 0;
    D3DPRESENT_PARAMETERS pp{};
    const HRESULT hr = chain->GetPresentParameters(&pp);
    if (!swap_chain) chain->Release();
    if (FAILED(hr)) return 0;
    switch (pp.PresentationInterval) {
    case D3DPRESENT_INTERVAL_IMMEDIATE: return 0;
    case D3DPRESENT_INTERVAL_TWO:       return 2;
    case D3DPRESENT_INTERVAL_THREE:     return 3;
    case D3DPRESENT_INTERVAL_FOUR:      return 4;
    default:                            return 1;   // DEFAULT and ONE
    }
}

// The frame's work for each context on the device. Returns whether a callback
// suppressed the present.
bool before_present(IDirect3DDevice9* device, IDirect3DSwapChain9* swap_chain, DWORD flags) {
    HubReader reader;
    DevState* st = dev_state(device);
    if (!st) return false;
    bool wants = false;
    for (D3D9ContextImpl* ctx : st->contexts) wants |= ctx->wants_present();
    const uint32_t interval = wants ? sync_interval(device, swap_chain) : 0;

    bool intercept = false;
    for (D3D9ContextImpl* ctx : st->contexts) {
        const auto timing = ctx->tick();
        const uint32_t frame = ctx->next_frame();
        D3D9FrameContextImpl fc(*ctx, frame, st, swap_chain);
        // The frame a capture was taking ends here, before VanGFX's post passes.
        ctx->capture_present(fc, timing.first);

        PresentEvent ev{};
        ev.sync_interval = interval;
        ev.flags         = flags;
        ctx->dispatch_present(fc, ev);
        intercept |= ev.intercept;
        if (!ev.intercept) ctx->post_process(swap_chain, timing);
    }
    return intercept;
}

HRESULT STDMETHODCALLTYPE hook_present(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                       const RGNDATA* dirty) {
    const Healer healer{ self };
    auto orig = s_present.original<PresentFn>(self);
    if (hub_idle() || internal()) return orig(self, src, dst, wnd, dirty);
    if (before_present(self, nullptr, 0)) return S_OK;
    return orig(self, src, dst, wnd, dirty);
}

HRESULT STDMETHODCALLTYPE hook_present_ex(IDirect3DDevice9Ex* self, const RECT* src, const RECT* dst, HWND wnd,
                                          const RGNDATA* dirty, DWORD flags) {
    const Healer healer{ self };
    auto orig = s_present_ex.original<PresentExFn>(self);
    if (hub_idle() || internal()) return orig(self, src, dst, wnd, dirty, flags);
    if (before_present(self, nullptr, flags)) return S_OK;
    return orig(self, src, dst, wnd, dirty, flags);
}

HRESULT STDMETHODCALLTYPE hook_sc_present(IDirect3DSwapChain9* self, const RECT* src, const RECT* dst, HWND wnd,
                                          const RGNDATA* dirty, DWORD flags) {
    auto orig = s_sc_present.original<ScPresentFn>(self);
    if (hub_idle() || internal()) return orig(self, src, dst, wnd, dirty, flags);
    IDirect3DDevice9* device = nullptr;
    if (FAILED(self->GetDevice(&device)) || !device) return orig(self, src, dst, wnd, dirty, flags);
    device->Release();   // the swap chain holds it
    if (before_present(device, self, flags)) return S_OK;
    return orig(self, src, dst, wnd, dirty, flags);
}

// Reset and ResetEx: device lost, the call, device reset.
template <typename Call>
HRESULT reset_common(IDirect3DDevice9* self, Call&& call) {
    const Healer healer{ self };   // a plain device's Reset rewrites seventeen setters
    if (hub_idle() || internal()) return call();
    {
        HubReader reader;
        if (DevState* st = dev_state(self)) {
            for (D3D9ContextImpl* ctx : st->contexts) {
                ctx->dispatch_device_lost();
                ctx->release_for_reset();
            }
        }
    }
    HRESULT hr;
    {
        // Reset sets the viewport and scissor on an Ex device, and releases the
        // device several times, through the table: its own work, not the game's.
        InternalScope quiet;
        hr = call();
    }
    if (SUCCEEDED(hr)) {
        heal_device(self);   // before anything below calls through the table
        HubReader reader;
        if (DevState* st = dev_state(self)) {
            reread_device(self, *st);
            capture_bind(self, *st);   // Reset bound the back buffer and the automatic depth buffer
            for (D3D9ContextImpl* ctx : st->contexts) {
                D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), st);
                ctx->dispatch_device_reset(fc);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp) {
    return reset_common(self, [&] { return s_reset.original<ResetFn>(self)(self, pp); });
}

HRESULT STDMETHODCALLTYPE hook_reset_ex(IDirect3DDevice9Ex* self, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode) {
    return reset_common(self, [&] { return s_reset_ex.original<ResetExFn>(self)(self, pp, mode); });
}

} // namespace

void install_present_hooks(IDirect3DDevice9* device, bool ex) noexcept {
    s_present.install(device, &hook_present);
    s_reset.install(device, &hook_reset);
    register_device_site(s_present);
    register_device_site(s_reset);
    if (ex) {
        s_present_ex.install(device, &hook_present_ex);
        s_reset_ex.install(device, &hook_reset_ex);
        register_ex_device_site(s_present_ex);
        register_ex_device_site(s_reset_ex);
    }
    IDirect3DSwapChain9* chain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain) {
        s_sc_present.install(chain, &hook_sc_present);
        chain->Release();
    }
    register_class_site(s_sc_present);
}

}  // namespace vangfx::backend::d3d9
