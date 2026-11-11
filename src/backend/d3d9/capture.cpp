/**
 * src/backend/d3d9/capture.cpp — what a capture needs from D3D9: the hooks'
 * reports to the Recorder, and copies of surfaces.
 *
 * A colour target is copied with StretchRect into a render target of its own
 * where the pass ends -- the copy is queued like a draw, and a multisampled
 * target is resolved on the way -- and read at Present with GetRenderTargetData
 * into a system-memory surface. A target StretchRect will not copy is read
 * there and then instead (GetRenderTargetData waits for the GPU).
 *
 * DEPTH
 *
 * Direct3D 9 has no way to read a depth-stencil surface: StretchRect,
 * GetRenderTargetData and LockRect all refuse one. A depth buffer made as a
 * texture of a format shaders can sample (INTZ, DF24, DF16) is drawn into an
 * R32F target by a full-screen triangle, with the game's state saved in a
 * state block and put back as the post passes do, and read like a colour copy.
 * A plain depth-stencil surface (CreateDepthStencilSurface, or the automatic
 * one) is left out with a note.
 *
 * RESET
 *
 * A plain device's Reset fails while anything in the default pool is alive, so
 * every copy still waiting is let go before one (drop_snapshots); reading it
 * afterwards says so.
 */

#include "d3d9_impl.hpp"
#include <capture/recorder.hpp>
#include <core/shader_compile.hpp>
#include <format_map.hpp>

#include <cstring>
#include <mutex>
#include <unordered_set>

namespace vangfx::backend::d3d9 {

namespace {

template <typename T>
void release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }

constexpr D3DFORMAT kINTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr D3DFORMAT kDF24 = static_cast<D3DFORMAT>(MAKEFOURCC('D', 'F', '2', '4'));
constexpr D3DFORMAT kDF16 = static_cast<D3DFORMAT>(MAKEFOURCC('D', 'F', '1', '6'));

constexpr const char* kDepthVS = R"(
float4 main(float4 p : POSITION) : POSITION { return p; }
)";

// The depth in the red channel of each texel, one texel per pixel.
constexpr const char* kDepthPS = R"(
sampler2D depth_tex : register(s0);
float4 inv_size : register(c0);
float4 main(float2 vpos : VPOS) : COLOR {
    return tex2Dlod(depth_tex, float4((vpos + 0.5) * inv_size.xy, 0, 0)).rrrr;
}
)";

struct Vertex { float x, y, z, w; };
constexpr Vertex kTriangle[3] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };

// The surface a resource is rendered through: itself, or a texture's top level.
IDirect3DSurface9* top_surface(IDirect3DResource9* res) noexcept {
    if (!res) return nullptr;
    switch (res->GetType()) {
    case D3DRTYPE_SURFACE: {
        auto* s = static_cast<IDirect3DSurface9*>(res);
        s->AddRef();
        return s;
    }
    case D3DRTYPE_TEXTURE: {
        IDirect3DSurface9* s = nullptr;
        if (FAILED(static_cast<IDirect3DTexture9*>(res)->GetSurfaceLevel(0, &s))) return nullptr;
        return s;
    }
    default:
        return nullptr;
    }
}

} // namespace

class Snapshots;

class Copy final : public capture::Snapshot {
public:
    Copy(Snapshots& host, IDirect3DDevice9* device) noexcept;
    ~Copy() override;

    Result<capture::Pixels> read() override;

    // A reset is coming: the copy's surfaces go now.
    void drop() noexcept { release(gpu); release(sys); dropped = true; }

    Snapshots&         host;
    IDirect3DDevice9*  device;              // weak: the surfaces hold it
    IDirect3DSurface9* gpu = nullptr;       // a render target in the default pool, read at Present
    IDirect3DSurface9* sys = nullptr;       // or a system-memory surface already filled
    SurfaceFormat      format = SurfaceFormat::Unknown;
    uint32_t           width  = 0;
    uint32_t           height = 0;
    bool               dropped = false;
};

class Snapshots final : public capture::SnapshotHost {
public:
    explicit Snapshots(D3D9ContextImpl& ctx) : ctx_(ctx) {}
    ~Snapshots() override {
        release(depth_ps_);
        release(depth_vs_);
        release(decl_);
    }

