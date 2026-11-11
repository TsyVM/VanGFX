/**
 * tests/d3d12_tests.cpp — the D3D12 backend, driven through a small "game"
 * (gpu12.hpp) on WARP.
 *
 * Each of the audit's measured failures has a test here: no draw events ever
 * (draws), the stack overflow on the second buffer (buffers), the installer
 * that unhooked itself on a second Reset (many_resets), and replace_ps doing
 * nothing (replace_ps).
 */

#include "gpu12.hpp"

#pragma warning(disable : 4324)   // the stream test pads its subobjects on purpose
#include <vangfx/vangfx.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

using namespace vgx;

namespace {

std::unique_ptr<vangfx::Context> attach(Dx12& g, IDXGISwapChain* sc = nullptr, std::string* log = nullptr) {
    auto b = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D12)
        .log_level(vangfx::LogLevel::Info)
        .d3d12_device(g.dev.Get())
        .d3d12_command_queue(g.queue.Get())
        .d3d12_swap_chain(sc);
    if (log) b.log_callback([log](vangfx::LogLevel, std::string_view m) { log->append(m); log->push_back('\n'); });
    auto ctx = b.build();
    CHECK_MSG(ctx.has_value(), ctx ? "" : ctx.error().message);
    return std::move(*ctx);
}

} // namespace

// Draw, list and pipeline events, attached before the game makes anything.
VGX_TEST(d3d12_draws) {
    Dx12 g;
    auto ctx = attach(g);
    int draws = 0, resets = 0, closes = 0, submits = 0, psos = 0;
    uint64_t pso_id = 0, drawn_pso = 0, vs_hash = 0, ps_hash = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent& e) {
        ++draws; drawn_pso = e.pso.id; vs_hash = e.vs_hash; ps_hash = e.ps_hash;
    });
    ctx->on_pso_create([&](vangfx::FrameContext&, vangfx::d3d12::PSOCreateEvent& e) { ++psos; pso_id = e.pso.id; });
    ctx->on_command_list([&](vangfx::FrameContext&, vangfx::d3d12::CommandListEvent& e) {
        using A = vangfx::d3d12::CommandListAction;
        if (e.action == A::Reset) ++resets;
        if (e.action == A::Close) ++closes;
        if (e.action == A::Submit) ++submits;
    });

    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(8, 8);
    Scene12 a(g), b(g);
    const auto va = a.view(rt.Get()), vb = b.view(rt.Get());
    resets = closes = submits = 0;   // a new list is created open and closed at once: count frames only
    for (int f = 0; f < 3; ++f) {
        a.draw(va, 8, 8, rs.Get(), pso.Get());
        b.draw(vb, 8, 8, rs.Get(), pso.Get());
    }
    CHECK_EQ(draws, 6);
    CHECK_EQ(resets, 6);
    CHECK_EQ(closes, 6);
    CHECK_EQ(submits, 6);
    CHECK_EQ(psos, 1);
    CHECK(pso_id != 0);
    CHECK_EQ(drawn_pso, pso_id);
    CHECK(vs_hash != 0 && ps_hash != 0 && vs_hash != ps_hash);
}

// Lists and pipelines made before VanGFX attached still report draws.
VGX_TEST(d3d12_late_attach) {
    Dx12 g;
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(8, 8);
    Scene12 s(g);
    const auto v = s.view(rt.Get());
    s.draw(v, 8, 8, rs.Get(), pso.Get());

    auto ctx = attach(g);
    int draws = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++draws; });
    for (int f = 0; f < 4; ++f) s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(draws, 4);
}

// The audit's crash: with a context attached, the second buffer, and releasing
// a buffer made before attaching, overflowed the stack.
VGX_TEST(d3d12_buffers) {
    Dx12 g;
    auto before = g.buffer(256);
    auto ctx = attach(g);
    int created = 0, destroyed = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create) ++created;
        if (e.action == vangfx::ResourceAction::Destroy) ++destroyed;
    });
    {
        auto b1 = g.buffer(256);
        auto b2 = g.buffer(1024);
        auto b3 = g.buffer(64, D3D12_HEAP_TYPE_UPLOAD);
        before.Reset();
    }
    CHECK_EQ(created, 3);
    CHECK_EQ(destroyed, 3);   // the three made after attaching; `before` was never reported
    for (int i = 0; i < 200; ++i) (void)g.buffer(256);
    CHECK_EQ(created, 203);
    CHECK_EQ(destroyed, 203);
}

