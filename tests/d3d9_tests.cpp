/**
 * tests/d3d9_tests.cpp — the D3D9 backend, driven through a small "game"
 * (gpu9.hpp) on the hardware adapter (D3D9 has no WARP).
 *
 * What the audit measured and what the rebuild changed each have a test: draws
 * reported across state-block recording and Reset (both rewrite the device's
 * table), resource/state/query hooks that were never installed, replacements
 * for shader model 2 and fixed-function draws (DirectX 9.0c games), post
 * passes, Reset with VanGFX's own default-pool resources alive, several
 * devices, a device's death, and the Ex device.
 */

#include "gpu9.hpp"
#include <hash.hpp>
#include <vangfx/vangfx.hpp>

#include <memory>
#include <string>

using namespace vgx;

namespace {

std::unique_ptr<vangfx::Context> attach9(IDirect3DDevice9* dev, std::string* log = nullptr) {
    auto b = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D9)
        .log_level(vangfx::LogLevel::Info)
        .d3d9_device(dev);
    if (log) b.log_callback([log](vangfx::LogLevel, std::string_view m) { log->append(m); log->push_back('\n'); });
    auto ctx = b.build();
    CHECK_MSG(ctx.has_value(), ctx ? "" : ctx.error().message);
    return std::move(*ctx);
}

constexpr const char* kGreenPS = "float4 main() : COLOR0 { return float4(0, 1, 0, 1); }";

} // namespace

// Sixty frames of two draws each, with state blocks recorded and applied in
// the mix: BeginStateBlock swaps seventeen setters out of the table and
// EndStateBlock swaps them back, dropping any hook in them.
VGX_TEST(d3d9_draws_every_frame) {
    Dx9 g;
    auto ctx = attach9(g.dev.Get());
    int draws = 0, presents = 0;
    uint64_t ps_hash = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent& e) { ++draws; ps_hash = e.ps_hash; });
    ctx->on_present([&](vangfx::FrameContext&, vangfx::PresentEvent&) { ++presents; });
    auto red = g.solid("1,0,0,1");
    auto bb = g.back_buffer();
    for (int f = 0; f < 60; ++f) {
        if (f % 7 == 0) {
            ComPtr<IDirect3DStateBlock9> sb;
            g.dev->BeginStateBlock();
            g.dev->SetPixelShader(red.Get());
            g.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            g.dev->EndStateBlock(&sb);
            sb->Apply();
        }
        g.draw(bb.Get(), red.Get());
        g.draw(bb.Get(), red.Get(), 3, nullptr, true);   // DrawPrimitiveUP
        g.dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    CHECK_EQ(draws, 120);
    CHECK_EQ(presents, 60);
    CHECK(ps_hash != 0);
}

// replace_ps changes the draws its filter matches, and only those.
VGX_TEST(d3d9_replace_ps) {
    Dx9 g;
    auto ctx = attach9(g.dev.Get());
    vangfx::ReplaceFilter quads;
    quads.min_verts = 4;
    auto green = vangfx::replace_ps(*ctx, kGreenPS, quads);
    CHECK_MSG(green.has_value(), green ? "" : green.error().message);
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(8, 8);

    g.draw(rt.Get(), red.Get(), 3);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed9);
    g.draw(rt.Get(), red.Get(), 6);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kGreen9);
    g.draw(rt.Get(), red.Get(), 3);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed9);
    ComPtr<IDirect3DPixelShader9> bound;
    g.dev->GetPixelShader(&bound);
    CHECK(bound.Get() == red.Get());   // the game's shader is what is bound after the draw

    vangfx::remove_replacement(*ctx, *green);
    g.draw(rt.Get(), red.Get(), 6);
    CHECK_EQ(g.read_pixel(rt.Get(), 1, 1), kRed9);
}

