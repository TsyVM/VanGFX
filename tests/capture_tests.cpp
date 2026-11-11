/**
 * tests/capture_tests.cpp — CaptureSession on each backend, through one small
 * deferred-shading frame:
 *
 *   pass 1  a G-buffer: albedo (red) and normals (blue) as two targets, and a
 *           depth buffer written at 0.5
 *   pass 2  "lighting" into a float target (0.25)
 *   pass 3  the albedo target again, overwritten green
 *   pass 4  the back buffer (blue)
 *
 * Targets scope takes each target as the frame left it (albedo green); Passes
 * scope also takes each at the end of every pass that drew into it (albedo red
 * after pass 1, as a lighting pass would have read it); GBuffer scope takes
 * what the G-buffer view recognises. The files and capture.json are checked,
 * and the frame the game presented is checked to be untouched by the copies.
 *
 * Direct3D 9 reads depth only from an INTZ texture; where the adapter has no
 * INTZ the depth buffer is a plain surface, and is checked to be listed with a
 * note instead.
 */

#include "harness.hpp"
#include <capture/image.hpp>
#include <capture/recorder.hpp>
#include <vangfx/config.hpp>
#include <vangfx/vangfx.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace vgx;
using vangfx::CapturedSurface;
using vangfx::CaptureScope;
using vangfx::FrameCapture;
using vangfx::GBufferLayerKind;
using vangfx::SurfaceFormat;
using vangfx::SurfaceRole;

namespace {

// ── What every backend's frame is checked against ────────────────────────────

// The formats the capture reports each target's pixels in, per backend.
struct Formats {
    SurfaceFormat albedo, normal, light;
};

std::filesystem::path out_dir(const char* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    auto d = std::filesystem::path(tmp) / L"vgx_capture" / name;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);   // this test's own output from an earlier run
    return d;
}

std::array<float, 4> texel(const CapturedSurface& s, uint32_t x = 5, uint32_t y = 7) {
    CHECK_MSG(s.available, "surface not available: " + s.note);
    std::vector<float> row(size_t(s.width) * 4);
    CHECK(vangfx::capture::decode_row(s.format, s.pixels.data() + size_t(y) * s.row_pitch, s.width, row.data()));
    return { row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3] };
}

bool close_to(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

std::string rgba(const std::array<float, 4>& v) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f, %.3f)", v[0], v[1], v[2], v[3]);
    return b;
}

void check_rgb(const CapturedSurface* s, float r, float g, float b, const char* what) {
    CHECK_MSG(s != nullptr, std::string(what) + ": not captured");
    const auto v = texel(*s);
    CHECK_MSG(close_to(v[0], r) && close_to(v[1], g) && close_to(v[2], b), std::string(what) + " reads " + rgba(v));
}

void check_red(const CapturedSurface* s, float r, const char* what) {
    CHECK_MSG(s != nullptr, std::string(what) + ": not captured");
    const auto v = texel(*s);
    CHECK_MSG(close_to(v[0], r, 0.002f), std::string(what) + " reads " + rgba(v));
}

const CapturedSurface* colour(const FrameCapture& f, SurfaceFormat fmt, uint32_t pass) {
    for (const auto& s : f.surfaces)
        if (s.role == SurfaceRole::Color && s.format == fmt && s.pass == pass) return &s;
    return nullptr;
}
const CapturedSurface* depth(const FrameCapture& f, uint32_t pass) {
    for (const auto& s : f.surfaces)
        if (s.role == SurfaceRole::Depth && s.pass == pass) return &s;
    return nullptr;
}
const CapturedSurface* back(const FrameCapture& f, uint32_t pass) {
    for (const auto& s : f.surfaces)
        if (s.role == SurfaceRole::Backbuffer && s.pass == pass) return &s;
    return nullptr;
}

std::string listing(const FrameCapture& f) {
    std::string out;
    char b[200];
    for (const auto& s : f.surfaces) {
        std::snprintf(b, sizeof b, "  id %llu role %d fmt %s %ux%u pass %u draws %u avail %d kind %d note '%s'\n",
                      static_cast<unsigned long long>(s.resource_id), int(s.role), vangfx::capture::format_name(s.format),
                      s.width, s.height, s.pass, s.draws, int(s.available), int(s.kind), s.note.c_str());
        out += b;
    }
    return out;
}