// Every Reset used to re-run the per-list installer, which saved its own hook
// as the original: SetPipelineState then recursed forever and DrawInstanced
// went silent. Many frames, many lists.
VGX_TEST(d3d12_many_resets) {
    Dx12 g;
    auto ctx = attach(g);
    int draws = 0;
    ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++draws; });
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(8, 8);
    std::vector<std::unique_ptr<Scene12>> scenes;
    for (int i = 0; i < 4; ++i) scenes.push_back(std::make_unique<Scene12>(g));
    for (int f = 0; f < 25; ++f)
        for (auto& s : scenes) {
            const auto v = s->view(rt.Get());
            s->draw(v, 8, 8, rs.Get(), pso.Get());   // Reset, SetPipelineState, draw, Close: every frame
        }
    CHECK_EQ(draws, 100);
}

// The render-target event names the target by the id its Create event gave it.
VGX_TEST(d3d12_render_targets) {
    Dx12 g;
    auto ctx = attach(g);
    uint64_t created_id = 0, bound_id = 0;
    uint32_t w = 0, h = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create && e.kind == vangfx::ResourceKind::RenderTarget)
            created_id = e.render_target.id;
    });
    ctx->on_render_target([&](vangfx::FrameContext&, vangfx::RenderTargetEvent& e) {
        bound_id = e.targets[0].id; w = e.width; h = e.height;
    });
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(16, 8);
    Scene12 s(g);
    s.draw(s.view(rt.Get()), 16, 8, rs.Get(), pso.Get());
    CHECK(created_id != 0);
    CHECK_EQ(bound_id, created_id);
    CHECK_EQ(w, 16u);
    CHECK_EQ(h, 8u);
}

// replace_ps changes the draws its filter matches, and only those.
VGX_TEST(d3d12_replace_ps) {
    Dx12 g;
    std::string log;
    auto ctx = attach(g, nullptr, &log);
    vangfx::ReplaceFilter only_quads;
    only_quads.min_verts = 4;
    auto green = vangfx::replace_ps(*ctx, "float4 main() : SV_Target { return float4(0, 1, 0, 1); }", only_quads);
    CHECK_MSG(green.has_value(), green ? "" : green.error().message);

    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(8, 8);
    Scene12 s(g);
    const auto v = s.view(rt.Get());

    s.draw(v, 8, 8, rs.Get(), pso.Get(), 3);      // 3 vertices: not matched
    CHECK_EQ(g.read_pixel(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 1, 1), kRed);
    s.draw(v, 8, 8, rs.Get(), pso.Get(), 6);      // 6 vertices: matched
    CHECK_EQ(g.read_pixel(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 1, 1), kGreen);
    s.draw(v, 8, 8, rs.Get(), pso.Get(), 3);      // and the next draw is the game's again
    CHECK_EQ(g.read_pixel(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 1, 1), kRed);

    vangfx::remove_replacement(*ctx, *green);
    s.draw(v, 8, 8, rs.Get(), pso.Get(), 6);
    CHECK_EQ(g.read_pixel(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 1, 1), kRed);
}

// A pipeline created before the first replacement cannot be rebuilt: its draws
// keep their shaders, and the log says why, once.
VGX_TEST(d3d12_replace_ps_unrecorded) {
    Dx12 g;
    std::string log;
    auto ctx = attach(g, nullptr, &log);
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto green = vangfx::replace_ps(*ctx, "float4 main() : SV_Target { return float4(0, 1, 0, 1); }");
    CHECK(green.has_value());
    auto rt = g.render_target(8, 8);
    Scene12 s(g);
    const auto v = s.view(rt.Get());
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(g.read_pixel(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET), kRed);
    const size_t first = log.find("pipeline recording");
    CHECK(first != std::string::npos);
    CHECK(log.find("pipeline recording", first + 1) == std::string::npos);
}

// A compile error comes back from replace_ps with the compiler's message.
VGX_TEST(d3d12_replace_ps_error) {
    Dx12 g;
    auto ctx = attach(g);
    auto bad = vangfx::replace_ps(*ctx, "float4 main() : SV_Target { return undeclared_thing; }");
    CHECK(!bad.has_value());
    CHECK(bad.error().code == vangfx::ShaderError::CompileFailed);
    CHECK(bad.error().message.find("undeclared_thing") != std::string::npos);
}

// Callbacks can be removed, including from inside themselves, and are not
// called after remove_callback returns.
VGX_TEST(d3d12_remove_callback) {
    Dx12 g;
    auto ctx = attach(g);
    int a = 0, b = 0;
    vangfx::CallbackId self{};
    auto id_a = ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++a; });
    self = ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++b; ctx->remove_callback(self); });
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(8, 8);
    Scene12 s(g);
    const auto v = s.view(rt.Get());
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(a, 2);
    CHECK_EQ(b, 1);
    ctx->remove_callback(id_a);
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(a, 2);
    {
        vangfx::CallbackGuard guard(*ctx, ctx->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++a; }));
        s.draw(v, 8, 8, rs.Get(), pso.Get());
    }
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(a, 3);
}