// A DirectX 9.0c game drawing with shader model 2, or with no vertex shader
// at all: a ps_3_0 replacement cannot run behind either, and is recompiled.
VGX_TEST(d3d9_replace_ps_sm2_and_fixed_function) {
    Dx9 g;
    std::string log;
    auto ctx = attach9(g.dev.Get(), &log);
    auto green = vangfx::replace_ps(*ctx, kGreenPS, {});
    CHECK_MSG(green.has_value(), green ? "" : green.error().message);
    auto red2 = g.solid("1,0,0,1", "ps_2_0");
    auto rt = g.target(8, 8);

    g.draw(rt.Get(), red2.Get(), 3, g.vs2.Get());
    CHECK_MSG(g.read_pixel(rt.Get(), 1, 1) == kGreen9, "vs_2_0 draw, log:\n" + log);
    g.draw(rt.Get(), red2.Get(), 3, nullptr, true);
    CHECK_MSG(g.read_pixel(rt.Get(), 1, 1) == kGreen9, "fixed-function draw, log:\n" + log);
    auto red3 = g.solid("1,0,0,1");
    g.draw(rt.Get(), red3.Get(), 3);
    CHECK_MSG(g.read_pixel(rt.Get(), 1, 1) == kGreen9, "vs_3_0 draw, log:\n" + log);
    ComPtr<IDirect3DPixelShader9> bound;
    g.dev->GetPixelShader(&bound);
    CHECK(bound.Get() == red3.Get());
}

// Create and Destroy events balance for every buffer and texture, and a write
// through Unlock is an Update.
VGX_TEST(d3d9_resources) {
    Dx9 g;
    auto ctx = attach9(g.dev.Get());
    int created = 0, destroyed = 0, updated = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create) ++created;
        if (e.action == vangfx::ResourceAction::Destroy) ++destroyed;
        if (e.action == vangfx::ResourceAction::Update) ++updated;
    });
    for (int i = 0; i < 50; ++i) {
        ComPtr<IDirect3DVertexBuffer9> vb;
        ComPtr<IDirect3DTexture9> tex;
        g.dev->CreateVertexBuffer(256, 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
        g.dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
    }
    CHECK_EQ(created, 100);
    CHECK_EQ(destroyed, 100);

    ComPtr<IDirect3DVertexBuffer9> vb;
    ComPtr<IDirect3DIndexBuffer9> ib;
    g.dev->CreateVertexBuffer(256, 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
    g.dev->CreateIndexBuffer(256, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr);
    void* p = nullptr;
    vb->Lock(0, 0, &p, 0); vb->Unlock();
    ib->Lock(0, 0, &p, 0); ib->Unlock();
    CHECK_EQ(updated, 2);
}

// A render-target texture: its top level is reported bound under the
// texture's Create id, with its size, SetRenderTarget(0) resets the viewport,
// and it reads back by that id.
VGX_TEST(d3d9_render_targets) {
    Dx9 g;
    auto ctx = attach9(g.dev.Get());
    uint64_t created = 0, bound = 0;
    uint32_t w = 0;
    float vp_width = 0;
    uint32_t px = 0;
    int draws = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        // The back buffer is announced as a render target too, when first met.
        if (e.action == vangfx::ResourceAction::Create && e.kind == vangfx::ResourceKind::RenderTarget && e.rt_desc.width == 16)
            created = e.render_target.id;
    });
    int bindings = 0;
    ctx->on_render_target([&](vangfx::FrameContext& f, vangfx::RenderTargetEvent& e) {
        bound = e.targets[0].id;
        w = e.width;
        // The second binding comes before the second draw clears the target:
        // it still holds the first draw's red.
        if (++bindings == 2) {
            auto r = f.readback(vangfx::RenderTargetHandle{ created });
            if (r && r->pixels.size() >= 4) std::memcpy(&px, r->pixels.data(), 4);
        }
    });
    ctx->on_draw([&](vangfx::FrameContext& f, vangfx::DrawEvent&) {
        vp_width = f.pipeline().viewports[0].width;
        ++draws;
    });
    auto red = g.solid("1,0,0,1");
    ComPtr<IDirect3DTexture9> tex;
    g.dev->CreateTexture(16, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
    ComPtr<IDirect3DSurface9> top;
    tex->GetSurfaceLevel(0, &top);
    g.draw(top.Get(), red.Get());
    g.draw(top.Get(), red.Get());
    CHECK(created != 0);
    CHECK_EQ(bound, created);
    CHECK_EQ(w, 16u);
    CHECK_EQ(vp_width, 16.0f);
    CHECK_EQ(px, kRed9);
}

