/**
 * src/backend/d3d10/present.cpp — IDXGISwapChain::Present hooks for D3D10.
 *
 * D3D10 uses DXGI for presentation.  The swap chain's device is an
 * ID3D10Device*, obtained via GetDevice(IID_PPV_ARGS).  A context that was
 * not given a swap chain adopts the first one presented on its device.
 */

#include "d3d10_impl.hpp"
#include <compat/d3d11_slots.hpp>   // swap_chain slots are shared with D3D11
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::d3d10 {

// draw.cpp — heal after WARP may rewrite the device vtable on Present
void heal_device_draw_hooks(ID3D10Device* device) noexcept;

namespace sc  = ::vangfx::d3d11_slots::swap_chain;
namespace sc1 = ::vangfx::d3d11_slots::swap_chain1;

namespace {

using PresentFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                                const DXGI_PRESENT_PARAMETERS*);
using ResizeFn   = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT,
                                                UINT, DXGI_FORMAT, UINT);

hooks::Site s_present { sc::Present,       "IDXGISwapChain::Present (D3D10)" };
hooks::Site s_present1{ sc1::Present1,     "IDXGISwapChain1::Present1 (D3D10)" };
hooks::Site s_resize  { sc::ResizeBuffers, "IDXGISwapChain::ResizeBuffers (D3D10)" };

static ID3D10Device* device_of_swap_chain(IDXGISwapChain* sc_ptr) noexcept {
    ID3D10Device* dev = nullptr;
    if (FAILED(sc_ptr->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    dev->Release(); // the swap chain keeps it alive
    return dev;
}

static void before_present(IDXGISwapChain* swap_chain, UINT sync, UINT flags) {
    ID3D10Device* device = device_of_swap_chain(swap_chain);
    if (!device) return;
    HubReader reader;
    for (D3D10ContextImpl* ctx : contexts_for(device)) {
        if (!ctx->swap_chain()) ctx->adopt_swap_chain(swap_chain);
        if (ctx->swap_chain() != swap_chain) continue;

        const auto timing    = ctx->tick();
        const uint32_t frame = ctx->next_frame();
        D3D10FrameContextImpl cap_fc(*ctx, frame, device, dev_state(device));
        ctx->capture_present(cap_fc, timing.first);

        PresentEvent ev{};
        ev.sync_interval = sync;
        ev.flags         = flags;
        D3D10FrameContextImpl fc(*ctx, frame, device, dev_state(device));
        ctx->dispatch_present(fc, ev);
    }
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* self, UINT sync, UINT flags) {
    auto orig = s_present.original<PresentFn>(self);
    if (hub_idle() || internal()) return orig(self, sync, flags);
    before_present(self, sync, flags);
    HRESULT hr = orig(self, sync, flags);
    // WARP rewrites the device vtable on each Present; re-patch any changed slots.
    if (ID3D10Device* dev = device_of_swap_chain(self)) heal_device_draw_hooks(dev);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* params) {
    auto orig = s_present1.original<Present1Fn>(self);
    if (hub_idle() || internal()) return orig(self, sync, flags, params);
    before_present(self, sync, flags);
    return orig(self, sync, flags, params);
}

HRESULT STDMETHODCALLTYPE hook_resize(IDXGISwapChain* self,
                                       UINT count, UINT w, UINT h,
                                       DXGI_FORMAT fmt, UINT flags) {
    auto orig = s_resize.original<ResizeFn>(self);
    if (hub_idle() || internal()) return orig(self, count, w, h, fmt, flags);
    // Notify device-lost before the buffers are released.
    ID3D10Device* device = device_of_swap_chain(self);
    if (device) {
        HubReader reader;
        for (D3D10ContextImpl* ctx : contexts_for(device)) {
            ctx->dispatch_device_lost();
        }
    }
    HRESULT hr = orig(self, count, w, h, fmt, flags);
    if (SUCCEEDED(hr) && device) {
        HubReader reader;
        for (D3D10ContextImpl* ctx : contexts_for(device)) {
            D3D10FrameContextImpl fc(*ctx, ctx->current_frame_index(), device, nullptr);
            ctx->dispatch_device_reset(fc);
        }
    }
    return hr;
}

} // namespace

bool install_present_hooks(IDXGISwapChain* swap_chain) noexcept {
    bool ok = true;
    ok &= s_present.install(swap_chain, &hook_present);
    ok &= s_resize.install(swap_chain, &hook_resize);
    // Present1 is on IDXGISwapChain1; install only if the interface is there.
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        ok &= s_present1.install(sc1, &hook_present1);
        sc1->Release();
    }
    return ok;
}

} // namespace vangfx::backend::d3d10
