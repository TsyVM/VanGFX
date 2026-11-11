/**
 * src/backend/d3d11/capture.cpp — what a capture needs from D3D11: the hooks'
 * reports to the Recorder, and copies of surfaces.
 *
 * A copy is a CopySubresourceRegion of subresource 0 into a staging texture on
 * the immediate context, made where the pass ends (the immediate context is
 * the only one whose commands run when they are made), and read with Map at
 * Present. A multisampled target is resolved into a default-usage texture on
 * the way: a staging texture cannot be a resolve destination. Passes drawn on
 * a deferred context are not copied -- nothing says when its command list
 * runs -- but what they drew is in the end-of-frame copies.
 */

#include "d3d11_impl.hpp"
#include <capture/recorder.hpp>
#include <format_map.hpp>

#include <cstring>

namespace vangfx::backend::d3d11 {

namespace {

template <typename T>
void release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }

DXGI_FORMAT typed(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
    default:                                return f;
    }
}

// The typeless family a depth format belongs to, for a staging texture the
// driver will not make in the depth format itself.
DXGI_FORMAT typeless_depth(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_TYPELESS;
    default:                               return f;
    }
}

// What a copy's bytes are: the view's format when there was one, a depth
// buffer's by its depth layout.
SurfaceFormat copied_format(DXGI_FORMAT resource, SurfaceFormat view, SurfaceRole role) noexcept {
    if (role == SurfaceRole::Depth) {
        switch (resource) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:       return SurfaceFormat::D24S8;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: return SurfaceFormat::D32FS8;
        case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:                 return SurfaceFormat::D32F;
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:                 return SurfaceFormat::D16;
        default: break;
        }
    }
    return view != SurfaceFormat::Unknown ? view : fmtmap::from_dxgi(resource);
}

class Copy final : public capture::Snapshot {
public:
    ~Copy() override { release(staging); }

    Result<capture::Pixels> read() override {
        const uint32_t pitch = fmtmap::row_pitch(format, width);
        const uint32_t rows  = fmtmap::row_count(format, height);
        if (!pitch || !rows) return gfx_err<capture::Pixels>(ShaderError::InvalidFormat, "the format has no pixel size");
        InternalScope internal_work;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = dc->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) {
            heal_context(dc);
            return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed, "Map failed on the staging copy");
        }
        capture::Pixels px;
        px.format    = format;
        px.width     = width;
        px.height    = height;
        px.row_pitch = pitch;
        px.bytes.resize(static_cast<size_t>(pitch) * rows);
        const auto* src = static_cast<const uint8_t*>(m.pData);
        const size_t n = pitch < m.RowPitch ? pitch : m.RowPitch;
        for (uint32_t y = 0; y < rows; ++y)
            std::memcpy(px.bytes.data() + static_cast<size_t>(y) * pitch, src + static_cast<size_t>(y) * m.RowPitch, n);
        dc->Unmap(staging, 0);
        heal_context(dc);   // Map is one of the calls after which the runtime swaps its entries
        return px;
    }

    ID3D11DeviceContext* dc      = nullptr;   // the immediate context (weak)
    ID3D11Texture2D*     staging = nullptr;
    SurfaceFormat        format  = SurfaceFormat::Unknown;
    uint32_t             width   = 0;
    uint32_t             height  = 0;
};

} // namespace

class Snapshots final : public capture::SnapshotHost {
public:
    explicit Snapshots(D3D11ContextImpl& ctx) : ctx_(ctx) {}

    bool can_snapshot_in(const void* scope, std::string& why) noexcept override {
        if (!scope || scope == ctx_.immediate()) return true;
        why = "drawn on a deferred context: its passes are not copied (what they drew is, at the end of the frame)";
        return false;
    }

    std::unique_ptr<capture::Snapshot> snapshot(const void*, const capture::TargetInfo& t, std::string& why) override {
        ID3D11Resource* res = resource_from_id(t.id);
        if (!res) { why = "the resource is gone"; return nullptr; }
        return copy(res, t, why);
    }

    std::unique_ptr<capture::Snapshot> snapshot_backbuffer(FrameContext&, capture::TargetInfo& info, std::string& why,
                                                           bool copy_it) override {
        IDXGISwapChain* sc = ctx_.swap_chain();
        if (!sc) { why = "no swap chain"; return nullptr; }
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) { why = "the swap chain would not give its back buffer"; return nullptr; }
        D3D11_TEXTURE2D_DESC d{};
        bb->GetDesc(&d);
        info.id = resource_id(bb);
        if (!info.id) { announce_resource(ctx_.device(), bb); info.id = resource_id(bb); }
        info.role   = SurfaceRole::Backbuffer;
        info.format = fmtmap::from_dxgi(d.Format);
        info.width  = d.Width;
        info.height = d.Height;
        auto c = copy_it ? copy(bb, info, why) : nullptr;
        bb->Release();
        return c;
    }

    void flush() override {}   // Map waits