// Buffer readback by the handle the Create event gave.
VGX_TEST(d3d9_buffer_readback) {
    Dx9 g;
    auto ctx = attach9(g.dev.Get());
    uint64_t id = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create && e.kind == vangfx::ResourceKind::VertexBuffer && !id)
            id = e.vertex_buffer.id;
    });
    uint32_t data[64];
    for (int i = 0; i < 64; ++i) data[i] = 0x2000 + i;
    ComPtr<IDirect3DVertexBuffer9> vb;
    g.dev->CreateVertexBuffer(sizeof(data), 0, 0, D3DPOOL_MANAGED, &vb, nullptr);
    void* p = nullptr;
    vb->Lock(0, 0, &p, 0);
    std::memcpy(p, data, sizeof(data));
    vb->Unlock();
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
    g.draw(rt.Get(), red.Get());
    CHECK(ok);
    CHECK_EQ(got, 0x2005u);
}

// State changes: one render state at a time folded into whole blocks, the
// viewport reset by SetRenderTarget(0), nothing while a state block records,
// and a state block's Apply read back.
VGX_TEST(d3d9_state) {
    Dx9 g;
    auto ctx = attach9(g.dev.Get());
    vangfx::StateChangeEvent last{};
    int events = 0;
    ctx->on_state_change([&](vangfx::FrameContext&, vangfx::StateChangeEvent& e) { last = e; ++events; });

    g.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
    CHECK_EQ(events, 1);
    CHECK(last.dirty & vangfx::StateDirty_DepthStencil);
    CHECK_EQ(last.depth.depth_func, vangfx::CompareFunc::Greater);

    auto rt = g.target(16, 8);
    g.dev->SetRenderTarget(0, rt.Get());
    CHECK(last.dirty & vangfx::StateDirty_Viewports);
    CHECK_EQ(last.viewports[0].width, 16.0f);

    events = 0;
    ComPtr<IDirect3DStateBlock9> sb;
    g.dev->BeginStateBlock();
    g.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    g.dev->EndStateBlock(&sb);
    CHECK_EQ(events, 0);
    g.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    CHECK_EQ(events, 1);
    sb->Apply();
    CHECK(events >= 2);
    CHECK_EQ(last.raster.cull_mode, vangfx::CullMode::Front);
}

// Occlusion and event queries through the query object's own table.
VGX_TEST(d3d9_queries) {
    Dx9 g;
    auto ctx = attach9(g.dev.Get());
    int begins = 0, ends = 0, results = 0;
    uint64_t samples = 0, qid = 0;
    ctx->on_query([&](vangfx::FrameContext&, vangfx::QueryEvent& e) {
        if (e.type != vangfx::QueryType::Occlusion) return;
        qid = e.query_id;
        if (e.action == vangfx::QueryAction::Begin) ++begins;
        if (e.action == vangfx::QueryAction::End) ++ends;
        if (e.action == vangfx::QueryAction::GetData && e.result_ready) { ++results; samples = e.occlusion_samples; }
    });
    auto red = g.solid("1,0,0,1");
    auto rt = g.target(8, 8);
    ComPtr<IDirect3DQuery9> q;
    CHECK(SUCCEEDED(g.dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q)));
    q->Issue(D3DISSUE_BEGIN);
    g.draw(rt.Get(), red.Get());
    q->Issue(D3DISSUE_END);
    DWORD n = 0;
    for (int i = 0; i < 10000 && q->GetData(&n, sizeof(n), D3DGETDATA_FLUSH) == S_FALSE; ++i) Sleep(0);
    CHECK_EQ(begins, 1);
    CHECK_EQ(ends, 1);
    CHECK_EQ(results, 1);
    CHECK_EQ(samples, 64u);
    CHECK(qid != 0);
}

