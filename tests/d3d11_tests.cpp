/**
 * tests/d3d11_tests.cpp — the D3D11 backend, driven through a small "game"
 * (gpu11.hpp).
 *
 * The audit's measured failures each have a test: draw events stopping after
 * the first frame (draws_every_frame), replace_ps doing nothing (replace_ps),
 * and Destroy events going missing once the first buffer died (resources).
 */

#include "gpu11.hpp"
#include <vangfx/vangfx.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

using namespace vgx;

namespace {

std::unique_ptr<vangfx::Context> attach11(Dx11& g, IDXGISwapChain* sc = nullptr, std::string* log = nullptr) {
    auto b = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D11)
        .log_level(vangfx::LogLevel::Info)
        .d3d11_device(g.dev.Get())
        .d3d11_swap_chain(sc);
    if (log) b.log_callback([log](vangfx::LogLevel, std::string_view m) { log->append(m); log->push_back('\n'); });
    auto ctx = b.build();
    CHECK_MSG(ctx.has_value(), ctx ? "" : ctx.error().message);
    return std::move(*ctx);
}

} // namespace

// The runtime rewrites its own Draw entry at Flush and Present; draw events
// used to stop after the first frame. Sixty frames, with Flush and ClearState
// in the mix.
VGX_TEST(d3d11_draws_every_frame) {
    Dx11 g;
    HWND hwnd = hidden_window11(8, 8);
    auto sc = g.swap_chain(hwnd, 8, 8);
    auto ctx = attach11(g, sc.Get());
    int draws = 0, presents = 0;
    uint64_t ps_hash = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent& e) { ++draws; ps_hash = e.ps_hash; });
    ctx->on_present([&](vangfx::FrameContext&, vangfx::PresentEvent&) { ++presents; });
    auto red = g.solid("1,0,0,1");
    ComPtr<ID3D11Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    for (int f = 0; f < 60; ++f) {
        g.draw(rtv.Get(), 8, 8, red.Get());
        g.dc->DrawInstanced(3, 1, 0, 0);
        if (f % 7 == 0) g.dc->Flush();
        if (f % 11 == 0) g.dc->ClearState();
        sc->Present(0, 0);
    }
    CHECK_EQ(draws, 120);
    CHECK_EQ(presents, 60);
    CHECK(ps_hash != 0);
    ctx.reset();
    DestroyWindow(hwnd);
}

// replace_ps changes the draws its filter matches, and only those.
VGX_TEST(d3d11_replace_ps) {
    Dx11 g;
    auto ctx = attach11(g);
    vangfx::ReplaceFilter quads;
    quads.min_verts = 4;
    auto green = vangfx::replace_ps(*ctx, "float4 main() : SV_Target { return float4(0, 1, 0, 1); }", quads);
    CHECK_MSG(green.has_value(), green ? "" : green.error().message);
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(8, 8);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);

    g.draw(rtv.Get(), 8, 8, red.Get(), 3);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed);
    g.draw(rtv.Get(), 8, 8, red.Get(), 6);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kGreen);
    g.draw(rtv.Get(), 8, 8, red.Get(), 3);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed);
    ComPtr<ID3D11PixelShader> bound;
    g.dc->PSGetShader(&bound, nullptr, nullptr);
    CHECK(bound.Get() == red.Get());   // the game's shader is what is bound after the draw

    vangfx::remove_replacement(*ctx, *green);
    g.draw(rtv.Get(), 8, 8, red.Get(), 6);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed);
}

// Create and Destroy events balance, for every buffer, not only the first.
VGX_TEST(d3d11_resources) {
    Dx11 g;
    auto ctx = attach11(g);
    int created = 0, destroyed = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create) ++created;
        if (e.action == vangfx::ResourceAction::Destroy) ++destroyed;
    });
    for (int i = 0; i < 50; ++i) {
        D3D11_BUFFER_DESC d{ 256, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        ComPtr<ID3D11Buffer> a, b;
        g.dev->CreateBuffer(&d, nullptr, &a);
        g.dev->CreateBuffer(&d, nullptr, &b);
    }
    CHECK_EQ(created, 100);
    CHECK_EQ(destroyed, 100);
}