private:
    std::unique_ptr<capture::Snapshot> copy(ID3D11Resource* res, const capture::TargetInfo& t, std::string& why) {
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))) || !tex) { why = "not a 2D texture"; return nullptr; }
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        tex->Release();
        const bool msaa = d.SampleDesc.Count > 1;
        if (msaa && t.role == SurfaceRole::Depth) { why = "a multisampled depth buffer cannot be resolved to copy"; return nullptr; }

        InternalScope internal_work;
        ID3D11Device* device = ctx_.device();
        ID3D11DeviceContext* dc = ctx_.immediate();
        auto c = std::make_unique<Copy>();
        c->dc     = dc;
        c->width  = d.Width;
        c->height = d.Height;
        c->format = copied_format(d.Format, t.format, t.role);

        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc = { 1, 0 };
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;

        if (msaa) {
            DXGI_FORMAT rf = t.format != SurfaceFormat::Unknown ? typed(fmtmap::to_dxgi(t.format)) : DXGI_FORMAT_UNKNOWN;
            if (rf == DXGI_FORMAT_UNKNOWN) rf = typed(d.Format);
            D3D11_TEXTURE2D_DESC rd = sd;
            rd.Format = rf;
            rd.Usage = D3D11_USAGE_DEFAULT;
            rd.CPUAccessFlags = 0;
            ID3D11Texture2D* resolved = nullptr;
            if (FAILED(device->CreateTexture2D(&rd, nullptr, &resolved)) || !resolved) {
                why = "could not create the texture to resolve the multisampled target into";
                return nullptr;
            }
            sd.Format = rf;
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &c->staging)) || !c->staging) {
                resolved->Release();
                why = "could not create a staging texture";
                return nullptr;
            }
            dc->ResolveSubresource(resolved, 0, res, 0, rf);
            dc->CopyResource(c->staging, resolved);
            resolved->Release();
            c->format = copied_format(rf, SurfaceFormat::Unknown, t.role);
        } else {
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &c->staging)) || !c->staging) {
                sd.Format = typeless_depth(d.Format);
                if (sd.Format == d.Format || FAILED(device->CreateTexture2D(&sd, nullptr, &c->staging)) || !c->staging) {
                    why = "could not create a staging texture";
                    return nullptr;
                }
            }
            dc->CopySubresourceRegion(c->staging, 0, 0, 0, 0, res, 0, nullptr);
        }
        return c;
    }

    D3D11ContextImpl& ctx_;
};

Snapshots* make_snapshots(D3D11ContextImpl& ctx) noexcept {
    try { return new Snapshots(ctx); } catch (...) { return nullptr; }
}
void destroy_snapshots(Snapshots* s) noexcept { delete s; }

capture::SnapshotHost* D3D11ContextImpl::snapshot_host() noexcept { return snapshots_; }

// ── The hooks' side ──────────────────────────────────────────────────────────

namespace {

capture::TargetInfo target_info(ID3D11View* view, SurfaceRole role, ID3D11Device* device) noexcept {
    capture::TargetInfo t;
    if (!view) return t;
    t.id   = view_resource_id(view, device);
    t.role = role;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (res) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            t.width  = d.Width;
            t.height = d.Height;
            t.format = fmtmap::from_dxgi(d.Format);
            tex->Release();
        }
        res->Release();
    }
    // The format the view reads it as, when it names one.
    DXGI_FORMAT vf = DXGI_FORMAT_UNKNOWN;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    if (role == SurfaceRole::Depth && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&dsv))) && dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        dsv->GetDesc(&vd);
        vf = vd.Format;
        dsv->Release();
    } else if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&rtv))) && rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        rtv->GetDesc(&vd);
        vf = vd.Format;
        rtv->Release();
    }
    if (vf != DXGI_FORMAT_UNKNOWN) t.format = fmtmap::from_dxgi(vf);
    return t;
}

capture::TargetSet target_set(const DcState& st) noexcept {
    capture::TargetSet set;
    set.count = st.rtv_count < 8 ? st.rtv_count : 8;
    for (uint32_t i = 0; i < set.count; ++i) set.colour[i] = target_info(st.rtv[i], SurfaceRole::Color, st.device);
    set.depth = target_info(st.dsv, SurfaceRole::Depth, st.device);
    return set;
}

bool capturing(const DcState& st) noexcept {
    for (D3D11ContextImpl* ctx : st.contexts) if (ctx->capture_active()) return true;
    return false;
}

} // namespace

void capture_bind(ID3D11DeviceContext* dc, DcState& st) {
    if (!capturing(st)) return;
    const capture::TargetSet set = target_set(st);
    for (D3D11ContextImpl* ctx : st.contexts)
        if (auto rec = ctx->recorder()) rec->bind(dc, set);
}

void capture_draw(ID3D11DeviceContext* dc, DcState& st) {
    for (D3D11ContextImpl* ctx : st.contexts) ctx->note_draw();
    if (!capturing(st)) return;
    for (D3D11ContextImpl* ctx : st.contexts) {
        auto rec = ctx->recorder();
        if (!rec) continue;
        if (!rec->knows(dc)) rec->bind(dc, target_set(st));
        rec->draw(dc);
    }
}

}  // namespace vangfx::backend::d3d11
