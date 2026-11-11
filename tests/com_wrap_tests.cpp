/**
 * tests/com_wrap_tests.cpp — vangfx::proxy::ComWrap<I> and ComWrapRegistry,
 * the D3D9 / DXGI passthrough wrappers, and the shared DXGI front end.
 *
 * Tests that run on WARP / the system d3d9.dll require no hardware.
 */

#include "harness.hpp"
#include <vangfx/proxy/com_wrap.hpp>
#include <vangfx/proxy/d3d9_wrap.hpp>
#include <vangfx/proxy/dxgi_wrap.hpp>
#include <vangfx/backend/dxgi/present.hpp>

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using namespace vgx;
using Microsoft::WRL::ComPtr;

// ── ComWrapRegistry ───────────────────────────────────────────────────────────

VGX_TEST(com_wrap_registry_basic) {
    // A minimal IUnknown stub for unit-testing the registry without COM infra.
    struct Stub : IUnknown {
        ULONG cnt = 1;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
        ULONG   STDMETHODCALLTYPE AddRef()  override { return ++cnt; }
        ULONG   STDMETHODCALLTYPE Release() override { return --cnt; }
    };
    Stub wrapped, real;
    vangfx::proxy::ComWrapRegistry::register_wrap(&wrapped, &real);
    CHECK_EQ(vangfx::proxy::ComWrapRegistry::real_of(&wrapped), static_cast<IUnknown*>(&real));
    CHECK_EQ(vangfx::proxy::ComWrapRegistry::wrap_of(&real),    static_cast<IUnknown*>(&wrapped));
    vangfx::proxy::ComWrapRegistry::unregister_wrap(&wrapped);
    CHECK(vangfx::proxy::ComWrapRegistry::real_of(&wrapped) == nullptr);
    CHECK(vangfx::proxy::ComWrapRegistry::wrap_of(&real)    == nullptr);
}

VGX_TEST(com_wrap_registry_typed_helpers) {
    struct Stub : IUnknown {
        ULONG cnt = 1;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
        ULONG   STDMETHODCALLTYPE AddRef()  override { return ++cnt; }
        ULONG   STDMETHODCALLTYPE Release() override { return --cnt; }
    };
    Stub w, r;
    vangfx::proxy::ComWrapRegistry::register_wrap(&w, &r);
    // Typed helpers cast through IUnknown.
    auto* got = vangfx::proxy::ComWrapRegistry::unwrap<IUnknown>(&w);
    CHECK_EQ(got, static_cast<IUnknown*>(&r));
    vangfx::proxy::ComWrapRegistry::unregister_wrap(&w);
}

// ── ComWrap<IDirect3D9> ───────────────────────────────────────────────────────

static IDirect3D9* make_real_d3d9() {
    HMODULE dll = LoadLibraryW(L"d3d9.dll");
    if (!dll) return nullptr;
    using Fn = IDirect3D9* (WINAPI*)(UINT);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(dll, "Direct3DCreate9"));
    return fn ? fn(D3D_SDK_VERSION) : nullptr;
}

VGX_TEST(com_wrap_d3d9_refcount) {
    ComPtr<IDirect3D9> real;
    real.Attach(make_real_d3d9());
    if (!real) { return; }  // d3d9.dll not available on this system (ARM64?)

    // Wrap the real object (wrap takes ownership; real ref stays at 1 from Attach).
    auto* wrap = new vangfx::proxy::IDirect3D9Wrap(real.Detach());

    // The wrapper starts at refcount 1; Release to 0 destroys it.
    ULONG refs = wrap->AddRef();
    CHECK_EQ(refs, 2u);
    refs = wrap->Release();
    CHECK_EQ(refs, 1u);
    wrap->Release();  // destroys; also releases the real
    // No crash = pass.
}

