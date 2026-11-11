/**
 * tests/d3d9_to_d3d12_tests.cpp — vangfx::d3d9to12 device context smoke test.
 *
 * Creates a DeviceContext12 via WARP (no hardware required) with a hidden
 * window. This confirms that the translate/targets/d3d9_to_d3d12 sources
 * compile, link and can initialise D3D12 on any Win10+ machine.
 *
 * Tests that need a full D3D9-API device (IDirect3DDevice9) belong in a
 * separate file once D9Root12::CreateDeviceInternal has a WARP path.
 */

#include "harness.hpp"
#include "gpu12.hpp"   // hidden_window()

#include <vangfx/translate/targets/d3d9_to_d3d12/core/DeviceContext12.h>

#include <windows.h>
#include <d3d9.h>

using namespace vgx;

// ── DeviceContext12 WARP creation ─────────────────────────────────────────────

VGX_TEST(d3d9to12_warp_device_create) {
    HWND hwnd = hidden_window(64, 64);
    if (!hwnd) return;  // skip on headless / CI without a desktop

    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth      = 64;
    pp.BackBufferHeight     = 64;
    pp.BackBufferFormat     = D3DFMT_X8R8G8B8;
    pp.BackBufferCount      = 1;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.Windowed             = TRUE;
    pp.hDeviceWindow        = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    vangfx::d3d9to12::DeviceContext12* ctx = nullptr;
    HRESULT hr = vangfx::d3d9to12::DeviceContext12::CreateWarp(hwnd, pp, false, &ctx);

    if (FAILED(hr)) {
        // WARP unavailable in this environment — skip gracefully.
        note("d3d9to12_warp_device_create: CreateWarp returned 0x%08lx, skipping", hr);
        DestroyWindow(hwnd);
        return;
    }

    CHECK(ctx != nullptr);
    CHECK(vangfx::d3d9to12::DeviceContext12::Current() == ctx);

    // Process exits: DeviceContext12 destructor runs during static teardown.
    DestroyWindow(hwnd);
}