    bool can_snapshot_in(const void*, std::string&) noexcept override { return true; }

    std::unique_ptr<capture::Snapshot> snapshot(const void*, const capture::TargetInfo& t, std::string& why) override {
        IDirect3DResource9* res = resource_from_id(t.id);
        IDirect3DSurface9* surface = top_surface(res);
        if (!surface) { why = "the resource is gone, or is not a surface or 2D texture"; return nullptr; }
        auto c = t.role == SurfaceRole::Depth ? copy_depth(res, surface, why) : copy_colour(surface, why);
        surface->Release();
        return c;
    }

    std::unique_ptr<capture::Snapshot> snapshot_backbuffer(FrameContext& fc, capture::TargetInfo& info, std::string& why,
                                                           bool copy) override {
        IDirect3DSwapChain9* chain = static_cast<D3D9FrameContextImpl&>(fc).swap_chain();
        IDirect3DDevice9* device = ctx_.device();
        IDirect3DSurface9* bb = nullptr;
        const HRESULT hr = chain ? chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                                 : device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        if (FAILED(hr) || !bb) { why = "the swap chain would not give its back buffer"; return nullptr; }
        D3DSURFACE_DESC d{};
        bb->GetDesc(&d);
        info.id     = surface_id(bb, device);
        info.role   = SurfaceRole::Backbuffer;
        info.format = fmtmap::from_d3d9(d.Format);
        info.width  = d.Width;
        info.height = d.Height;
        auto c = copy ? copy_colour(bb, why) : nullptr;
        bb->Release();
        return c;
    }

    void flush() override {}   // GetRenderTargetData waits