// Two contexts on one device both hear; destroying one leaves the other
// working; with both gone the game keeps rendering.
VGX_TEST(d3d12_two_contexts) {
    Dx12 g;
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(8, 8);
    Scene12 s(g);
    const auto v = s.view(rt.Get());
    int a = 0, b = 0;
    auto ca = attach(g);
    auto cb = attach(g);
    ca->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++a; });
    cb->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++b; });
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(a, 1); CHECK_EQ(b, 1);
    ca.reset();
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(a, 1); CHECK_EQ(b, 2);
    cb.reset();
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    auto more = g.buffer(128);
    more.Reset();
    CHECK_EQ(g.read_pixel(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET), kRed);
    auto again = attach(g);   // and a new context after all that starts clean
    int c = 0;
    again->on_draw([&](vangfx::FrameContext&, vangfx::DrawEvent&) { ++c; });
    s.draw(v, 8, 8, rs.Get(), pso.Get());
    CHECK_EQ(c, 1);
}

// Draws recorded on several threads at once.
VGX_TEST(d3d12_threads) {
    Dx12 g;
    auto ctx = attach(g);
    std::atomic<int> draws{ 0 };
    ctx->on_draw([&](vangfx::FrameContext& f, vangfx::DrawEvent&) {
        ++draws;
        CHECK(f.native_context() != nullptr);
    });
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            ComPtr<ID3D12CommandAllocator> a;
            ComPtr<ID3D12GraphicsCommandList> l;
            g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
            g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), pso.Get(), IID_PPV_ARGS(&l));
            for (int i = 0; i < 250; ++i) l->DrawInstanced(3, 1, 0, 0);
            l->Close();
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(draws.load(), 1000);
}

// A post pass runs over the back buffer at Present.
VGX_TEST(d3d12_post_pass) {
    Dx12 g;
    HWND hwnd = hidden_window(8, 8);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = 8; d.Height = 8; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(SUCCEEDED(g.factory->CreateSwapChainForHwnd(g.queue.Get(), hwnd, &d, nullptr, nullptr, &sc1)));
    ComPtr<IDXGISwapChain3> sc;
    sc1.As(&sc);

    std::string log;
    auto ctx = attach(g, nullptr, &log);   // no swap chain given: adopted from the first Present
    int presents = 0;
    ctx->on_present([&](vangfx::FrameContext&, vangfx::PresentEvent&) { ++presents; });
    auto pass = vangfx::inject_ps_post(*ctx, "color = float4(color.b, color.g, color.r, 1);");   // swap red and blue
    CHECK_MSG(pass.has_value(), pass ? "" : pass.error().message);

    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    Scene12 s(g);
    for (int f = 0; f < 2; ++f) {
        const UINT i = sc->GetCurrentBackBufferIndex();
        ComPtr<ID3D12Resource> bb;
        sc->GetBuffer(i, IID_PPV_ARGS(&bb));
        const auto v = s.view(bb.Get());
        s.alloc->Reset();
        s.list->Reset(s.alloc.Get(), pso.Get());
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { bb.Get(), 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        s.list->ResourceBarrier(1, &b);
        s.list->OMSetRenderTargets(1, &v, FALSE, nullptr);
        const D3D12_VIEWPORT vp{ 0, 0, 8, 8, 0, 1 };
        const D3D12_RECT sr{ 0, 0, 8, 8 };
        s.list->RSSetViewports(1, &vp);
        s.list->RSSetScissorRects(1, &sr);
        s.list->SetGraphicsRootSignature(rs.Get());
        s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        s.list->DrawInstanced(3, 1, 0, 0);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        s.list->ResourceBarrier(1, &b);
        s.list->Close();
        g.execute(s.list.Get());
        sc->Present(0, 0);
        g.wait();
        const uint32_t px = g.read_pixel(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, 2, 2);
        CHECK_MSG(px == kBlue, "pixel " + std::to_string(px) + ", presents " + std::to_string(presents) + ", log:\n" + log);
    }
    CHECK_EQ(presents, 2);
    vangfx::remove_injection(*ctx, *pass);
    ctx.reset();
    sc.Reset(); sc1.Reset();
    DestroyWindow(hwnd);
}

// Pipeline state streams: the 4-byte subobjects (sample mask, formats, flags)
// are where the old parser lost its place.
VGX_TEST(d3d12_pipeline_stream) {
    Dx12 g;
    ComPtr<ID3D12Device2> dev2;
    CHECK(SUCCEEDED(g.dev.As(&dev2)));
    auto ctx = attach(g);
    vangfx::d3d12::PSOKind kind{};
    uint32_t rts = 0, samples = 0, rt_format = 0, ds_format = 0;
    bool has_vs = false, has_ps = false;
    ctx->on_pso_create([&](vangfx::FrameContext&, vangfx::d3d12::PSOCreateEvent& e) {
        kind = e.kind;
        if (!e.graphics) return;
        rts = e.graphics->num_render_targets;
        samples = e.graphics->sample_count;
        rt_format = static_cast<uint32_t>(e.graphics->rtv_formats[0]);
        ds_format = static_cast<uint32_t>(e.graphics->dsv_format);
        has_vs = !e.graphics->vs.empty();
        has_ps = !e.graphics->ps.empty();
    });

    auto rs = g.empty_root_signature();
    auto vs = compile("float4 main(uint id : SV_VertexID) : SV_Position { return 0; }", "vs_5_0");
    auto ps = compile("float4 main() : SV_Target { return 1; }", "ps_5_0");
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* v; } s_rs;
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK; UINT v = UINT_MAX; } s_mask;
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS; D3D12_SHADER_BYTECODE v; } s_vs;
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY; D3D12_PRIMITIVE_TOPOLOGY_TYPE v = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; } s_topo;
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS; D3D12_SHADER_BYTECODE v; } s_ps;
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT; DXGI_FORMAT v = DXGI_FORMAT_D32_FLOAT; } s_dsf;
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; D3D12_RT_FORMAT_ARRAY v{}; } s_rtf;
    struct alignas(void*) { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC; DXGI_SAMPLE_DESC v{ 1, 0 }; } s_sd;
    s_rs.v = rs.Get();
    s_vs.v = { vs->GetBufferPointer(), vs->GetBufferSize() };
    s_ps.v = { ps->GetBufferPointer(), ps->GetBufferSize() };
    s_rtf.v.NumRenderTargets = 1;
    s_rtf.v.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;

    std::vector<uint8_t> stream;
    auto push = [&](const auto& so) {
        const auto* p = reinterpret_cast<const uint8_t*>(&so);
        stream.insert(stream.end(), p, p + sizeof(so));
    };
    push(s_rs); push(s_mask); push(s_vs); push(s_topo); push(s_ps); push(s_dsf); push(s_rtf); push(s_sd);
    D3D12_PIPELINE_STATE_STREAM_DESC sd{ stream.size(), stream.data() };
    ComPtr<ID3D12PipelineState> pso;
    CHECK(SUCCEEDED(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso))));
    CHECK(kind == vangfx::d3d12::PSOKind::Graphics);
    CHECK(has_vs && has_ps);
    CHECK_EQ(rts, 1u);
    CHECK_EQ(samples, 1u);
    CHECK(rt_format == static_cast<uint32_t>(vangfx::SurfaceFormat::R16G16B16A16_Float));
    CHECK(ds_format == static_cast<uint32_t>(vangfx::SurfaceFormat::D32_Float));
}