// Runs one frame of `scene` under a capture begun before it, and returns what
// the capture handed its callback.
template <typename Scene>
FrameCapture capture_frame(vangfx::Context& ctx, Scene& scene, vangfx::CaptureConfig cfg) {
    std::optional<FrameCapture> got;
    cfg.on_capture = [&](const FrameCapture& f) { got = f; };
    auto session = vangfx::CaptureSession::begin(ctx, cfg);
    CHECK_MSG(session.has_value(), session ? "" : session.error().message);
    scene.frame();
    session->stop();   // already stopped by itself (max_frames 1); waits for the files
    CHECK_MSG(got.has_value(), "no frame was captured");
    CHECK(!ctx.capture_active());
    return *got;
}

bool file_starts_with(const std::string& path, const std::vector<uint8_t>& magic) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> head(magic.size());
    if (!in.read(head.data(), std::streamsize(head.size()))) return false;
    for (size_t i = 0; i < magic.size(); ++i) if (uint8_t(head[i]) != magic[i]) return false;
    return true;
}

void check_files(const FrameCapture& f, vangfx::CaptureFormat format) {
    CHECK(!f.folder.empty());
    const std::vector<uint8_t> magic = format == vangfx::CaptureFormat::EXR ? std::vector<uint8_t>{ 0x76, 0x2F, 0x31, 0x01 }
                                                                             : std::vector<uint8_t>{ 0x89, 'P', 'N', 'G' };
    int files = 0;
    for (const auto& s : f.surfaces) {
        if (!s.available) continue;
        CHECK_MSG(!s.file.empty(), "an available surface has no file name");
        CHECK_MSG(file_starts_with(s.file, magic), "not written, or not the format asked for: " + s.file);
        ++files;
    }
    CHECK(files >= 4);
    std::ifstream in(f.folder / "capture.json", std::ios::binary);
    CHECK_MSG(in.good(), "no capture.json in " + f.folder.string());
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_MSG(json.find("\"passes\": 4") != std::string::npos, json);
    CHECK_MSG(json.find("\"surfaces\": [") != std::string::npos, json);
    CHECK_MSG(json.find("\"role\": \"Backbuffer\"") != std::string::npos, json);
}

// Targets: every target as the frame left it, and the back buffer once.
void check_targets(const FrameCapture& f, const Formats& k, bool depth_readable) {
    const std::string list = listing(f);
    CHECK_MSG(f.passes == 4, "passes " + std::to_string(f.passes) + "\n" + list);
    CHECK_MSG(f.draws == 4, "draws " + std::to_string(f.draws) + "\n" + list);
    for (const auto& s : f.surfaces) CHECK_MSG(s.pass == 0, "a pass copy in Targets scope\n" + list);
    int backs = 0;
    for (const auto& s : f.surfaces) backs += s.role == SurfaceRole::Backbuffer;
    CHECK_MSG(backs == 1, "the back buffer is listed " + std::to_string(backs) + " times\n" + list);

    const CapturedSurface* albedo = colour(f, k.albedo, 0);
    check_rgb(albedo, 0, 1, 0, "albedo at the end of the frame");
    CHECK_EQ(albedo->draws, 2u);
    check_rgb(colour(f, k.normal, 0), 0, 0, 1, "normals");
    check_red(colour(f, k.light, 0), 0.25f, "lighting");
    check_rgb(back(f, 0), 0, 0, 1, "back buffer");
    const CapturedSurface* d = depth(f, 0);
    CHECK_MSG(d != nullptr, "depth not listed\n" + list);
    if (depth_readable) check_red(d, 0.5f, "depth");
    else CHECK_MSG(!d->available && !d->note.empty(), "an unreadable depth buffer without a note\n" + list);
    CHECK_EQ(f.width, 64u);
    CHECK_EQ(f.height, 64u);
}