VGX_TEST(com_wrap_d3d9_qi_identity) {
    ComPtr<IDirect3D9> real;
    real.Attach(make_real_d3d9());
    if (!real) { return; }

    auto* wrap = new vangfx::proxy::IDirect3D9Wrap(real.Detach());
    // QI for IDirect3D9 should return the wrapper itself.
    IDirect3D9* qi = nullptr;
    HRESULT hr = wrap->QueryInterface(IID_PPV_ARGS(&qi));
    CHECK(SUCCEEDED(hr));
    CHECK_EQ(qi, static_cast<IDirect3D9*>(wrap));
    if (qi) qi->Release();

    // QI for IUnknown should also return the wrapper.
    IUnknown* unk = nullptr;
    hr = wrap->QueryInterface(IID_PPV_ARGS(&unk));
    CHECK(SUCCEEDED(hr));
    CHECK_EQ(unk, static_cast<IUnknown*>(wrap));
    if (unk) unk->Release();

    wrap->Release();
}

VGX_TEST(com_wrap_d3d9_registry) {
    ComPtr<IDirect3D9> real;
    real.Attach(make_real_d3d9());
    if (!real) { return; }

    IDirect3D9* raw_real = real.Detach();
    auto* wrap = new vangfx::proxy::IDirect3D9Wrap(raw_real);

    // Confirm the registry has the mapping.
    auto* found_real = vangfx::proxy::ComWrapRegistry::real_of(wrap);
    CHECK_EQ(found_real, static_cast<IUnknown*>(raw_real));

    // Release unregisters automatically.
    wrap->Release();
    CHECK(vangfx::proxy::ComWrapRegistry::real_of(wrap) == nullptr);
}

VGX_TEST(com_wrap_d3d9_passthrough_adapter_count) {
    ComPtr<IDirect3D9> real;
    real.Attach(make_real_d3d9());
    if (!real) { return; }

    IDirect3D9* raw_real = real.Detach();
    UINT expected = raw_real->GetAdapterCount();

    auto* wrap = new vangfx::proxy::IDirect3D9Wrap(raw_real);
    UINT through_wrap = wrap->GetAdapterCount();
    CHECK_EQ(through_wrap, expected);
    wrap->Release();
}

// ── IDirect3DDevice9Wrap ──────────────────────────────────────────────────────

VGX_TEST(com_wrap_d3d9_device_passthrough) {
    ComPtr<IDirect3D9> d3d;
    d3d.Attach(make_real_d3d9());
    if (!d3d) { return; }

    // Create a software-only device (no window, no hardware needed).
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth      = 1;
    pp.BackBufferHeight     = 1;
    pp.BackBufferFormat     = D3DFMT_X8R8G8B8;
    pp.BackBufferCount      = 1;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.Windowed             = TRUE;

    IDirect3DDevice9* real_dev = nullptr;
    HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, nullptr,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &pp, &real_dev);
    if (FAILED(hr) || !real_dev) {
        // HAL not available (CI runner with no GPU); skip.
        return;
    }

    auto* wrap_dev = new vangfx::proxy::IDirect3DDevice9Wrap(real_dev);

    // Passthrough: wrapped BeginScene/EndScene should forward without error.
    hr = wrap_dev->BeginScene();
    CHECK(SUCCEEDED(hr));
    hr = wrap_dev->EndScene();
    CHECK(SUCCEEDED(hr));

    wrap_dev->Release();
}

// ── IDXGISwapChainWrap + shared DXGI front end ────────────────────────────────