// Buffer readback by handle and by the address a draw's view carries.
VGX_TEST(d3d12_buffer_readback) {
    Dx12 g;
    auto ctx = attach(g);
    uint64_t id = 0, va = 0;
    ctx->on_resource([&](vangfx::FrameContext&, vangfx::ResourceEvent& e) {
        if (e.action == vangfx::ResourceAction::Create && e.kind == vangfx::ResourceKind::ConstantBuffer && !id) {
            id = e.constant_buffer.id;
            va = e.d3d12_desc.gpu_virtual_address;
        }
    });
    auto up = g.buffer(256, D3D12_HEAP_TYPE_UPLOAD);
    uint32_t* p = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (int i = 0; i < 64; ++i) p[i] = 0x1000 + i;
    up->Unmap(0, nullptr);
    CHECK(id != 0 && va != 0);

    vangfx::Result<vangfx::BufferData> by_handle = std::unexpected(vangfx::VanGFXError{});
    vangfx::Result<vangfx::BufferData> by_address = std::unexpected(vangfx::VanGFXError{});
    uint32_t first = 0, tenth = 0;
    // Readback is a FrameContext call: make one from a draw.
    ctx->on_draw([&](vangfx::FrameContext& f, vangfx::DrawEvent&) {
        by_handle = f.readback(vangfx::ConstantBufferHandle{ id }, 0, 16);
        if (by_handle) std::memcpy(&first, by_handle->bytes.data(), 4);
        by_address = f.readback_at(va + 40, 4);
        if (by_address) std::memcpy(&tenth, by_address->bytes.data(), 4);
    });
    auto rs = g.empty_root_signature();
    auto pso = g.solid_pipeline(rs.Get(), "1,0,0,1");
    auto rt = g.render_target(8, 8);
    Scene12 s(g);
    s.draw(s.view(rt.Get()), 8, 8, rs.Get(), pso.Get());
    CHECK(by_handle.has_value());
    CHECK(by_address.has_value());
    CHECK_EQ(first, 0x1000u);
    CHECK_EQ(tenth, 0x100Au);
}