    void enlist(Copy* c) { std::lock_guard lk(mutex_); live_.insert(c); }
    void leave(Copy* c) noexcept { std::lock_guard lk(mutex_); live_.erase(c); }
    void drop_all() noexcept {
        std::lock_guard lk(mutex_);
        for (Copy* c : live_) c->drop();
    }

private:
    std::unique_ptr<capture::Snapshot> copy_colour(IDirect3DSurface9* surface, std::string& why) {
        D3DSURFACE_DESC d{};
        surface->GetDesc(&d);
        const SurfaceFormat fmt = fmtmap::from_d3d9(d.Format);
        if (!fmtmap::row_pitch(fmt, d.Width)) {
            why = "a format with no defined pixel size (D3DFORMAT " + std::to_string(static_cast<unsigned>(d.Format)) + ")";
            return nullptr;
        }
        InternalScope internal_work;
        IDirect3DDevice9* device = ctx_.device();
        auto c = std::make_unique<Copy>(*this, device);
        c->format = fmt;
        c->width  = d.Width;
        c->height = d.Height;
        if (SUCCEEDED(device->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &c->gpu, nullptr)) &&
            c->gpu && SUCCEEDED(device->StretchRect(surface, nullptr, c->gpu, nullptr, D3DTEXF_NONE)))
            return c;
        release(c->gpu);
        // Read at once: GetRenderTargetData takes a single-sampled target in the
        // default pool to a system-memory surface of its size and format.
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
            why = "StretchRect would not resolve the multisampled target";
            return nullptr;
        }
        if (FAILED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &c->sys, nullptr)) ||
            !c->sys || FAILED(device->GetRenderTargetData(surface, c->sys))) {
            why = "neither StretchRect nor GetRenderTargetData would copy it";
            return nullptr;
        }
        return c;
    }

    std::unique_ptr<capture::Snapshot> copy_depth(IDirect3DResource9* res, IDirect3DSurface9* surface, std::string& why) {
        D3DSURFACE_DESC d{};
        surface->GetDesc(&d);
        if (res->GetType() != D3DRTYPE_TEXTURE || (d.Format != kINTZ && d.Format != kDF24 && d.Format != kDF16)) {
            why = "Direct3D 9 cannot read a depth-stencil surface; a depth buffer made as an INTZ, DF24 or DF16 "
                  "texture is read by drawing it";
            return nullptr;
        }
        IDirect3DDevice9* device = ctx_.device();
        {
            DevState* st = dev_state(device);   // the hooks calling here hold a HubReader
            if (st && st->recording) { why = "the device was recording a state block"; return nullptr; }
        }
        InternalScope internal_work;
        if (!depth_pipeline(device, why)) return nullptr;

        auto c = std::make_unique<Copy>(*this, device);
        c->format = SurfaceFormat::D32F;
        c->width  = d.Width;
        c->height = d.Height;
        if (FAILED(device->CreateRenderTarget(d.Width, d.Height, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &c->gpu, nullptr)) || !c->gpu) {
            why = "could not create the R32F target the depth is drawn into";
            return nullptr;
        }

        IDirect3DStateBlock9* saved = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) {
            why = "could not save the device state (CreateStateBlock)";
            return nullptr;
        }
        IDirect3DSurface9* targets[4]{};
        for (DWORD i = 0; i < 4; ++i)
            if (FAILED(device->GetRenderTarget(i, &targets[i]))) targets[i] = nullptr;
        IDirect3DSurface9* ds = nullptr;
        if (FAILED(device->GetDepthStencilSurface(&ds))) ds = nullptr;

        const bool began = SUCCEEDED(device->BeginScene());
        device->SetRenderTarget(0, c->gpu);
        for (DWORD i = 1; i < 4; ++i) device->SetRenderTarget(i, nullptr);
        device->SetDepthStencilSurface(nullptr);   // it may be the texture being read
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
        device->SetStreamSourceFreq(0, 1);
        device->SetTexture(0, static_cast<IDirect3DTexture9*>(res));
        device->SetVertexDeclaration(decl_);
        device->SetVertexShader(depth_vs_);
        device->SetPixelShader(depth_ps_);
        const float inv[4] = { 1.0f / static_cast<float>(d.Width), 1.0f / static_cast<float>(d.Height), 0, 0 };
        device->SetPixelShaderConstantF(0, inv, 1);
        const HRESULT drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kTriangle, sizeof(Vertex));
        if (began) device->EndScene();

        device->SetTexture(0, nullptr);
        for (DWORD i = 0; i < 4; ++i) {
            if (i == 0 && !targets[0]) continue;   // target 0 cannot be unbound
            device->SetRenderTarget(i, targets[i]);
        }
        device->SetDepthStencilSurface(ds);
        saved->Apply();
        saved->Release();
        for (IDirect3DSurface9* s : targets) if (s) s->Release();
        if (ds) ds->Release();

        if (FAILED(drawn)) { why = "the draw reading the depth texture failed"; return nullptr; }
        return c;
    }

    bool depth_pipeline(IDirect3DDevice9* device, std::string& why) {
        if (depth_ps_) return true;
        if (depth_failed_) { why = depth_why_; return false; }
        auto fail = [&](std::string w) { depth_failed_ = true; depth_why_ = std::move(w); why = depth_why_; return false; };
        auto vs = detail::compile_hlsl(ShaderBuilder{}.source(kDepthVS).entry("main").profile("vs_3_0"), "vs_3_0");
        auto ps = detail::compile_hlsl(ShaderBuilder{}.source(kDepthPS).entry("main").profile("ps_3_0"), "ps_3_0");
        if (!vs || !ps) return fail("could not compile the shaders that read a depth texture");
        const D3DVERTEXELEMENT9 elements[] = {
            { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            D3DDECL_END()
        };
        if (FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD*>(vs->data()), &depth_vs_)) ||
            FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ps->data()), &depth_ps_)) ||
            FAILED(device->CreateVertexDeclaration(elements, &decl_))) {
            release(depth_vs_);
            release(depth_ps_);
            release(decl_);
            return fail("the device refused the shader model 3 shaders that read a depth texture");
        }
        return true;
    }

    D3D9ContextImpl& ctx_;

    std::mutex                   mutex_;
    std::unordered_set<Copy*>    live_;

    IDirect3DVertexShader9*      depth_vs_ = nullptr;
    IDirect3DPixelShader9*       depth_ps_ = nullptr;
    IDirect3DVertexDeclaration9* decl_     = nullptr;
    bool                         depth_failed_ = false;
    std::string                  depth_why_;
};

Copy::Copy(Snapshots& h, IDirect3DDevice9* d) noexcept : host(h), device(d) { host.enlist(this); }

Copy::~Copy() {
    host.leave(this);
    release(gpu);
    release(sys);
}