static bool make_warp_device_and_swapchain(
    ComPtr<ID3D11Device>& out_dev, ComPtr<IDXGISwapChain>& out_sc)
{
    // Create a minimal WARP D3D11 device.
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &out_dev, &fl, nullptr);
    if (FAILED(hr) || !out_dev) return false;

    // We need a window handle for CreateSwapChain.
    static const wchar_t* kClass = L"VanGFX.WrapTest";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kClass, L"", WS_OVERLAPPED,
                                 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    IDXGIDevice* dxgi_dev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    bool ok = false;
    if (SUCCEEDED(out_dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) &&
        SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferCount          = 2;
        desc.BufferDesc.Width     = 16;
        desc.BufferDesc.Height    = 16;
        desc.BufferDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SampleDesc.Count     = 1;
        desc.SwapEffect           = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.OutputWindow         = hwnd;
        desc.Windowed             = TRUE;
        ok = SUCCEEDED(factory->CreateSwapChain(out_dev.Get(), &desc, &out_sc));
    }
    if (factory)  factory->Release();
    if (adapter)  adapter->Release();
    if (dxgi_dev) dxgi_dev->Release();
    DestroyWindow(hwnd);
    return ok && out_sc;
}

VGX_TEST(com_wrap_dxgi_sc_passthrough) {
    ComPtr<ID3D11Device>  dev;
    ComPtr<IDXGISwapChain> sc;
    if (!make_warp_device_and_swapchain(dev, sc)) { return; }

    IDXGISwapChain* raw_sc = sc.Detach();
    DXGI_SWAP_CHAIN_DESC expected_desc{};
    raw_sc->GetDesc(&expected_desc);

    auto* wrap_sc = new vangfx::proxy::IDXGISwapChainWrap(raw_sc);

    DXGI_SWAP_CHAIN_DESC got_desc{};
    HRESULT hr = wrap_sc->GetDesc(&got_desc);
    CHECK(SUCCEEDED(hr));
    CHECK_EQ(got_desc.BufferDesc.Width,  expected_desc.BufferDesc.Width);
    CHECK_EQ(got_desc.BufferDesc.Height, expected_desc.BufferDesc.Height);

    wrap_sc->Release();
}

VGX_TEST(dxgi_present_hook_install_warp) {
    ComPtr<ID3D11Device>  dev;
    ComPtr<IDXGISwapChain> sc;
    if (!make_warp_device_and_swapchain(dev, sc)) { return; }

    vangfx::backend::dxgi::clear_backends();
    bool ok = vangfx::backend::dxgi::install_present_hooks(sc.Get());
    CHECK(ok);

    vangfx::backend::dxgi::uninstall_present_hooks();
}

VGX_TEST(dxgi_present_hook_callback_fires) {
    ComPtr<ID3D11Device>  dev;
    ComPtr<IDXGISwapChain> sc;
    if (!make_warp_device_and_swapchain(dev, sc)) { return; }

    vangfx::backend::dxgi::clear_backends();
    vangfx::backend::dxgi::reset_present_count();

    static uint32_t cb_count = 0;
    vangfx::backend::dxgi::BackendCallbacks cbs{};
    cbs.on_present = [](IDXGISwapChain*, UINT, UINT) { ++cb_count; };
    vangfx::backend::dxgi::register_backend(cbs);

    bool ok = vangfx::backend::dxgi::install_present_hooks(sc.Get());
    CHECK(ok);

    cb_count = 0;
    // Present through the FLIP swap chain: DXGI_PRESENT_TEST avoids a real flip.
    sc->Present(0, DXGI_PRESENT_TEST);

    CHECK(cb_count >= 1u);
    CHECK(vangfx::backend::dxgi::present_count() >= 1u);

    vangfx::backend::dxgi::uninstall_present_hooks();
    vangfx::backend::dxgi::clear_backends();
}

VGX_TEST(dxgi_prime_present_hooks_warp) {
    ComPtr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &dev, &fl, nullptr);
    if (FAILED(hr) || !dev) { return; }

    vangfx::backend::dxgi::clear_backends();
    vangfx::backend::dxgi::uninstall_present_hooks();

    bool ok = vangfx::backend::dxgi::prime_present_hooks(dev.Get());
    CHECK(ok);

    vangfx::backend::dxgi::uninstall_present_hooks();
    vangfx::backend::dxgi::clear_backends();
}