// Buffer readback by the handle the Create event gave.
VGX_TEST(d3d11_buffer_readback) {
    Dx11 g;
    auto ctx = attach11(g);
    uint64_t id = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create && e.kind == vangfx::ResourceKind::VertexBuffer && !id)
            id = e.vertex_buffer.id;
    });
    uint32_t data[64];
    for (int i = 0; i < 64; ++i) data[i] = 0x2000 + i;
    D3D11_BUFFER_DESC d{ sizeof(data), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init{ data, 0, 0 };
    ComPtr<ID3D11Buffer> vb;
    g.dev->CreateBuffer(&d, &init, &vb);
    CHECK(id != 0);
    uint32_t got = 0;
    bool ok = false;
    ctx->on_draw([&](vangfx::FrameContext& f, vangfx::DrawEvent&) {
        auto r = f.readback(vangfx::VertexBufferHandle{ id }, 20, 4);
        ok = r.has_value();
        if (ok) std::memcpy(&got, r->bytes.data(), 4);
    });
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(8, 8);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
    g.draw(rtv.Get(), 8, 8, red.Get());
    CHECK(ok);
    CHECK_EQ(got, 0x2005u);
}

// The render-target event names the target by its Create event's id.
VGX_TEST(d3d11_render_targets) {
    Dx11 g;
    auto ctx = attach11(g);
    uint64_t created = 0, bound = 0;
    uint32_t w = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create && e.kind == vangfx::ResourceKind::RenderTarget) created = e.render_target.id;
    });
    ctx->on_render_target([&](vangfx::FrameContext&, vangfx::RenderTargetEvent& e) { bound = e.targets[0].id; w = e.width; });
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(16, 8);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
    g.draw(rtv.Get(), 16, 8, red.Get());
    CHECK(created != 0);
    CHECK_EQ(bound, created);
    CHECK_EQ(w, 16u);
}

// Draws recorded on deferred contexts, on several threads at once.
VGX_TEST(d3d11_deferred) {
    Dx11 g;
    auto ctx = attach11(g);
    std::atomic<int> draws{ 0 }, deferred_draws{ 0 };
    ctx->on_draw([&](vangfx::FrameContext& f, vangfx::DrawEvent&) {
        ++draws;
        if (f.native_context() != g.dc.Get()) ++deferred_draws;
    });
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(8, 8);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
    std::vector<ComPtr<ID3D11CommandList>> lists(4);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            ComPtr<ID3D11DeviceContext> def;
            g.dev->CreateDeferredContext(0, &def);
            for (int i = 0; i < 50; ++i) g.draw(rtv.Get(), 8, 8, red.Get(), 3, def.Get());
            def->FinishCommandList(FALSE, &lists[t]);
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(deferred_draws.load(), 200);
    for (auto& l : lists) g.dc->ExecuteCommandList(l.Get(), FALSE);
    g.draw(rtv.Get(), 8, 8, red.Get());
    CHECK_EQ(draws.load(), 201);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed);
}

// A post pass runs over the back buffer at Present, without disturbing the
// game's bindings.
VGX_TEST(d3d11_post_pass) {
    Dx11 g;
    HWND hwnd = hidden_window11(8, 8);
    auto sc = g.swap_chain(hwnd, 8, 8);
    std::string log;
    auto ctx = attach11(g, nullptr, &log);   // the swap chain is adopted at its first Present
    auto pass = vangfx::inject_ps_post(*ctx, "color = float4(color.b, color.g, color.r, 1);");
    CHECK_MSG(pass.has_value(), pass ? "" : pass.error().message);
    auto red = g.solid("1,0,0,1");
    ComPtr<ID3D11Texture2D> bb;
    sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    g.draw(rtv.Get(), 8, 8, red.Get());
    sc->Present(0, 0);
    const uint32_t px = g.read_pixel(bb.Get(), 2, 2);
    CHECK_MSG(px == kBlue, "pixel " + std::to_string(px) + ", log:\n" + log);
    ComPtr<ID3D11PixelShader> bound;
    g.dc->PSGetShader(&bound, nullptr, nullptr);
    CHECK(bound.Get() == red.Get());   // the game's binding survived the pass
    vangfx::remove_injection(*ctx, *pass);
    ctx.reset();
    rtv.Reset(); bb.Reset(); sc.Reset();
    DestroyWindow(hwnd);
}

