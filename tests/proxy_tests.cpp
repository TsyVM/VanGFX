/**
 * tests/proxy_tests.cpp — vangfx::proxy::SystemModule
 *
 * These tests load real system DLLs via SystemModule to verify the loading,
 * chain, and resolution paths.  They do not build a full generated proxy
 * (that would need a separate CMake sub-project); that is exercised by the
 * d3d9/dxgi integration tests below that go through the system DLL directly.
 */

#include "harness.hpp"
#include <vangfx/proxy/proxy.hpp>

#include <windows.h>
#include <d3d9.h>
#include <dxgi1_4.h>
#include <d3d11.h>
#include <wrl/client.h>

using namespace vgx;
using Microsoft::WRL::ComPtr;

// ── SystemModule ──────────────────────────────────────────────────────────────

VGX_TEST(proxy_system_module_load_d3d9) {
    vangfx::proxy::SystemModule mod;
    CHECK(mod.load("d3d9"));
    CHECK(mod.loaded());
    CHECK(mod.handle() != nullptr);
}

VGX_TEST(proxy_system_module_named_proc) {
    vangfx::proxy::SystemModule mod;
    CHECK(mod.load("d3d9"));
    void* fn = mod.proc("Direct3DCreate9");
    CHECK(fn != nullptr);
}

VGX_TEST(proxy_system_module_ordinal_proc) {
    // ordinal 37 = Direct3DCreate9
    vangfx::proxy::SystemModule mod;
    CHECK(mod.load("d3d9"));
    void* fn = mod.proc_by_ordinal(37);
    CHECK(fn != nullptr);
    // Named and ordinal should give the same address.
    void* named = mod.proc("Direct3DCreate9");
    CHECK_EQ(fn, named);
}

VGX_TEST(proxy_system_module_missing_name) {
    vangfx::proxy::SystemModule mod;
    CHECK(mod.load("d3d9"));
    void* fn = mod.proc("ThisFunctionDoesNotExist");
    CHECK(fn == nullptr);
}

VGX_TEST(proxy_system_module_load_once) {
    // load() is idempotent — calling it twice gives the same handle.
    vangfx::proxy::SystemModule mod;
    CHECK(mod.load("d3d9"));
    void* h1 = mod.handle();
    CHECK(mod.load("d3d9")); // second call: should not re-load
    void* h2 = mod.handle();
    CHECK_EQ(h1, h2);
}

// ── d3d9 through SystemModule ─────────────────────────────────────────────────

VGX_TEST(proxy_d3d9_create_via_system_module) {
    vangfx::proxy::SystemModule mod;
    CHECK(mod.load("d3d9"));
    using Fn = IDirect3D9* (WINAPI*)(UINT);
    auto create = reinterpret_cast<Fn>(mod.proc("Direct3DCreate9"));
    CHECK(create != nullptr);
    ComPtr<IDirect3D9> d3d;
    d3d.Attach(create(D3D_SDK_VERSION));
    CHECK(d3d != nullptr);
    // At least one adapter (the hardware or WARP one) must be reported.
    UINT count = d3d->GetAdapterCount();
    CHECK(count >= 1u);
}

// ── dxgi through SystemModule ─────────────────────────────────────────────────

VGX_TEST(proxy_dxgi_factory_via_system_module) {
    vangfx::proxy::SystemModule mod;
    CHECK(mod.load("dxgi"));
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    auto create = reinterpret_cast<Fn>(mod.proc("CreateDXGIFactory1"));
    CHECK(create != nullptr);
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = create(IID_PPV_ARGS(&factory));
    CHECK(SUCCEEDED(hr));
    CHECK(factory != nullptr);
}

VGX_TEST(proxy_dxgi_warp_d3d11_device) {
    vangfx::proxy::SystemModule dxgi_mod;
    CHECK(dxgi_mod.load("dxgi"));
    using FactoryFn = HRESULT (WINAPI*)(REFIID, void**);
    auto create_factory = reinterpret_cast<FactoryFn>(dxgi_mod.proc("CreateDXGIFactory1"));
    CHECK(create_factory != nullptr);

    ComPtr<IDXGIFactory1> factory;
    CHECK(SUCCEEDED(create_factory(IID_PPV_ARGS(&factory))));

    // Create a D3D11 device on WARP.
    ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &device, &fl, nullptr);
    CHECK(SUCCEEDED(hr));
    CHECK(device != nullptr);
    CHECK(fl >= D3D_FEATURE_LEVEL_11_0);
}
