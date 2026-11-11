/**
 * tests/gpu_tests.cpp — Phase 4 conformance tests for gpu::Device / gpu::Pass.
 *
 * D3D11 (WARP) tests run always.
 * D3D9 and D3D12 tests require VGX_HW=1 or the respective backend option.
 */

#include "harness.hpp"
#include "gpu11.hpp"

#include <vangfx/gpu/device.hpp>
#include <vangfx/gpu/pass.hpp>
#include <vangfx/gpu/pipeline_desc.hpp>
#include <vangfx/resources/texture.hpp>
#include <vangfx/resources/format.hpp>

// ── D3D11 backend ─────────────────────────────────────────────────────────────

#ifdef VANGFX_D3D11

#include "../src/gpu/d3d11/device_d3d11.hpp"
using Microsoft::WRL::ComPtr;

// A simple pass-through pixel shader that samples t0 and outputs red
static constexpr const char* kRedPS =
    "#include \"vangfx/fx/vangfx.hlsli\"\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    return float4(1,0,0,1);\n"
    "}\n";

// Passthrough PS that outputs backbuffer sample
static constexpr const char* kPassthroughPS =
    "Texture2D    tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    return tex.Sample(smp, uv);\n"
    "}\n";

VGX_TEST(d3d11_device_create) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());
    CHECK(dev != nullptr);
    CHECK(dev->api() == vangfx::gpu::Api::D3D11);
}

VGX_TEST(d3d11_create_texture) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    vangfx::TextureDesc td{};
    td.width = td.height = 64;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;

    auto res = dev->create_texture(td, nullptr);
    CHECK(res.has_value());
    CHECK(res->valid());
    CHECK(res->width()  == 64);
    CHECK(res->height() == 64);
}

VGX_TEST(d3d11_create_pipeline) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl  = "float4 main() : SV_Target { return float4(1,0,0,1); }";
    pipe.shaders.ps.entry = "main";

    auto res = dev->create_pipeline(pipe);
    CHECK(res.has_value());
    CHECK(res->valid());
}

VGX_TEST(d3d11_fullscreen_pass_pixel) {
    // Render a red fullscreen triangle to an off-screen RT, read a pixel back,
    // verify it is red.
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    static constexpr uint32_t W = 64, H = 64;

    vangfx::TextureDesc td{};
    td.width = W; td.height = H;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto rt_res = dev->create_texture(td, nullptr);
    CHECK(rt_res.has_value());
    auto& rt = *rt_res;

    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl  = "float4 main() : SV_Target { return float4(1,0,0,1); }";
    pipe.shaders.ps.entry = "main";
    auto pipe_res = dev->create_pipeline(pipe);
    CHECK(pipe_res.has_value());

    {
        auto pass = dev->begin_pass();
        pass->set_render_target(rt);
        pass->set_viewport(0, 0, W, H);
        pass->set_pipeline(*pipe_res);
        vangfx::gpu::VanGFXParams params{};
        pass->update_params(params);
        pass->draw_fullscreen();
        pass->end();
    }

    // Read pixel back via native D3D11 texture
    auto* native_tex = static_cast<ID3D11Texture2D*>(rt.native());
    uint32_t px = d11.read_pixel(native_tex, 4, 4);
    // R8G8B8A8_UNORM little-endian: R=0xFF, G=0x00, B=0x00, A=0xFF → 0xFF0000FF
    CHECK_EQ(px, vgx::kRed);
}

VGX_TEST(d3d11_state_restored) {
    // After a pass the game-side render target should be restored.
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    // Set a "game" RT
    static constexpr uint32_t W = 32, H = 32;
    auto game_tex = d11.target(W, H);
    ComPtr<ID3D11RenderTargetView> game_rtv;
    {
        D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
        rtvd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        d11.dev->CreateRenderTargetView(game_tex.Get(), &rtvd, &game_rtv);
    }
    d11.dc->OMSetRenderTargets(1, game_rtv.GetAddressOf(), nullptr);

    // Run a VanGFX pass
    vangfx::TextureDesc td{};
    td.width = W; td.height = H;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto van_rt = dev->create_texture(td, nullptr);
    CHECK(van_rt.has_value());

    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl  = "float4 main() : SV_Target { return float4(0,1,0,1); }";
    pipe.shaders.ps.entry = "main";
    auto pipe_res = dev->create_pipeline(pipe);
    CHECK(pipe_res.has_value());

    {
        auto pass = dev->begin_pass();
        pass->set_render_target(*van_rt);
        pass->set_viewport(0, 0, W, H);
        pass->set_pipeline(*pipe_res);
        vangfx::gpu::VanGFXParams params{};
        pass->update_params(params);
        pass->draw_fullscreen();
        pass->end();
    }

    // Check that the game's RT is restored
    ID3D11RenderTargetView* restored_rtv = nullptr;
    d11.dc->OMGetRenderTargets(1, &restored_rtv, nullptr);
    CHECK(restored_rtv == game_rtv.Get());
    if (restored_rtv) restored_rtv->Release();
}