// Direct3D 11.1: a device context state swapped in and out, as Direct2D does
// around its drawing. The swap rewrites the table, and the first call after it
// -- here a Get VanGFX does not hook -- rewrites it again: the draw after that
// is missed unless the swap is settled. And each state's draws run the shaders
// that state holds, which nobody set again after the swap: that is what they
// report. (Every draw has a pipeline: a draw in a state with nothing bound
// leaves the target zeroed on some hardware, VanGFX or not.)
VGX_TEST(d3d11_swap_context_state) {
    Dx11 g;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11DeviceContext1> dc1;
    CHECK(SUCCEEDED(g.dev.As(&dev1)));
    CHECK(SUCCEEDED(g.dc.As(&dc1)));
    auto ctx = attach11(g);
    int draws = 0;
    uint64_t last_ps = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent& e) { ++draws; last_ps = e.ps_hash; });
    auto red  = g.solid("1,0,0,1");
    auto blue = g.solid("0,0,1,1");
    auto rt = g.target(8, 8);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
    g.draw(rtv.Get(), 8, 8, red.Get());
    const uint64_t red_hash = last_ps;
    CHECK(red_hash != 0);

    // The other state gets a pipeline of its own.
    const D3D_FEATURE_LEVEL fl = g.dev->GetFeatureLevel();
    ComPtr<ID3DDeviceContextState> other;
    CHECK(SUCCEEDED(dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &other)));
    {
        ComPtr<ID3DDeviceContextState> game;
        dc1->SwapDeviceContextState(other.Get(), &game);
        g.draw(rtv.Get(), 8, 8, blue.Get());
        dc1->SwapDeviceContextState(game.Get(), nullptr);
    }
    const uint64_t blue_hash = last_ps;
    CHECK(blue_hash != 0 && blue_hash != red_hash);

    for (int round = 0; round < 3; ++round) {
        ComPtr<ID3DDeviceContextState> game;
        dc1->SwapDeviceContextState(other.Get(), &game);
        D3D11_PRIMITIVE_TOPOLOGY t{};
        draws = 0;
        g.dc->IAGetPrimitiveTopology(&t);
        g.dc->Draw(3, 0);
        CHECK_EQ(draws, 1);
        CHECK_EQ(last_ps, blue_hash);

        // Back to the game's state, asked for by name in some rounds only.
        if (round == 1) dc1->SwapDeviceContextState(game.Get(), nullptr);
        else            dc1->SwapDeviceContextState(game.Get(), &other);
        draws = 0;
        g.dc->IAGetPrimitiveTopology(&t);
        g.dc->Draw(3, 0);
        CHECK_EQ(draws, 1);
        CHECK_EQ(last_ps, red_hash);
    }
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed);
}

// Direct3D 11.3/11.4 and ID3D11Multithread: Flush1, Signal, Wait and turning
// multithread protection on and off each rewrite the table; the draw straight
// after each is reported.
VGX_TEST(d3d11_flush1_fences_protection) {
    Dx11 g;
    ComPtr<ID3D11DeviceContext3> dc3;
    ComPtr<ID3D11DeviceContext4> dc4;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11Multithread> mt;
    CHECK(SUCCEEDED(g.dc.As(&dc3)));
    CHECK(SUCCEEDED(g.dc.As(&dc4)));
    CHECK(SUCCEEDED(g.dev.As(&dev5)));
    CHECK(SUCCEEDED(g.dc.As(&mt)));
    auto ctx = attach11(g);
    int draws = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++draws; });
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(8, 8);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
    g.draw(rtv.Get(), 8, 8, red.Get());
    ComPtr<ID3D11Fence> fence;
    CHECK(SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));

    draws = 0;
    dc3->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
    g.dc->Draw(3, 0);
    CHECK_EQ(draws, 1);
    dc4->Signal(fence.Get(), 1);
    g.dc->Draw(3, 0);
    CHECK_EQ(draws, 2);
    dc4->Wait(fence.Get(), 1);
    g.dc->Draw(3, 0);
    CHECK_EQ(draws, 3);
    mt->SetMultithreadProtected(TRUE);
    g.dc->Draw(3, 0);
    CHECK_EQ(draws, 4);
    mt->SetMultithreadProtected(FALSE);
    g.dc->Draw(3, 0);
    CHECK_EQ(draws, 5);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed);
}

// Two contexts on one device, one destroyed mid-way; afterwards the game
// renders on untouched.
VGX_TEST(d3d11_two_contexts) {
    Dx11 g;
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(8, 8);
    ComPtr<ID3D11RenderTargetView> rtv;
    g.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
    int a = 0, b = 0;
    auto ca = attach11(g);
    auto cb = attach11(g);
    ca->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++a; });
    cb->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++b; });
    g.draw(rtv.Get(), 8, 8, red.Get());
    CHECK_EQ(a, 1); CHECK_EQ(b, 1);
    ca.reset();
    g.draw(rtv.Get(), 8, 8, red.Get());
    CHECK_EQ(a, 1); CHECK_EQ(b, 2);
    cb.reset();
    g.draw(rtv.Get(), 8, 8, red.Get());
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed);
}