// A post pass runs over the back buffer at Present, and the game's bindings
// and state come through it unchanged.
VGX_TEST(d3d9_post_pass) {
    Dx9 g;
    std::string log;
    auto ctx = attach9(g.dev.Get(), &log);
    auto pass = vangfx::inject_ps_post(*ctx, "color = float4(color.b, color.g, color.r, 1);");
    CHECK_MSG(pass.has_value(), pass ? "" : pass.error().message);
    auto red = g.solid("1,0,0,1");
    auto bb = g.back_buffer();
    g.draw(bb.Get(), red.Get());
    g.dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATER);
    g.dev->Present(nullptr, nullptr, nullptr, nullptr);
    const uint32_t px = g.read_pixel(bb.Get(), 2, 2);
    CHECK_MSG(px == kBlue9, "pixel " + std::to_string(px) + ", log:\n" + log);
    ComPtr<IDirect3DPixelShader9> bound;
    g.dev->GetPixelShader(&bound);
    CHECK(bound.Get() == red.Get());
    DWORD zfunc = 0;
    g.dev->GetRenderState(D3DRS_ZFUNC, &zfunc);
    CHECK_EQ(zfunc, static_cast<DWORD>(D3DCMP_GREATER));
    ComPtr<IDirect3DSurface9> rt0;
    g.dev->GetRenderTarget(0, &rt0);
    CHECK(rt0.Get() == bb.Get());
    vangfx::remove_injection(*ctx, *pass);
}

// Reset succeeds with VanGFX's own default-pool resources alive (a render
// target a callback made, the post pass's copy of the frame), reports device
// lost and reset, and everything keeps working: Reset rewrites the setters in
// the table.
VGX_TEST(d3d9_reset) {
    Dx9 g;
    std::string log;
    auto ctx = attach9(g.dev.Get(), &log);
    int lost = 0, reset = 0, draws = 0, states = 0;
    ctx->on_device_lost([&] { ++lost; });
    ctx->on_device_reset([&](vangfx::FrameContext&) { ++reset; });
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++draws; });
    ctx->on_state_change([&](vangfx::FrameContext&, vangfx::StateChangeEvent&) { ++states; });
    bool made = false;
    ctx->on_present([&](vangfx::FrameContext& f, vangfx::PresentEvent&) {
        if (!made) made = f.create_render_target({ 8, 8, vangfx::SurfaceFormat::A8R8G8B8 }).has_value();
    });
    auto pass = vangfx::inject_ps_post(*ctx, "color = float4(color.b, color.g, color.r, 1);");
    CHECK(pass.has_value());
    auto red = g.solid("1,0,0,1");
    {
        auto bb = g.back_buffer();
        g.draw(bb.Get(), red.Get());
        g.dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    CHECK(made);
    const HRESULT hr = g.dev->Reset(&g.pp);
    CHECK_MSG(SUCCEEDED(hr), "Reset hr " + std::to_string(static_cast<unsigned long>(hr)) + ", log:\n" + log);
    CHECK_EQ(lost, 1);
    CHECK_EQ(reset, 1);

    draws = 0; states = 0;
    auto bb = g.back_buffer();
    for (int f = 0; f < 3; ++f) {
        g.draw(bb.Get(), red.Get());
        g.dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    CHECK_EQ(draws, 3);
    CHECK(states > 0);   // SetRenderState, one of the seventeen Reset rewrites
    CHECK_EQ(g.read_pixel(bb.Get(), 2, 2), kBlue9);
}

// Two devices at once, then one released and a new one made: every device has
// its own table, and a dead device's table is forgotten.
VGX_TEST(d3d9_devices) {
    int a = 0, b = 0;
    {
        Dx9 g1, g2;
        auto c1 = attach9(g1.dev.Get());
        auto c2 = attach9(g2.dev.Get());
        c1->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++a; });
        c2->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++b; });
        auto red1 = g1.solid("1,0,0,1");
        auto red2 = g2.solid("1,0,0,1");
        auto rt1 = g1.target(8, 8);
        auto rt2 = g2.target(8, 8);
        g1.draw(rt1.Get(), red1.Get());
        g2.draw(rt2.Get(), red2.Get());
        g2.draw(rt2.Get(), red2.Get());
        CHECK_EQ(a, 1);
        CHECK_EQ(b, 2);
        c1.reset();
        g1.draw(rt1.Get(), red1.Get());
        CHECK_EQ(a, 1);
        CHECK_EQ(g1.read_pixel(rt1.Get(), 1, 1), kRed9);
    }
    // Both devices and contexts gone; a new device, a new context.
    Dx9 g3;
    auto c3 = attach9(g3.dev.Get());
    int c = 0;
    c3->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++c; });
    auto red3 = g3.solid("1,0,0,1");
    auto rt3 = g3.target(8, 8);
    for (int i = 0; i < 5; ++i) g3.draw(rt3.Get(), red3.Get());
    CHECK_EQ(c, 5);
}