VGX_TEST(d3d11_params_cbuffer) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    static constexpr uint32_t W = 16, H = 16;
    vangfx::TextureDesc td{};
    td.width = W; td.height = H;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto rt_res = dev->create_texture(td, nullptr);
    CHECK(rt_res.has_value());

    // PS that reads time from cb0 and uses it as the red channel
    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl =
        "cbuffer vangfx_params : register(b0) {\n"
        "    float4 vangfx_float4[8]; float vangfx_time; float vangfx_delta;\n"
        "    float2 vangfx_resolution;\n"
        "};\n"
        "float4 main() : SV_Target {\n"
        "    return float4(vangfx_time, 0, 0, 1);\n"
        "}\n";
    pipe.shaders.ps.entry = "main";
    auto pipe_res = dev->create_pipeline(pipe);
    CHECK(pipe_res.has_value());

    vangfx::gpu::VanGFXParams params{};
    params.time = 1.0f;

    {
        auto pass = dev->begin_pass();
        pass->set_render_target(*rt_res);
        pass->set_viewport(0, 0, W, H);
        pass->set_pipeline(*pipe_res);
        pass->update_params(params);
        pass->draw_fullscreen();
        pass->end();
    }

    auto* native = static_cast<ID3D11Texture2D*>(rt_res->native());
    uint32_t px = d11.read_pixel(native, 0, 0);
    // R should be ~255 (time=1.0), G=0, B=0, A=255
    uint8_t r = px & 0xFF;
    CHECK_MSG(r > 200, "expected red ~255 from vangfx_time=1.0, got " + vgx::str(r));
}

#endif // VANGFX_D3D11

// ── D3D9 backend ──────────────────────────────────────────────────────────────

#ifdef VANGFX_D3D9

#include "../src/gpu/d3d9/device_d3d9.hpp"
#include <d3d9.h>
#include <dxgi.h>

static IDirect3DDevice9* make_d3d9_warp_device() {
    // D3D9 has no WARP; create a device on the default adapter in software (REF).
    // For CI we use REF; for HW tests use D3DDEVTYPE_HAL.
    static IDirect3DDevice9* cached = nullptr;
    if (cached) return cached;

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return nullptr;

    static HWND hwnd = CreateWindowExW(0, L"STATIC", L"vgx_d3d9_test",
                                        WS_OVERLAPPED, 0, 0, 64, 64,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth  = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    pp.Windowed         = TRUE;
    pp.hDeviceWindow    = hwnd;

    D3DDEVTYPE dev_type = vgx::use_hardware() ? D3DDEVTYPE_HAL : D3DDEVTYPE_REF;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, dev_type, hwnd,
                                  D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                  &pp, &cached))) {
        d3d->Release();
        return nullptr;
    }
    d3d->Release();
    return cached;
}

VGX_TEST(d3d9_device_create) {
    auto* raw = make_d3d9_warp_device();
    if (!raw) { vgx::note("no D3D9 device (REF not installed)"); return; }

    auto dev = vangfx::gpu::make_d3d9_device(raw);
    CHECK(dev != nullptr);
    CHECK(dev->api() == vangfx::gpu::Api::D3D9);
}

VGX_TEST(d3d9_create_texture) {
    auto* raw = make_d3d9_warp_device();
    if (!raw) { vgx::note("no D3D9 device"); return; }

    auto dev = vangfx::gpu::make_d3d9_device(raw);

    vangfx::TextureDesc td{};
    td.width = td.height = 32;
    td.format = vangfx::SurfaceFormat::BGRA8;

    auto res = dev->create_texture(td, nullptr);
    CHECK(res.has_value());
    CHECK(res->valid());
}

VGX_TEST(d3d9_pipeline_create) {
    auto* raw = make_d3d9_warp_device();
    if (!raw) { vgx::note("no D3D9 device"); return; }

    auto dev = vangfx::gpu::make_d3d9_device(raw);

    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl  = "float4 main() : COLOR { return float4(1,0,0,1); }";
    pipe.shaders.ps.entry = "main";

    auto res = dev->create_pipeline(pipe);
    CHECK(res.has_value());
    CHECK(res->valid());
}