Result<capture::Pixels> Copy::read() {
    if (dropped) return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed, "the device was reset before the copy was read");
    const uint32_t pitch = fmtmap::row_pitch(format, width);
    const uint32_t rows  = fmtmap::row_count(format, height);
    if (!pitch || !rows) return gfx_err<capture::Pixels>(ShaderError::InvalidFormat, "the format has no pixel size");
    InternalScope internal_work;
    if (gpu) {
        D3DSURFACE_DESC d{};
        gpu->GetDesc(&d);
        if (FAILED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
            return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed, "could not create the system-memory surface to read into");
        if (FAILED(device->GetRenderTargetData(gpu, sys)))
            return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed, "GetRenderTargetData failed on the copy");
        release(gpu);
    }
    D3DLOCKED_RECT locked{};
    if (!sys || FAILED(sys->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
        return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed, "LockRect failed on the copy");
    capture::Pixels px;
    px.format    = format;
    px.width     = width;
    px.height    = height;
    px.row_pitch = pitch;
    px.bytes.resize(static_cast<size_t>(pitch) * rows);
    const auto* src = static_cast<const uint8_t*>(locked.pBits);
    const size_t n = pitch < static_cast<uint32_t>(locked.Pitch) ? pitch : static_cast<uint32_t>(locked.Pitch);
    for (uint32_t y = 0; y < rows; ++y)
        std::memcpy(px.bytes.data() + static_cast<size_t>(y) * pitch, src + static_cast<size_t>(y) * locked.Pitch, n);
    sys->UnlockRect();
    return px;
}

Snapshots* make_snapshots(D3D9ContextImpl& ctx) noexcept {
    try { return new Snapshots(ctx); } catch (...) { return nullptr; }
}
void destroy_snapshots(Snapshots* s) noexcept { delete s; }
void drop_snapshots(Snapshots* s) noexcept { if (s) s->drop_all(); }

capture::SnapshotHost* D3D9ContextImpl::snapshot_host() noexcept { return snapshots_; }

// ── The hooks' side ──────────────────────────────────────────────────────────

namespace {

capture::TargetInfo target_info(IDirect3DSurface9* surface, SurfaceRole role, IDirect3DDevice9* device) noexcept {
    capture::TargetInfo t;
    if (!surface) return t;
    t.id   = surface_id(surface, device);
    t.role = role;
    D3DSURFACE_DESC d{};
    if (SUCCEEDED(surface->GetDesc(&d))) {
        t.format = fmtmap::from_d3d9(d.Format);
        t.width  = d.Width;
        t.height = d.Height;
    }
    return t;
}

// What the device has bound now: slot i of up to four, and the depth-stencil.
capture::TargetSet target_set(IDirect3DDevice9* device) noexcept {
    capture::TargetSet set;
    for (DWORD i = 0; i < 4; ++i) {
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(device->GetRenderTarget(i, &rt)) || !rt) continue;   // D3DERR_NOTFOUND when unbound
        set.colour[i] = target_info(rt, SurfaceRole::Color, device);
        set.count = i + 1;
        rt->Release();
    }
    IDirect3DSurface9* ds = nullptr;
    if (SUCCEEDED(device->GetDepthStencilSurface(&ds)) && ds) {
        set.depth = target_info(ds, SurfaceRole::Depth, device);
        ds->Release();
    }
    return set;
}

bool capturing(const DevState& st) noexcept {
    for (D3D9ContextImpl* ctx : st.contexts) if (ctx->capture_active()) return true;
    return false;
}

} // namespace

void capture_bind(IDirect3DDevice9* device, DevState& st) {
    if (!capturing(st)) return;
    const capture::TargetSet set = target_set(device);
    for (D3D9ContextImpl* ctx : st.contexts)
        if (auto rec = ctx->recorder()) rec->bind(device, set);
}

void capture_draw(IDirect3DDevice9* device, DevState& st) {
    for (D3D9ContextImpl* ctx : st.contexts) ctx->note_draw();
    if (!capturing(st)) return;
    for (D3D9ContextImpl* ctx : st.contexts) {
        auto rec = ctx->recorder();
        if (!rec) continue;
        if (!rec->knows(device)) rec->bind(device, target_set(device));
        rec->draw(device);
    }
}

}  // namespace vangfx::backend::d3d9
