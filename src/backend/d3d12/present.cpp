/**
 * src/backend/d3d12/present.cpp — IDXGISwapChain::Present and
 * IDXGISwapChain1::Present1.
 *
 * Swap chains share one class too, so Present is hooked once for all of them:
 * through the application's swap chain when the builder was given it, or else
 * through a small swap chain VanGFX makes on the context's queue for the
 * purpose (prime_present_hooks). Each call is matched to its contexts by the
 * swap chain's device -- GetDevice hands back the ID3D12Device, not the queue
 * (it refuses ID3D12CommandQueue) -- and a D3D11 swap chain, or a D3D12 one on
 * a device nothing is attached to, passes straight through.
 *
 * A context that was not given a swap chain adopts the first one presented on
 * its device. Present events fire for that swap chain only, so an overlay's or
 * a tool's swap chain on the same device does not advance the frame count.
 *
 * Post passes are executed on the queue the swap chain was created on when
 * VanGFX saw the creation (AutoAttach hooks the factory), else on the queue the
 * context was given, which is the game's direct queue in practice.
 */

#include "d3d12_impl.hpp"
#include "effect_bridge.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <vangfx/events/surface.hpp>

#include <dxgi1_4.h>

namespace vangfx::backend::d3d12 {

namespace sc = ::vangfx::d3d12_slots::swap_chain;

namespace {

using PresentFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

hooks::Site s_present { sc::Present,  "IDXGISwapChain::Present" };
hooks::Site s_present1{ sc::Present1, "IDXGISwapChain1::Present1" };

// The D3D12 device behind a swap chain, or null for any other swap chain.
ID3D12Device* device_of_swap_chain(IDXGISwapChain* swap_chain) noexcept {
    ID3D12Device* device = nullptr;
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device))) || !device) return nullptr;
    device->Release();   // the swap chain holds it
    return device;
}

// Runs the frame's work for each context this swap chain belongs to. Returns
// whether a callback suppressed the present.
bool before_present(IDXGISwapChain* swap_chain, UINT sync, UINT flags,
                    D3D12ContextImpl** presented, uint32_t& presented_count) {
    ID3D12Device* device = device_of_swap_chain(swap_chain);
    if (!device) return false;

    HubReader reader;
    const ContextSet cs = contexts_for(device);
    bool intercept = false;
    for (D3D12ContextImpl* ctx : cs) {
        if (!ctx->swap_chain()) ctx->adopt_swap_chain(swap_chain);
        if (ctx->swap_chain() != swap_chain) continue;
        presented[presented_count++] = ctx;

        const auto timing = ctx->tick();
        const uint32_t frame = ctx->next_frame();
        D3D12FrameContextImpl fc(*ctx, frame, swap_chain);
        ID3D12CommandQueue* queue = swap_chain_queue(swap_chain);
        if (!queue) queue = ctx->queue();

        // The frame a capture was taking ends here, before anything VanGFX
        // draws over the back buffer.
        snapshots_present(ctx->snapshots(), queue);
        ctx->capture_present(fc, timing.first);

        PresentEvent ev{};
        ev.sync_interval = sync;
        ev.flags         = flags;
        ctx->dispatch_present(fc, ev);
        intercept |= ev.intercept;
        if (!ev.intercept) ctx->post_process(swap_chain, queue, timing);
    }
    return intercept;
}

void after_present(D3D12ContextImpl* const* presented, uint32_t count) noexcept {
    if (!count) return;
    HubReader reader;
    // A context destroyed while Present ran is no longer attached; skip it.
    for (uint32_t i = 0; i < count; ++i)
        if (hub_attached(presented[i])) effect_bridge::collect(*presented[i]);
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* self, UINT sync, UINT flags) {
    auto orig = s_present.original<PresentFn>(self);
    if (hub_idle() || internal()) return orig(self, sync, flags);
    D3D12ContextImpl* presented[ContextSet::kMax];
    uint32_t count = 0;
    if (before_present(self, sync, flags, presented, count)) return S_OK;
    const HRESULT hr = orig(self, sync, flags);
    after_present(presented, count);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    auto orig = s_present1.original<Present1Fn>(self);
    if (hub_idle() || internal()) return orig(self, sync, flags, params);
    D3D12ContextImpl* presented[ContextSet::kMax];
    uint32_t count = 0;
    if (before_present(self, sync, flags, presented, count)) return S_OK;
    const HRESULT hr = orig(self, sync, flags, params);
    after_present(presented, count);
    return hr;
}

LRESULT CALLBACK prime_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

} // namespace

bool install_present_hooks(IDXGISwapChain* swap_chain) noexcept {
    if (!swap_chain) return false;
    bool ok = s_present.install(swap_chain, &hook_present);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1) {
        ok &= s_present1.install(sc1, &hook_present1);
        sc1->Release();
    }
    return ok;
}

bool prime_present_hooks(ID3D12CommandQueue* queue) noexcept {
    if (!queue) return false;
    InternalScope internal_work;

    static const wchar_t* kClass = L"VanGFX.PresentPrimer";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = &prime_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);   // fails harmlessly when already registered
    HWND hwnd = CreateWindowExW(0, kClass, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    bool ok = false;
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 16; d.Height = 16;
        d.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount      = 2;
        d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, hwnd, &d, nullptr, nullptr, &sc1)) && sc1) {
            ok = install_present_hooks(sc1);
            sc1->Release();
        }
        factory->Release();
    }
    DestroyWindow(hwnd);
    return ok;
}

void uninstall_present_hooks() noexcept {
    s_present.uninstall();
    s_present1.uninstall();
}

} // namespace vangfx::backend::d3d12
