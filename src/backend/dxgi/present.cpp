/**
 * src/backend/dxgi/present.cpp — shared DXGI swap chain front end.
 *
 * Owns exactly one set of hooks::Site for IDXGISwapChain::Present,
 * IDXGISwapChain1::Present1, and IDXGISwapChain::ResizeBuffers.  Every active
 * backend (D3D10 / D3D11 / D3D12) registers callbacks and is dispatched to
 * from the single hook.
 *
 * The VanHooks vtable patch is idempotent: install() is a no-op when the slot
 * is already patched.  Multiple backends calling install_present_hooks() with
 * different swap chain pointers of the same class is safe.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vangfx/backend/dxgi/present.hpp>
#include <compat/d3d11_slots.hpp>
#include <compat/hook_site.hpp>

#include <atomic>
#include <array>
#include <d3d11.h>
#include <dxgi1_4.h>

namespace vangfx::backend::dxgi {

namespace sc  = ::vangfx::d3d11_slots::swap_chain;
namespace sc1 = ::vangfx::d3d11_slots::swap_chain1;

namespace {

using PresentFn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn   = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

hooks::Site s_present { sc::Present,       "IDXGISwapChain::Present (dxgi)" };
hooks::Site s_present1{ sc1::Present1,     "IDXGISwapChain1::Present1 (dxgi)" };
hooks::Site s_resize  { sc::ResizeBuffers, "IDXGISwapChain::ResizeBuffers (dxgi)" };

std::array<BackendCallbacks, kMaxBackends> s_backends{};
std::atomic<int>      s_backend_count{ 0 };
std::atomic<uint32_t> s_present_count{ 0 };

void dispatch_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const int n = s_backend_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (s_backends[i].on_present) s_backends[i].on_present(sc, sync, flags);
    s_present_count.fetch_add(1, std::memory_order_relaxed);
}

void dispatch_present1(IDXGISwapChain1* sc1, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    const int n = s_backend_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (s_backends[i].on_present1) s_backends[i].on_present1(sc1, sync, flags, p);
    s_present_count.fetch_add(1, std::memory_order_relaxed);
}

void dispatch_resize(IDXGISwapChain* sc, UINT cnt, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    const int n = s_backend_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (s_backends[i].on_resize) s_backends[i].on_resize(sc, cnt, w, h, fmt, flags);
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* self, UINT sync, UINT flags) {
    auto orig = s_present.original<PresentFn>(self);
    dispatch_present(self, sync, flags);
    return orig(self, sync, flags);
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    auto orig = s_present1.original<Present1Fn>(self);
    dispatch_present1(self, sync, flags, p);
    return orig(self, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE hook_resize(IDXGISwapChain* self, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    auto orig = s_resize.original<ResizeFn>(self);
    dispatch_resize(self, n, w, h, fmt, flags);
    return orig(self, n, w, h, fmt, flags);
}

LRESULT CALLBACK prime_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

} // namespace

bool register_backend(BackendCallbacks cb) noexcept {
    int idx = s_backend_count.load(std::memory_order_acquire);
    if (idx >= kMaxBackends) return false;
    s_backends[idx] = cb;
    s_backend_count.store(idx + 1, std::memory_order_release);
    return true;
}

void clear_backends() noexcept {
    s_backend_count.store(0, std::memory_order_release);
    s_backends = {};
}

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

bool prime_present_hooks(IUnknown* device_or_queue) noexcept {
    if (!device_or_queue) return false;

    static const wchar_t* kClass = L"VanGFX.PresentPrimer.Dxgi";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = &prime_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kClass, L"", WS_OVERLAPPED, 0, 0, 16, 16,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    bool ok = false;
    IDXGIFactory2* factory = nullptr;

    // Try to get the factory from the device's DXGI device.
    IDXGIDevice* dxgi_dev = nullptr;
    if (SUCCEEDED(device_or_queue->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter))) {
            adapter->GetParent(IID_PPV_ARGS(&factory));
            adapter->Release();
        }
        dxgi_dev->Release();
    }
    // Fallback: create a fresh factory (works for D3D12 command queues too).
    if (!factory) CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    if (factory) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 16; d.Height = 16;
        d.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount      = 2;
        d.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1p = nullptr;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(device_or_queue, hwnd, &d, nullptr, nullptr, &sc1p)) && sc1p) {
            ok = install_present_hooks(sc1p);
            sc1p->Release();
        }
        factory->Release();
    }

    DestroyWindow(hwnd);
    return ok;
}

void uninstall_present_hooks() noexcept {
    s_present.uninstall();
    s_present1.uninstall();
    s_resize.uninstall();
}

uint32_t present_count()       noexcept { return s_present_count.load(std::memory_order_relaxed); }
void     reset_present_count() noexcept { s_present_count.store(0, std::memory_order_relaxed); }

} // namespace vangfx::backend::dxgi