// Passes: the targets at the end of each pass too.
void check_passes(const FrameCapture& f, const Formats& k, bool depth_readable) {
    const std::string list = listing(f);
    CHECK_MSG(f.passes == 4, "passes " + std::to_string(f.passes) + "\n" + list);
    check_rgb(colour(f, k.albedo, 1), 1, 0, 0, "albedo after the G-buffer pass");
    check_rgb(colour(f, k.normal, 1), 0, 0, 1, "normals after the G-buffer pass");
    check_red(colour(f, k.light, 2), 0.25f, "lighting after its pass");
    check_rgb(colour(f, k.albedo, 3), 0, 1, 0, "albedo after pass 3");
    check_rgb(back(f, 4), 0, 0, 1, "back buffer after pass 4");
    check_rgb(colour(f, k.albedo, 0), 0, 1, 0, "albedo at the end of the frame");
    if (depth_readable) check_red(depth(f, 1), 0.5f, "depth after the G-buffer pass");
    CHECK_MSG(colour(f, k.albedo, 2) == nullptr, "a copy of a target its pass did not draw into\n" + list);
}

// GBuffer: what the G-buffer view recognises, and the back buffer.
void check_gbuffer(const FrameCapture& f, const Formats& k, bool depth_readable) {
    const std::string list = listing(f);
    const auto& g = f.gbuffer;
    CHECK_MSG(g.has(GBufferLayerKind::Albedo), "no albedo layer\n" + list);
    CHECK_MSG(g.has(GBufferLayerKind::Normal), "no normal layer\n" + list);
    CHECK_MSG(g.has(GBufferLayerKind::Backbuffer), "no back buffer layer\n" + list);
    if (depth_readable) CHECK_MSG(g.has(GBufferLayerKind::DepthRaw), "no depth layer\n" + list);
    CHECK_MSG(colour(f, k.light, 0) == nullptr, "GBuffer scope read a target it does not recognise\n" + list);
    for (uint32_t i = 0; i < g.layer_count; ++i) {
        if (g.layers[i].kind != GBufferLayerKind::Albedo) continue;
        CHECK(g.layers[i].available);
        std::vector<float> row(size_t(g.layers[i].width) * 4);
        CHECK(vangfx::capture::decode_row(g.layers[i].format, g.layers[i].pixels.data(), g.layers[i].width, row.data()));
        CHECK_MSG(close_to(row[1], 1) && close_to(row[0], 0), "albedo layer is not green");
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Direct3D 9
// ─────────────────────────────────────────────────────────────────────────────

#if VANGFX_D3D9
#include "gpu9.hpp"

namespace {

constexpr D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

struct Scene9 {
    Dx9 g{ false, 64, 64 };
    ComPtr<IDirect3DTexture9> albedo, normal, light, depth_tex;
    ComPtr<IDirect3DSurface9> albedo_s, normal_s, light_s, depth_s, bb;
    ComPtr<IDirect3DPixelShader9> mrt, lit, green, blue;
    bool intz = false;

    Scene9() {
        auto* d = g.dev.Get();
        d->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &albedo, nullptr);
        d->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A2B10G10R10, D3DPOOL_DEFAULT, &normal, nullptr);
        d->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &light, nullptr);
        if (!albedo || !normal || !light) fail(__FILE__, __LINE__, "render target textures refused");
        albedo->GetSurfaceLevel(0, &albedo_s);
        normal->GetSurfaceLevel(0, &normal_s);
        light->GetSurfaceLevel(0, &light_s);
        intz = SUCCEEDED(g.d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                                  D3DRTYPE_TEXTURE, kINTZ)) &&
               SUCCEEDED(d->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, kINTZ, D3DPOOL_DEFAULT, &depth_tex, nullptr));
        if (intz) depth_tex->GetSurfaceLevel(0, &depth_s);
        else d->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth_s, nullptr);
        auto b = compile9("struct O { float4 a : COLOR0; float4 n : COLOR1; };"
                          "O main() { O o; o.a = float4(1, 0, 0, 1); o.n = float4(0, 0, 1, 1); return o; }", "ps_3_0");
        d->CreatePixelShader(static_cast<const DWORD*>(b->GetBufferPointer()), &mrt);
        lit   = g.solid("0.25, 1, 0, 1");
        green = g.solid("0, 1, 0, 1");
        blue  = g.solid("0, 0, 1, 1");
        bb    = g.back_buffer();
    }

    void quad(float z) {
        struct V { float x, y, z, w; } v[3] = { { -1, -1, z, 1 }, { -1, 3, z, 1 }, { 3, -1, z, 1 } };
        g.dev->SetVertexShader(g.vs3.Get());
        g.dev->SetVertexDeclaration(g.decl.Get());
        g.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, v, sizeof(V));
    }

    void frame() {
        auto* d = g.dev.Get();
        d->BeginScene();
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_LIGHTING, FALSE);
        // 1: the G-buffer, one slot per call as D3D9 binds them.
        d->SetRenderTarget(0, albedo_s.Get());
        d->SetRenderTarget(1, normal_s.Get());
        d->SetDepthStencilSurface(depth_s.Get());
        d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
        d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d->SetPixelShader(mrt.Get());
        quad(0.5f);
        // 2: lighting.
        d->SetRenderTarget(0, light_s.Get());
        d->SetRenderTarget(1, nullptr);
        d->SetDepthStencilSurface(nullptr);
        d->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        d->SetPixelShader(lit.Get());
        quad(0);
        // 3: the albedo target again.
        d->SetRenderTarget(0, albedo_s.Get());
        d->SetPixelShader(green.Get());
        quad(0);
        // 4: the back buffer.
        d->SetRenderTarget(0, bb.Get());
        d->SetPixelShader(blue.Get());
        quad(0);
        d->EndScene();
        d->Present(nullptr, nullptr, nullptr, nullptr);
    }
};

