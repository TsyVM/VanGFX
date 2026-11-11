/**
 * src/backend/d3d11/present.cpp — IDXGISwapChain::Present, Present1 and
 * ResizeBuffers.
 *
 * Hooked once for the swap chain class, through the application's swap chain
 * or a small one made for the purpose. Each call is matched to its contexts by
 * the swap chain's device (GetDevice(ID3D11Device)); a context that was not
 * given a swap chain adopts the first presented on its device. Present runs the
 * frame's work -- the frame counter, capture, callbacks, post passes -- and then
 * repairs the immediate context's table, which Present rewrites.
 *
 * ResizeBuffers is reported as device lost / reset: the back buffers are gone
 * and recreated, and anything holding a view of one must let go first.
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::d3d11 {

namespace sc  = ::vangfx::d3d11_slots::swap_chain;
namespace sc1 = ::vangfx::d3d11_slots::swap_chain1;

namespace {

using PresentFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn   = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

hooks::Site s_present { sc::Present,        "IDXGISwapChain::Present (D3D11)" };
hooks::Site s_present1{ sc1::Present1,      "IDXGISwapChain1::Present1 (D3D11)" };
hooks::Site s_resize  { sc::ResizeBuffers,  "IDXGISwapChain::ResizeBuffers (D3D11)" };

ID3D11Device* device_of_swap_chain(IDXGISwapChain* swap_chain) noexcept {
    ID3D11Device* device = nullptr;
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device))) || !device) return nullptr;
    device->Release();   // the swap chain holds it
    return device;
}

// The frame's work for each context this swap chain belongs to. Returns
// whether a callback suppressed the present.
bool before_present(IDXGISwapChain* swap_chain, UINT sync, UINT flags, D3D11ContextImpl** presented, uint32_t& count) {
    ID3D11Device* device = device_of_swap_chain(swap_chain);
    if (!device) return false;
    HubReader reader;
    bool intercept = false;
    for (D3D11ContextImpl* ctx : contexts_for(device)) {
        if (!ctx->swap_chain()) ctx->adopt_swap_chain(swap_chain);
        if (ctx->swap_chain() != swap_chain) continue;
        presented[count++] = ctx;

        const auto timing = ctx->tick();
        const uint32_t frame = ctx->next_frame();
        D3D11FrameContextImpl fc(*ctx, frame, ctx->immediate(), dc_state(ctx->immediate()));
        // The frame a capture was taking ends here, before VanGFX's post passes.
        ctx->capture_present(fc, timing.first);

        PresentEvent ev{};
        ev.sync_interval = sync;
        ev.flags         = flags;
        ctx->dispatch_present(fc, ev);
        intercept |= ev.intercept;
        if (!ev.intercept) ctx->post_process(swap_chain, timing);
    }
    return intercept;
}

void after_present(D3D11ContextImpl* const* presented, uint32_t count) noexcept {
    if (!count) return;
    HubReader reader;
    for (uint32_t i = 0; i < count; ++i)
        if (hub_attached(presented[i])) heal_context(presented[i]->immediate());
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* self, UINT sync, UINT flags) {
    auto orig = s_present.original<PresentFn>(self);
    if (hub_idle() || internal()) return orig(self, sync, flags);
    D3D11ContextImpl* presented[ContextSet::kMax];
    uint32_t count = 0;
    if (before_present(self, sync, flags, presented, count)) return S_OK;
    const HRESULT hr = orig(self, sync, flags);
    after_present(presented, count);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    auto orig = s_present1.original<Present1Fn>(self);
    if (hub_idle() || internal()) return orig(self, sync, flags, params);
    D3D11ContextImpl* presented[ContextSet::kMax];
    uint32_t count = 0;
    if (before_present(self, sync, flags, presented, count)) return S_OK;
    const HRESULT hr = orig(self, sync, flags, params);
    after_present(presented, count);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_resize(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    auto orig = s_resize.original<ResizeFn>(self);
    if (hub_idle() || internal()) return orig(self, n, w, h, fmt, flags);
    ID3D11Device* device = device_of_swap_chain(self);
    D3D11ContextImpl* mine[ContextSet::kMax];
    uint32_t count = 0;
    {
        HubReader reader;
        for (D3D11ContextImpl* ctx : contexts_for(device)) {
            if (ctx->swap_chain() != self) continue;
            mine[count++] = ctx;
            ctx->release_post();   // a view of a back buffer would make the resize fail
            ctx->dispatch_device_lost();
        }
    }
    const HRESULT hr = orig(self, n, w, h, fmt, flags);
    if (SUCCEEDED(hr) && count) {
        HubReader reader;
        for (uint32_t i = 0; i < count; ++i) {
            if (!hub_attached(mine[i])) continue;
            D3D11FrameContextImpl fc(*mine[i], mine[i]->current_frame_index());
            mine[i]->dispatch_device_reset(fc);
        }
    }
    return hr;
}

LRESULT CALLBACK prime_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

} // namespace

bool install_present_hooks(IDXGISwapChain* swap_chain) noexcept {
    if (!swap_chain) return false;
    bool ok = s_present.install(swap_chain, &hook_present);
    ok &= s_resize.install(swap_chain, &hook_resize);
    IDXGISwapChain1* sc1p = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc1p))) && sc1p) {
        ok &= s_present1.install(sc1p, &hook_present1);
        sc1p->Release();
    }
    return ok;
}

bool prime_present_hooks(ID3D11Device* device) noexcept {
    InternalScope internal_work;
    static const wchar_t* kClass = L"VanGFX.PresentPrimer11";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = &prime_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kClass, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    bool ok = false;
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 16; d.Height = 16;
        d.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount      = 2;
        d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1p = nullptr;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(device, hwnd, &d, nullptr, nullptr, &sc1p)) && sc1p) {
            ok = install_present_hooks(sc1p);
            sc1p->Release();
        }
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
    DestroyWindow(hwnd);
    return ok;
}

void uninstall_present_hooks() noexcept {
    s_present.uninstall();
    s_present1.uninstall();
    s_resize.uninstall();
}

}  // namespace vangfx::backend::d3d11