#endif // VANGFX_D3D9

// ── D3D12 backend ─────────────────────────────────────────────────────────────

#ifdef VANGFX_D3D12

#include "../src/gpu/d3d12/device_d3d12.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
using Microsoft::WRL::ComPtr;

static bool make_d3d12_warp(ID3D12Device** out_dev, ID3D12CommandQueue** out_queue) {
    ComPtr<IDXGIFactory4> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (!vgx::use_hardware()) {
        if (FAILED(fac->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
    } else {
        if (FAILED(fac->EnumAdapters(0, &adapter))) return false;
    }

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                  IID_PPV_ARGS(out_dev)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED((*out_dev)->CreateCommandQueue(&qd, IID_PPV_ARGS(out_queue)))) return false;

    return true;
}

VGX_TEST(d3d12_device_create) {
    ComPtr<ID3D12Device>       d3d12_dev;
    ComPtr<ID3D12CommandQueue> d3d12_q;
    if (!make_d3d12_warp(&d3d12_dev, &d3d12_q)) {
        vgx::note("no D3D12 device");
        return;
    }

    auto dev = vangfx::gpu::make_d3d12_device(d3d12_dev.Get(), d3d12_q.Get());
    CHECK(dev != nullptr);
    CHECK(dev->api() == vangfx::gpu::Api::D3D12);
}

VGX_TEST(d3d12_create_texture) {
    ComPtr<ID3D12Device>       d3d12_dev;
    ComPtr<ID3D12CommandQueue> d3d12_q;
    if (!make_d3d12_warp(&d3d12_dev, &d3d12_q)) { vgx::note("no D3D12 device"); return; }

    auto dev = vangfx::gpu::make_d3d12_device(d3d12_dev.Get(), d3d12_q.Get());

    vangfx::TextureDesc td{};
    td.width = td.height = 64;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;

    auto res = dev->create_texture(td, nullptr);
    CHECK(res.has_value());
    CHECK(res->valid());
    CHECK_EQ(res->width(),  64u);
    CHECK_EQ(res->height(), 64u);
}

VGX_TEST(d3d12_pipeline_create) {
    ComPtr<ID3D12Device>       d3d12_dev;
    ComPtr<ID3D12CommandQueue> d3d12_q;
    if (!make_d3d12_warp(&d3d12_dev, &d3d12_q)) { vgx::note("no D3D12 device"); return; }

    auto dev = vangfx::gpu::make_d3d12_device(d3d12_dev.Get(), d3d12_q.Get());

    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl  = "float4 main() : SV_Target { return float4(1,0,0,1); }";
    pipe.shaders.ps.entry = "main";

    auto res = dev->create_pipeline(pipe);
    CHECK(res.has_value());
    CHECK(res->valid());
}

VGX_TEST(d3d12_begin_pass) {
    ComPtr<ID3D12Device>       d3d12_dev;
    ComPtr<ID3D12CommandQueue> d3d12_q;
    if (!make_d3d12_warp(&d3d12_dev, &d3d12_q)) { vgx::note("no D3D12 device"); return; }

    auto dev = vangfx::gpu::make_d3d12_device(d3d12_dev.Get(), d3d12_q.Get());

    vangfx::TextureDesc td{};
    td.width = td.height = 64;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto rt_res = dev->create_texture(td, nullptr);
    CHECK(rt_res.has_value());

    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl  = "float4 main() : SV_Target { return float4(1,0,0,1); }";
    pipe.shaders.ps.entry = "main";
    auto pipe_res = dev->create_pipeline(pipe);
    CHECK(pipe_res.has_value());

    // begin_pass, set_render_target, set_pipeline, draw_fullscreen, end
    // If this doesn't crash we're good (no easy pixel readback for D3D12 in test)
    {
        auto pass = dev->begin_pass();
        CHECK(pass != nullptr);
        pass->set_render_target(*rt_res);
        pass->set_viewport(0, 0, 64, 64);
        pass->set_pipeline(*pipe_res);
        vangfx::gpu::VanGFXParams params{};
        params.time = 0.1f;
        pass->update_params(params);
        pass->draw_fullscreen();
        pass->end();
    }
    CHECK(true); // reached here without crash
}

#endif // VANGFX_D3D12