const Formats kFormats9{ SurfaceFormat::A8R8G8B8, SurfaceFormat::R10G10B10A2, SurfaceFormat::R32F };

std::unique_ptr<vangfx::Context> attach_capture9(IDirect3DDevice9* dev, std::string& log) {
    auto ctx = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D9)
        .log_level(vangfx::LogLevel::Info)
        .log_callback([&log](vangfx::LogLevel, std::string_view m) { log.append(m); log.push_back('\n'); })
        .d3d9_device(dev)
        .build();
    CHECK_MSG(ctx.has_value(), ctx ? "" : ctx.error().message);
    return std::move(*ctx);
}

} // namespace

VGX_TEST(capture_d3d9_targets) {
    Scene9 s;
    std::string log;
    auto ctx = attach_capture9(s.g.dev.Get(), log);
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.output_dir = out_dir("d3d9_targets");
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    note("D3D9 INTZ: %s", s.intz ? "yes" : "no (depth is a plain surface)");
    check_targets(f, kFormats9, s.intz);
    check_files(f, vangfx::CaptureFormat::PNG);
    CHECK_EQ(s.g.read_pixel(s.bb.Get(), 3, 3), kBlue9);   // what the game presented, untouched
    auto latest = vangfx::latest_frame_capture(*ctx);
    CHECK(latest.has_value() && latest->frame_index == f.frame_index);
}

VGX_TEST(capture_d3d9_passes) {
    Scene9 s;
    std::string log;
    auto ctx = attach_capture9(s.g.dev.Get(), log);
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.scope         = CaptureScope::Passes;
    cfg.output_dir    = out_dir("d3d9_passes");
    cfg.output_format = vangfx::CaptureFormat::EXR;
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_passes(f, kFormats9, s.intz);
    check_files(f, vangfx::CaptureFormat::EXR);
    // The depth copy draws on the game's device mid-frame: nothing it changed shows.
    s.frame();
    CHECK_EQ(s.g.read_pixel(s.bb.Get(), 3, 3), kBlue9);
    CHECK_EQ(s.g.read_pixel(s.albedo_s.Get(), 3, 3), kGreen9);
    ComPtr<IDirect3DPixelShader9> bound;
    s.g.dev->GetPixelShader(&bound);
    CHECK(bound.Get() == s.blue.Get());
}