// A device destroyed before its context, after a state block (BeginStateBlock
// rewrites the whole table, the Release hook included): its table must be
// forgotten and its context orphaned, so a device made next -- often at the
// same addresses -- is hooked afresh and reported only to its own context.
VGX_TEST(d3d9_device_dies_first) {
    int stale = 0, fresh = 0;
    std::unique_ptr<vangfx::Context> c1;
    {
        Dx9 g1;
        c1 = attach9(g1.dev.Get());
        c1->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++stale; });
        ComPtr<IDirect3DStateBlock9> sb;
        g1.dev->BeginStateBlock();
        g1.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g1.dev->EndStateBlock(&sb);
    }
    for (int round = 0; round < 4; ++round) {
        Dx9 g;
        auto c = attach9(g.dev.Get());
        int n = 0;
        c->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++n; });
        auto red = g.solid("1,0,0,1");
        auto rt = g.target(8, 8);
        for (int i = 0; i < 3; ++i) g.draw(rt.Get(), red.Get());
        CHECK_EQ(n, 3);
        fresh += n;
        ComPtr<IDirect3DStateBlock9> sb;
        g.dev->BeginStateBlock();
        g.dev->EndStateBlock(&sb);
    }
    CHECK_EQ(fresh, 12);
    CHECK_EQ(stale, 0);
    c1.reset();
}

// A shader created before VanGFX attached is named by its bytecode's hash all
// the same (read back from the shader object).
VGX_TEST(d3d9_shader_made_before_attach) {
    Dx9 g;
    auto blob = compile9("float4 main() : COLOR0 { return float4(1, 0, 0, 1); }", "ps_3_0");
    ComPtr<IDirect3DPixelShader9> red;
    g.dev->CreatePixelShader(static_cast<const DWORD*>(blob->GetBufferPointer()), &red);
    auto ctx = attach9(g.dev.Get());
    uint64_t seen = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent& e) { seen = e.ps_hash; });
    auto rt = g.target(8, 8);
    g.draw(rt.Get(), red.Get());
    CHECK_EQ(seen, vangfx::hash_bytes(blob->GetBufferPointer(), blob->GetBufferSize()));
}

// The Ex device: PresentEx, ResetEx and the Ex creation calls.
VGX_TEST(d3d9_ex) {
    Dx9 g(true);
    auto ctx = attach9(g.dev.Get());
    CHECK_EQ(ctx->d3d9_ver(), 11u);
    int presents = 0, lost = 0, reset = 0, rts = 0, draws = 0;
    ctx->on_present([&](vangfx::FrameContext&, vangfx::PresentEvent&) { ++presents; });
    ctx->on_device_lost([&] { ++lost; });
    ctx->on_device_reset([&](vangfx::FrameContext&) { ++reset; });
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++draws; });
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create && e.kind == vangfx::ResourceKind::RenderTarget) ++rts;
    });
    auto red = g.solid("1,0,0,1");
    auto bb = g.back_buffer();
    g.draw(bb.Get(), red.Get());
    g.dev_ex->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    g.dev->Present(nullptr, nullptr, nullptr, nullptr);
    CHECK_EQ(presents, 2);
    ComPtr<IDirect3DSurface9> rt;
    rts = 0;   // the back buffer was announced when first bound
    CHECK(SUCCEEDED(g.dev_ex->CreateRenderTargetEx(8, 8, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr, 0)));
    CHECK_EQ(rts, 1);
    bb.Reset();
    CHECK(SUCCEEDED(g.dev_ex->ResetEx(&g.pp, nullptr)));
    CHECK_EQ(lost, 1);
    CHECK_EQ(reset, 1);
    draws = 0;
    bb = g.back_buffer();
    g.draw(bb.Get(), red.Get());
    CHECK_EQ(draws, 1);
}
