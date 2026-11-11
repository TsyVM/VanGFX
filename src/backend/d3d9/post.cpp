/**
 * src/backend/d3d9/post.cpp — post-processing passes at Present.
 *
 * The passes draw on the game's device, so everything they change is put back
 * afterwards: a D3DSBT_ALL state block captures the game's state before the
 * passes and is applied after them, and the render targets and depth-stencil
 * surface -- which no state block holds -- are saved and set back by hand,
 * before the Apply (setting target 0 resets the viewport, which the Apply then
 * restores). The state block is made for each frame and released: kept, it
 * would hold the game's bound objects alive from one frame to the next.
 *
 * Each pass copies the back buffer into a render-target texture (StretchRect,
 * which also resolves a multisampled one) and draws a full-screen triangle into
 * the back buffer reading it: vs_3_0 and ps_3_0, since the snippet is wrapped
 * for ps_3_0 (effect/inject.cpp) and a ps_3_0 shader runs only behind a
 * vs_3_0 one. Constants c0-c7 are the vangfx_float4 slots and c8 is (time,
 * delta, width, height).
 */

#include "d3d9_impl.hpp"
#include <core/shader_compile.hpp>

#include <cstring>
#include <unordered_map>

namespace vangfx::backend::d3d9 {

namespace {

constexpr const char* kFullscreenVS = R"(
float4 main(float4 p : POSITION) : POSITION { return p; }
)";

template <typename T> void release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }

struct Vertex { float x, y, z, w; };
// One triangle covering the screen: clip space, so no half-pixel offset is
// involved (the pixel shader addresses the scene by VPOS).
constexpr Vertex kTriangle[3] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };

} // namespace

class PostProcessor {
public:
    explicit PostProcessor(D3D9ContextImpl& ctx) noexcept : ctx_(ctx) {}
    ~PostProcessor() { release_targets(); release_all(); }

    void run(IDirect3DSwapChain9* swap_chain, const ContextImpl::PostPassList& passes, const D3D9ContextImpl::Timing& t);
    void release_targets() noexcept { release(scene_surface_); release(scene_); scene_desc_ = {}; }

private:
    bool init();
    void release_all() noexcept;
    void fail_once(const std::string& why) {
        if (!failed_) ctx_.log(LogLevel::Error, "D3D9 post-processing: " + why);
        failed_ = true;
    }
    bool ensure_scene(const D3DSURFACE_DESC& bd);
    void draw_pass(IDirect3DDevice9* dev, IDirect3DSurface9* bb, IDirect3DPixelShader9* ps, const float* constants);

    D3D9ContextImpl&              ctx_;
    bool                          ready_ = false, failed_ = false;
    IDirect3DVertexShader9*       vs_    = nullptr;
    IDirect3DVertexDeclaration9*  decl_  = nullptr;
    IDirect3DTexture9*            scene_ = nullptr;
    IDirect3DSurface9*            scene_surface_ = nullptr;
    D3DSURFACE_DESC               scene_desc_{};
    std::unordered_map<uint64_t, IDirect3DPixelShader9*> ps_;
};

void destroy_post_processor(PostProcessor* p) noexcept { delete p; }

bool PostProcessor::init() {
    if (ready_) return true;
    if (failed_) return false;
    IDirect3DDevice9* dev = ctx_.device();
    auto vs = detail::compile_hlsl(ShaderBuilder{}.source(kFullscreenVS).entry("main").profile("vs_3_0"), "vs_3_0");
    if (!vs) { fail_once("could not compile the full-screen vertex shader: " + vs.error().message); return false; }
    if (FAILED(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(vs->data()), &vs_))) {
        fail_once("the device refused a vs_3_0 shader; post passes need shader model 3.");
        return false;
    }
    const D3DVERTEXELEMENT9 elements[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        D3DDECL_END()
    };
    if (FAILED(dev->CreateVertexDeclaration(elements, &decl_))) { fail_once("could not create the vertex declaration."); return false; }
    ready_ = true;
    return true;
}

bool PostProcessor::ensure_scene(const D3DSURFACE_DESC& bd) {
    if (scene_ && scene_desc_.Width == bd.Width && scene_desc_.Height == bd.Height && scene_desc_.Format == bd.Format)
        return true;
    release_targets();
    IDirect3DDevice9* dev = ctx_.device();
    if (FAILED(dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &scene_, nullptr)) ||
        FAILED(scene_->GetSurfaceLevel(0, &scene_surface_))) {
        release_targets();
        fail_once("could not create the texture the frame is copied into (back buffer format " +
                  std::to_string(static_cast<unsigned>(bd.Format)) + ").");
        return false;
    }
    scene_desc_ = bd;
    return true;
}

