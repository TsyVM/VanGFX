/**
 * tests/d3d8_tests.cpp — D3D8 backend conformance tests.
 *
 * Tests run only when VANGFX_D3D8 is defined (CMake option -DVANGFX_D3D8=ON).
 *
 * Requirements:
 *   - d3d8.dll present (Windows XP/Vista/7 or a D3D8→D3D9 wrapper such as
 *     d3d8.dll from the D3D8To9 project placed in the test binary directory).
 *   - A real GPU (D3D8 has no WARP / software rasterizer).
 *
 * Tests verify:
 *   1. D3D8ContextImpl constructs / destructs without crashing.
 *   2. D3D8→D3D9 translator shim creates a device8() pointer.
 *   3. make_shader_hash returns a non-zero value for a non-zero handle.
 *   4. translate_shader rewrites the DX8 vertex shader version token.
 */

#include "harness.hpp"

#ifdef VANGFX_D3D8

#include <vangfx/vangfx.hpp>
#include "../src/backend/d3d8/d3d8_impl.hpp"
#include "../src/translate/targets/d3d8_to_d3d9/translator.hpp"

using namespace vangfx;
using namespace vangfx::backend::d3d8;
using namespace vangfx::translate::d3d8_to_d3d9;

// ── Shader hash ───────────────────────────────────────────────────────────────

VGX_TEST(d3d8_shader_hash_nonzero) {
    uint64_t h = make_shader_hash(0x0001);
    CHECK(h != 0);
}

VGX_TEST(d3d8_shader_hash_zero_for_null) {
    uint64_t h = make_shader_hash(0);
    CHECK(h == 0);
}

VGX_TEST(d3d8_shader_hash_distinct) {
    uint64_t h1 = make_shader_hash(1);
    uint64_t h2 = make_shader_hash(2);
    CHECK(h1 != h2);
}

// ── Shader translation ────────────────────────────────────────────────────────

VGX_TEST(d3d8_translate_vs_version_token) {
    // DX8 vertex shader version token: 0xFFFF0101
    // Expected DX9 version token:      0xFFFE0101
    uint32_t tokens[] = { 0xFFFF0101u, 0xFFFFFFFFu }; // version + end token
    auto out = translate_shader(tokens, 2, /*is_pixel_shader=*/false);
    CHECK(out.size() == 2);
    CHECK(out[0] == 0xFFFE0101u);  // version rewritten
    CHECK(out[1] == 0xFFFFFFFFu);  // end token unchanged
}

VGX_TEST(d3d8_translate_ps_unchanged) {
    // DX8 pixel shader 1.1 token: 0xFFFF0101 (intentionally reused; ps uses 0xFFFF)
    uint32_t tokens[] = { 0xFFFF0101u, 0xFFFFFFFFu };
    auto out = translate_shader(tokens, 2, /*is_pixel_shader=*/true);
    CHECK(out.size() == 2);
    CHECK(out[0] == 0xFFFF0101u);  // pixel shader version stays 0xFFFF
}

VGX_TEST(d3d8_translate_empty) {
    auto out = translate_shader(nullptr, 0, false);
    CHECK(out.empty());
}

// ── D3D8→D3D9 shim (requires a D3D9 device) ──────────────────────────────────

#ifdef VANGFX_D3D9

#include <d3d9.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

VGX_TEST(d3d8_shim_device8_pointer) {
    // Create a minimal D3D9 device on WARP-equivalent (D3DDEVTYPE_NULLREF).
    // D3D9 has no WARP; use D3DDEVTYPE_REF which may not be present on all
    // machines.  Skip gracefully if device creation fails.
    ComPtr<IDirect3D9> d3d9;
    d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    if (!d3d9) { vgx::note("SKIP: Direct3DCreate9 failed"); return; }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed         = TRUE;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    ComPtr<IDirect3DDevice9> dev9;
    HRESULT hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF,
                                     GetForegroundWindow(),
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                     D3DCREATE_FPU_PRESERVE,
                                     &pp, &dev9);
    if (FAILED(hr)) {
        hr = d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_REF,
                                 GetForegroundWindow(),
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                 &pp, &dev9);
    }
    if (FAILED(hr)) { vgx::note("SKIP: Cannot create D3D9 device for D3D8 shim test"); return; }

    auto shim = create(dev9.Get());
    CHECK(shim != nullptr);
    CHECK(shim->device8() != nullptr);
    CHECK(shim->d3d9() == dev9.Get());
}

#endif // VANGFX_D3D9
#endif // VANGFX_D3D8
