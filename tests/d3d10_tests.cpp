/**
 * tests/d3d10_tests.cpp — D3D10 GPU layer conformance tests + frontend
 * interception tests, all running on WARP.
 */

#include "gpu10.hpp"
#include <vangfx/vangfx.hpp>

#include <atomic>
#include <memory>
#include <string>

#include "../src/gpu/d3d10/device_d3d10.hpp"

using namespace vgx;
using Microsoft::WRL::ComPtr;

namespace {

std::unique_ptr<vangfx::Context> attach10(Dx10& g, IDXGISwapChain* sc = nullptr) {
    auto b = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D10)
        .log_level(vangfx::LogLevel::Info)
        .d3d10_device(g.dev.Get())
        .d3d10_swap_chain(sc);
    auto ctx = b.build();
    CHECK_MSG(ctx.has_value(), ctx ? "" : ctx.error().message);
    return std::move(*ctx);
}

} // namespace

// ── GPU layer tests ───────────────────────────────────────────────────────────

VGX_TEST(d3d10_device_create) {
    Dx10 d10;
    auto dev = vangfx::gpu::make_d3d10_device(d10.dev.Get());
    CHECK(dev != nullptr);
    CHECK(dev->api() == vangfx::gpu::Api::D3D10);
}

VGX_TEST(d3d10_create_texture) {
    Dx10 d10;
    auto dev = vangfx::gpu::make_d3d10_device(d10.dev.Get());

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

VGX_TEST(d3d10_create_pipeline) {
    Dx10 d10;
    auto dev = vangfx::gpu::make_d3d10_device(d10.dev.Get());

    vangfx::gpu::PipelineDesc pipe{};
    pipe.shaders.ps.hlsl  = "float4 main() : SV_Target { return float4(1,0,0,1); }";
    pipe.shaders.ps.entry = "main";

    auto res = dev->create_pipeline(pipe);
    CHECK(res.has_value());
    CHECK(res->valid());
}

VGX_TEST(d3d10_fullscreen_pass_pixel) {
    Dx10 d10;
    auto dev = vangfx::gpu::make_d3d10_device(d10.dev.Get());

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

    auto* native_tex = static_cast<ID3D10Texture2D*>(rt.native());
    uint32_t px = d10.read_pixel(native_tex, 4, 4);
    CHECK_EQ(px, kRed);
}

VGX_TEST(d3d10_state_restored) {
    Dx10 d10;
    auto dev = vangfx::gpu::make_d3d10_device(d10.dev.Get());

    static constexpr uint32_t W = 32, H = 32;
    auto game_tex = d10.target(W, H);
    ComPtr<ID3D10RenderTargetView> game_rtv;
    {
        D3D10_RENDER_TARGET_VIEW_DESC rtvd{};
        rtvd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvd.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE2D;
        d10.dev->CreateRenderTargetView(game_tex.Get(), &rtvd, &game_rtv);
    }
    d10.dev->OMSetRenderTargets(1, game_rtv.GetAddressOf(), nullptr);

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

    ID3D10RenderTargetView* restored_rtv = nullptr;
    d10.dev->OMGetRenderTargets(1, &restored_rtv, nullptr);
    CHECK(restored_rtv == game_rtv.Get());
    if (restored_rtv) restored_rtv->Release();
}

// ── Frontend interception tests ───────────────────────────────────────────────

VGX_TEST(d3d10_draws_every_frame) {
    Dx10 g;
    HWND hwnd = hidden_window10(8, 8);
    auto sc = g.swap_chain(hwnd, 8, 8);
    auto ctx = attach10(g, sc.Get());
    int draws = 0, presents = 0;
    uint64_t ps_hash = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent& e) { ++draws; ps_hash = e.ps_hash; });
    ctx->on_present([&](vangfx::FrameContext&, vangfx::PresentEvent&) { ++presents; });
    auto red = g.solid("1,0,0,1");

    ComPtr<ID3D10Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ComPtr<ID3D10RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);

    for (int f = 0; f < 60; ++f) {
        g.draw(rtv.Get(), 8, 8, red.Get());
        g.dev->Draw(3, 0); // second draw — same PS
        sc->Present(0, 0);
    }
    CHECK_EQ(draws, 120);
    CHECK_EQ(presents, 60);
    CHECK(ps_hash != 0);
    ctx.reset();
    DestroyWindow(hwnd);
}