void PostProcessor::draw_pass(IDirect3DDevice9* dev, IDirect3DSurface9* bb, IDirect3DPixelShader9* ps, const float* constants) {
    // Outside the scene: StretchRect is not allowed inside one on every driver.
    if (FAILED(dev->StretchRect(bb, nullptr, scene_surface_, nullptr, D3DTEXF_NONE))) {
        fail_once("StretchRect from the back buffer failed.");
        return;
    }
    const bool began = SUCCEEDED(dev->BeginScene());
    dev->SetRenderTarget(0, bb);
    for (DWORD i = 1; i < 4; ++i) dev->SetRenderTarget(i, nullptr);
    dev->SetDepthStencilSurface(nullptr);

    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetStreamSourceFreq(0, 1);
    dev->SetTexture(0, scene_);
    dev->SetVertexDeclaration(decl_);
    dev->SetVertexShader(vs_);
    dev->SetPixelShader(ps);
    dev->SetPixelShaderConstantF(0, constants, 9);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kTriangle, sizeof(Vertex));
    if (began) dev->EndScene();
}

void PostProcessor::run(IDirect3DSwapChain9* swap_chain, const ContextImpl::PostPassList& passes, const D3D9ContextImpl::Timing& t) {
    if (passes.empty()) return;
    InternalScope internal_work;
    if (!init()) return;
    IDirect3DDevice9* dev = ctx_.device();

    // Shaders of passes since removed.
    for (auto it = ps_.begin(); it != ps_.end();) {
        bool live = false;
        for (const auto& p : passes) live |= p.id == it->first;
        if (live) { ++it; continue; }
        if (it->second) it->second->Release();
        it = ps_.erase(it);
    }

    IDirect3DSurface9* bb = nullptr;
    const HRESULT got = swap_chain ? swap_chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                                   : dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(got) || !bb) { fail_once("could not get the back buffer."); return; }
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    if (!ensure_scene(bd)) { bb->Release(); return; }

    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) {
        bb->Release();
        fail_once("could not capture the device state (CreateStateBlock); post passes are skipped rather than "
                  "leave the game's state changed.");
        return;
    }
    IDirect3DSurface9* targets[4]{};
    for (DWORD i = 0; i < 4; ++i)
        if (FAILED(dev->GetRenderTarget(i, &targets[i]))) targets[i] = nullptr;
    IDirect3DSurface9* depth = nullptr;
    if (FAILED(dev->GetDepthStencilSurface(&depth))) depth = nullptr;

    float constants[9][4]{};
    const auto slots = ctx_.uniforms();
    for (int i = 0; i < 8; ++i) std::memcpy(constants[i], slots[i].data(), 16);
    constants[8][0] = t.time;
    constants[8][1] = t.delta;
    constants[8][2] = static_cast<float>(bd.Width);
    constants[8][3] = static_cast<float>(bd.Height);

    for (const auto& pass : passes) {
        IDirect3DPixelShader9*& ps = ps_[pass.id];
        if (!ps && FAILED(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(pass.ps.data()), &ps))) {
            ctx_.log(LogLevel::Error, "D3D9 post-processing: a pass's shader was refused; that pass is skipped.");
            ps = nullptr;
            continue;
        }
        if (ps) draw_pass(dev, bb, ps, &constants[0][0]);
    }

    dev->SetTexture(0, nullptr);
    for (DWORD i = 0; i < 4; ++i) {
        if (i == 0 && !targets[0]) continue;   // target 0 cannot be unbound
        dev->SetRenderTarget(i, targets[i]);
    }
    dev->SetDepthStencilSurface(depth);
    saved->Apply();
    saved->Release();
    for (IDirect3DSurface9* s : targets) if (s) s->Release();
    if (depth) depth->Release();
    bb->Release();
}

void PostProcessor::release_all() noexcept {
    for (auto& [id, p] : ps_) if (p) p->Release();
    ps_.clear();
    release(decl_);
    release(vs_);
}

void run_post_passes(D3D9ContextImpl& ctx, PostProcessor*& processor, IDirect3DSwapChain9* swap_chain,
                     const D3D9ContextImpl::Timing& t) {
    const auto passes = ctx.post_passes();
    if (!passes || passes->empty()) return;
    if (!processor) processor = new PostProcessor(ctx);
    processor->run(swap_chain, *passes, t);
}

void release_post_targets(PostProcessor* p) noexcept {
    if (p) p->release_targets();
}

}  // namespace vangfx::backend::d3d9