VGX_TEST(capture_d3d9_gbuffer) {
    Scene9 s;
    std::string log;
    auto ctx = attach_capture9(s.g.dev.Get(), log);
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.scope = CaptureScope::GBuffer;
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_gbuffer(f, kFormats9, s.intz);
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Direct3D 11
// ─────────────────────────────────────────────────────────────────────────────

#if VANGFX_D3D11
#include "gpu11.hpp"

namespace {

constexpr const char* kVSHalfDepth =
    "float4 main(uint id : SV_VertexID) : SV_Position {"
    " float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2,-2) + float2(-1,1), 0.5, 1); }";

struct Scene11 {
    Dx11 g;
    HWND hwnd = hidden_window11(64, 64);
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11Texture2D> albedo, normal, light, depth_tex, bb;
    ComPtr<ID3D11RenderTargetView> albedo_v, normal_v, light_v, bb_v;
    ComPtr<ID3D11DepthStencilView> depth_v;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> mrt, lit, green, blue;

    ComPtr<ID3D11Texture2D> texture(DXGI_FORMAT f, UINT bind) {
        D3D11_TEXTURE2D_DESC d{ 64, 64, 1, 1, f, { 1, 0 }, D3D11_USAGE_DEFAULT, bind, 0, 0 };
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &t))) fail(__FILE__, __LINE__, "texture refused");
        return t;
    }

    Scene11() {
        sc = g.swap_chain(hwnd, 64, 64);
        sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        albedo    = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        normal    = texture(DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        light     = texture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        depth_tex = texture(DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL);
        g.dev->CreateRenderTargetView(albedo.Get(), nullptr, &albedo_v);
        g.dev->CreateRenderTargetView(normal.Get(), nullptr, &normal_v);
        g.dev->CreateRenderTargetView(light.Get(), nullptr, &light_v);
        g.dev->CreateRenderTargetView(bb.Get(), nullptr, &bb_v);
        g.dev->CreateDepthStencilView(depth_tex.Get(), nullptr, &depth_v);
        auto vb = compile11(kVSHalfDepth, "vs_5_0");
        g.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
        auto mb = compile11("struct O { float4 a : SV_Target0; float4 n : SV_Target1; };"
                            "O main() { O o; o.a = float4(1, 0, 0, 1); o.n = float4(0, 0, 1, 1); return o; }", "ps_5_0");
        g.dev->CreatePixelShader(mb->GetBufferPointer(), mb->GetBufferSize(), nullptr, &mrt);
        lit   = g.solid("0.25, 1, 0, 1");
        green = g.solid("0, 1, 0, 1");
        blue  = g.solid("0, 0, 1, 1");
    }
    ~Scene11() {
        sc.Reset();
        DestroyWindow(hwnd);
    }

    void pass(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, ID3D11PixelShader* ps) {
        auto* c = g.dc.Get();
        c->OMSetRenderTargets(n, rtvs, dsv);
        const D3D11_VIEWPORT vp{ 0, 0, 64, 64, 0, 1 };
        c->RSSetViewports(1, &vp);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->Draw(3, 0);
    }

    void frame() {
        auto* c = g.dc.Get();
        const float black[4] = { 0, 0, 0, 1 };
        c->ClearRenderTargetView(albedo_v.Get(), black);
        c->ClearRenderTargetView(normal_v.Get(), black);
        c->ClearDepthStencilView(depth_v.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ID3D11RenderTargetView* gbuf[2] = { albedo_v.Get(), normal_v.Get() };
        pass(2, gbuf, depth_v.Get(), mrt.Get());
        ID3D11RenderTargetView* l = light_v.Get();
        pass(1, &l, nullptr, lit.Get());
        ID3D11RenderTargetView* a = albedo_v.Get();
        pass(1, &a, nullptr, green.Get());
        ID3D11RenderTargetView* b = bb_v.Get();
        pass(1, &b, nullptr, blue.Get());
        sc->Present(0, 0);
    }
};

const Formats kFormats11{ SurfaceFormat::RGBA8, SurfaceFormat::R10G10B10A2, SurfaceFormat::R32F };

std::unique_ptr<vangfx::Context> attach_capture11(Scene11& s) {
    auto ctx = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D11)
        .d3d11_device(s.g.dev.Get())
        .d3d11_swap_chain(s.sc.Get())
        .build();
    CHECK_MSG(ctx.has_value(), ctx ? "" : ctx.error().message);
    return std::move(*ctx);
}

} // namespace

VGX_TEST(capture_d3d11_targets) {
    Scene11 s;
    auto ctx = attach_capture11(s);
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.output_dir = out_dir("d3d11_targets");
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_targets(f, kFormats11, true);
    check_files(f, vangfx::CaptureFormat::PNG);
    CHECK_EQ(s.g.read_pixel(s.bb.Get(), 3, 3), kBlue);
}

VGX_TEST(capture_d3d11_passes) {
    Scene11 s;
    auto ctx = attach_capture11(s);
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.scope         = CaptureScope::Passes;
    cfg.output_dir    = out_dir("d3d11_passes");
    cfg.output_format = vangfx::CaptureFormat::EXR;
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_passes(f, kFormats11, true);
    check_files(f, vangfx::CaptureFormat::EXR);
    CHECK_EQ(s.g.read_pixel(s.albedo.Get(), 3, 3), kGreen);
}

VGX_TEST(capture_d3d11_gbuffer) {
    Scene11 s;
    auto ctx = attach_capture11(s);
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.scope = CaptureScope::GBuffer;
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_gbuffer(f, kFormats11, true);
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Direct3D 12
// ─────────────────────────────────────────────────────────────────────────────

#if VANGFX_D3D12
#include "gpu12.hpp"

namespace {

struct Scene12c {
    Dx12 g;
    HWND hwnd = hidden_window(64, 64);
    ComPtr<IDXGISwapChain3> sc;
    ComPtr<ID3D12Resource> albedo, normal, light, depth_tex, bbs[2];
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE albedo_v{}, normal_v{}, light_v{}, bb_v[2]{}, depth_v{};
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> mrt, lit, green, blue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;

    void make_swap_chain() {
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(g.factory->CreateSwapChainForHwnd(g.queue.Get(), hwnd, &d, nullptr, nullptr, &sc1)))
            fail(__FILE__, __LINE__, "swap chain refused");
        sc1.As(&sc);
    }

    // Everything else, made once the context is attached (so VanGFX sees the
    // views created, and each resource's initial state).
    void make_resources() {
        D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
        hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
        const UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto rtv = [&](ID3D12Resource* r, UINT slot) {
            auto h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += slot * inc;
            g.dev->CreateRenderTargetView(r, nullptr, h);
            return h;
        };
        albedo = g.render_target(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
        normal = g.render_target(64, 64, DXGI_FORMAT_R10G10B10A2_UNORM);
        light  = g.render_target(64, 64, DXGI_FORMAT_R32_FLOAT);
        albedo_v = rtv(albedo.Get(), 0);
        normal_v = rtv(normal.Get(), 1);
        light_v  = rtv(light.Get(), 2);
        for (UINT i = 0; i < 2; ++i) {
            sc->GetBuffer(i, IID_PPV_ARGS(&bbs[i]));
            bb_v[i] = rtv(bbs[i].Get(), 3 + i);
        }
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC dd{};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = 64; dd.Height = 64; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.SampleDesc.Count = 1;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1.0f;
        if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                  IID_PPV_ARGS(&depth_tex))))
            fail(__FILE__, __LINE__, "depth buffer refused");
        depth_v = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        g.dev->CreateDepthStencilView(depth_tex.Get(), nullptr, depth_v);

        rs = g.empty_root_signature();
        mrt   = pipeline("struct O { float4 a : SV_Target0; float4 n : SV_Target1; };"
                         "O main() { O o; o.a = float4(1, 0, 0, 1); o.n = float4(0, 0, 1, 1); return o; }",
                         { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM }, DXGI_FORMAT_D24_UNORM_S8_UINT);
        lit   = pipeline("float4 main() : SV_Target { return float4(0.25, 1, 0, 1); }", { DXGI_FORMAT_R32_FLOAT });
        green = pipeline("float4 main() : SV_Target { return float4(0, 1, 0, 1); }", { DXGI_FORMAT_R8G8B8A8_UNORM });
        blue  = pipeline("float4 main() : SV_Target { return float4(0, 0, 1, 1); }", { DXGI_FORMAT_R8G8B8A8_UNORM });
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
    }

    ComPtr<ID3D12PipelineState> pipeline(const char* ps_src, std::initializer_list<DXGI_FORMAT> rts,
                                         DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN) {
        auto vs = compile("float4 main(uint id : SV_VertexID) : SV_Position {"
                          " float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2,-2) + float2(-1,1), 0.5, 1); }",
                          "vs_5_0");
        auto ps = compile(ps_src, "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rs.Get();
        d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = static_cast<UINT>(rts.size());
        UINT i = 0;
        for (DXGI_FORMAT f : rts) {
            d.RTVFormats[i] = f;
            d.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            ++i;
        }
        if (dsv != DXGI_FORMAT_UNKNOWN) {
            d.DSVFormat = dsv;
            d.DepthStencilState.DepthEnable    = TRUE;
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            d.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
        }
        d.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(g.dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) fail(__FILE__, __LINE__, "test pipeline refused");
        return pso;
    }

    ~Scene12c() {
        g.wait();
        sc.Reset();
        DestroyWindow(hwnd);
    }

    void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
        list->ResourceBarrier(1, &b);
    }

    void draw(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv, ID3D12PipelineState* pso) {
        list->OMSetRenderTargets(n, rtvs, FALSE, dsv);
        list->SetPipelineState(pso);
        const D3D12_VIEWPORT vp{ 0, 0, 64, 64, 0, 1 };
        const D3D12_RECT sr{ 0, 0, 64, 64 };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sr);
        list->SetGraphicsRootSignature(rs.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    }

    UINT presented = 0;   // the back buffer the last frame drew into

    void frame() {
        const UINT i = sc->GetCurrentBackBufferIndex();
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        const float black[4] = { 0, 0, 0, 1 };
        list->ClearRenderTargetView(albedo_v, black, 0, nullptr);
        list->ClearRenderTargetView(normal_v, black, 0, nullptr);
        list->ClearDepthStencilView(depth_v, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        // 1: the G-buffer, then made readable for the lighting pass.
        const D3D12_CPU_DESCRIPTOR_HANDLE gbuf[2] = { albedo_v, normal_v };
        draw(2, gbuf, &depth_v, mrt.Get());
        transition(albedo.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(normal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // 2: lighting.
        draw(1, &light_v, nullptr, lit.Get());
        transition(albedo.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(normal.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // 3: the albedo target again.
        draw(1, &albedo_v, nullptr, green.Get());
        // 4: the back buffer.
        transition(bbs[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        draw(1, &bb_v[i], nullptr, blue.Get());
        transition(bbs[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        g.queue->ExecuteCommandLists(1, lists);
        sc->Present(0, 0);
        g.wait();
        presented = i;
    }
};

const Formats kFormats12{ SurfaceFormat::RGBA8, SurfaceFormat::R10G10B10A2, SurfaceFormat::R32F };

std::unique_ptr<vangfx::Context> attach_capture12(Scene12c& s, std::string& log) {
    auto ctx = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D12)
        .log_level(vangfx::LogLevel::Info)
        .log_callback([&log](vangfx::LogLevel, std::string_view m) { log.append(m); log.push_back('\n'); })
        .d3d12_device(s.g.dev.Get())
        .d3d12_command_queue(s.g.queue.Get())
        .d3d12_swap_chain(s.sc.Get())
        .build();
    CHECK_MSG(ctx.has_value(), ctx ? "" : ctx.error().message);
    return std::move(*ctx);
}

} // namespace

VGX_TEST(capture_d3d12_targets) {
    Scene12c s;
    s.make_swap_chain();
    std::string log;
    auto ctx = attach_capture12(s, log);
    s.make_resources();
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.output_dir = out_dir("d3d12_targets");
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_targets(f, kFormats12, true);
    check_files(f, vangfx::CaptureFormat::PNG);
    CHECK_EQ(s.g.read_pixel(s.bbs[s.presented].Get(), D3D12_RESOURCE_STATE_PRESENT, 3, 3), kBlue);
}

VGX_TEST(capture_d3d12_passes) {
    Scene12c s;
    s.make_swap_chain();
    std::string log;
    auto ctx = attach_capture12(s, log);
    s.make_resources();
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.scope         = CaptureScope::Passes;
    cfg.output_dir    = out_dir("d3d12_passes");
    cfg.output_format = vangfx::CaptureFormat::EXR;
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_passes(f, kFormats12, true);
    check_files(f, vangfx::CaptureFormat::EXR);
    // The copies went into the game's own list between its barriers: the
    // states it expects are the states it gets on the next frame.
    s.frame();
    CHECK_EQ(s.g.read_pixel(s.bbs[s.presented].Get(), D3D12_RESOURCE_STATE_PRESENT, 3, 3), kBlue);
    CHECK_EQ(s.g.read_pixel(s.albedo.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 3, 3), kGreen);
}

VGX_TEST(capture_d3d12_gbuffer) {
    Scene12c s;
    s.make_swap_chain();
    std::string log;
    auto ctx = attach_capture12(s, log);
    s.make_resources();
    s.frame();
    vangfx::CaptureConfig cfg;
    cfg.scope = CaptureScope::GBuffer;
    const FrameCapture f = capture_frame(*ctx, s, cfg);
    check_gbuffer(f, kFormats12, true);
}
#endif
