/**
 * src/backend/d3d11/store.cpp — D3D11ResourceStore.
 *
 * Everything FrameContext exposes on the D3D11 backend. Before this file every
 * one of those methods returned NotInitialized from one of seven identical stub
 * structs.
 *
 * Two things worth knowing before reading:
 *
 * Views, not resources. A D3D11 render target is an ID3D11RenderTargetView over
 * a texture, and that is what OMSetRenderTargets deals in -- so that is what a
 * RenderTargetHandle names here. Readback needs the texture underneath, which
 * is reached with GetResource.
 *
 * Adopted views. The interesting render targets belong to the game, not to us.
 * adopt_render_target lets the OMSetRenderTargets hook register one under the
 * handle it already reports in its event, so a callback can take the handle
 * straight from the event and read it back. Those views are borrowed: `owned`
 * is false and release_all leaves them alone.
 */

#include "store.hpp"
#include "d3d11_impl.hpp"
#include <format_map.hpp>

#include <algorithm>
#include <cstring>
#include <string>

namespace vangfx::backend::d3d11 {

namespace {

template <typename T>
void safe_release(T*& p) noexcept
{
    if (p) { p->Release(); p = nullptr; }
}

// D3D11_BIND_FLAG set implied by a TextureDesc.
UINT bind_flags_for(const TextureDesc& d) noexcept
{
    UINT flags = D3D11_BIND_SHADER_RESOURCE;
    if (d.is_render_target) flags |= D3D11_BIND_RENDER_TARGET;
    if (d.is_uav)           flags |= D3D11_BIND_UNORDERED_ACCESS;
    if (d.is_depth) {
        // A depth texture cannot be both DEPTH_STENCIL and RENDER_TARGET, and
        // it can only carry SHADER_RESOURCE if its format is typeless.
        flags = D3D11_BIND_DEPTH_STENCIL;
        if (fmtmap::to_dxgi(d.format) == DXGI_FORMAT_R24G8_TYPELESS ||
            fmtmap::to_dxgi(d.format) == DXGI_FORMAT_R32_TYPELESS  ||
            fmtmap::to_dxgi(d.format) == DXGI_FORMAT_R16_TYPELESS) {
            flags |= D3D11_BIND_SHADER_RESOURCE;
        }
    }
    return flags;
}

}  // namespace

void D3D11ResourceStore::release_all() noexcept
{
    std::lock_guard lk(mutex_);

    for (auto& [id, rec] : textures_) {
        safe_release(rec.srv); safe_release(rec.rtv);
        safe_release(rec.dsv); safe_release(rec.tex);
    }
    // Adopted views belong to the game. Releasing one would take a reference
    // the game still expects to hold.
    for (auto& [id, rec] : render_targets_) if (rec.owned) safe_release(rec.view);
    for (auto& [id, rec] : depth_buffers_)  if (rec.owned) safe_release(rec.view);

    for (auto& [id, rec] : shaders_) {
        safe_release(rec.vs); safe_release(rec.ps); safe_release(rec.gs);
        safe_release(rec.hs); safe_release(rec.ds); safe_release(rec.cs);
    }

    textures_.clear();
    render_targets_.clear();
    depth_buffers_.clear();
    shaders_.clear();
    buffers_.clear();

    scratch_.clear();
    scratch_.shrink_to_fit();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Creation
// ─────────────────────────────────────────────────────────────────────────────

Result<TextureHandle>
D3D11ResourceStore::create_texture(uint64_t id, const TextureDesc& d)
{
    if (!device_) {
        return gfx_err<TextureHandle>(ShaderError::NotInitialized, "D3D11: no device.");
    }
    if (d.width == 0 || d.height == 0) {
        return gfx_err<TextureHandle>(ShaderError::InvalidFormat,
            "D3D11: texture needs a non-zero width and height.");
    }
    InternalScope internal_work;

    const DXGI_FORMAT fmt = fmtmap::to_dxgi(d.format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        return gfx_err<TextureHandle>(ShaderError::InvalidFormat,
            "D3D11: no DXGI equivalent for the requested SurfaceFormat.");
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = d.width;
    td.Height           = d.height;
    td.MipLevels        = d.mip_levels;
    td.ArraySize        = (d.type == TextureType::TexCube) ? 6u
                        : std::max(1u, d.array_size);
    td.Format           = fmt;
    td.SampleDesc.Count = std::max(1u, d.msaa_samples);
    td.Usage            = d.is_dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    td.BindFlags        = bind_flags_for(d);
    td.CPUAccessFlags   = d.is_dynamic ? D3D11_CPU_ACCESS_WRITE : 0u;
    td.MiscFlags        = (d.type == TextureType::TexCube)
                        ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;

    // A multisampled texture cannot be dynamic, and a dynamic one cannot be a
    // render target. Rather than let D3D11 reject the combination with
    // E_INVALIDARG, say which pair is the problem.
    if (d.is_dynamic && (td.BindFlags & D3D11_BIND_RENDER_TARGET)) {
        return gfx_err<TextureHandle>(ShaderError::InvalidFormat,
            "D3D11: a texture cannot be both dynamic and a render target. "
            "Dynamic means the CPU writes it each frame; a render target is "
            "written by the GPU.");
    }

    TextureRec rec;
    rec.format = d.format;
    rec.width  = d.width;
    rec.height = d.height;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &rec.tex)) || !rec.tex) {
        return gfx_err<TextureHandle>(ShaderError::OutOfMemory,
            "D3D11: CreateTexture2D failed. Check the format supports the "
            "requested bind flags with CheckFormatSupport.");
    }

    // Views for whatever it was asked to be. A typeless resource is viewed
    // through its typed counterpart, which is the whole reason to make one.
    if (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format        = fmtmap::srv_format_for(fmt);
        sd.ViewDimension = (td.SampleDesc.Count > 1)
                         ? D3D11_SRV_DIMENSION_TEXTURE2DMS
                         : D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = d.mip_levels ? d.mip_levels : 1u;
        (void)device_->CreateShaderResourceView(rec.tex, &sd, &rec.srv);
    }
    if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
        (void)device_->CreateRenderTargetView(rec.tex, nullptr, &rec.rtv);
    }
    if (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format        = fmtmap::dsv_format_for(fmt);
        dd.ViewDimension = (td.SampleDesc.Count > 1)
                         ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                         : D3D11_DSV_DIMENSION_TEXTURE2D;
        (void)device_->CreateDepthStencilView(rec.tex, &dd, &rec.dsv);
    }

    std::lock_guard lk(mutex_);
    textures_[id] = rec;
    return TextureHandle{ id };
}

Result<RenderTargetHandle>
D3D11ResourceStore::create_render_target(uint64_t id, const RenderTargetDesc& d)
{
    TextureDesc td;
    td.format           = d.format;
    td.width            = d.width;
    td.height           = d.height;
    td.msaa_samples     = d.msaa_samples;
    td.mip_levels       = 1;
    td.is_render_target = true;

    // Made as a texture and then unwrapped: a render target that cannot also be
    // sampled is nearly useless for the things this API is used for, and D3D11
    // charges nothing for the extra view.
    auto tex = create_texture(id, td);
    if (!tex) return gfx_err<RenderTargetHandle>(tex.error().code, tex.error().message);

    std::lock_guard lk(mutex_);
    const auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.rtv) {
        return gfx_err<RenderTargetHandle>(ShaderError::OutOfMemory,
            "D3D11: the texture was created but no render-target view could be "
            "made over it.");
    }

    ViewRec v;
    v.view   = it->second.rtv;
    v.tex    = it->second.tex;
    v.format = d.format;
    v.width  = d.width;
    v.height = d.height;
    v.owned  = false;   // the TextureRec owns it; releasing twice would be a bug
    render_targets_[id] = v;
    return RenderTargetHandle{ id };
}

Result<DepthBufferHandle>
D3D11ResourceStore::create_depth_buffer(uint64_t id, const DepthBufferDesc& d)
{
    TextureDesc td;
    td.format       = d.format;
    td.width        = d.width;
    td.height       = d.height;
    td.msaa_samples = d.msaa_samples;
    td.mip_levels   = 1;
    td.is_depth     = true;

    auto tex = create_texture(id, td);
    if (!tex) return gfx_err<DepthBufferHandle>(tex.error().code, tex.error().message);

    std::lock_guard lk(mutex_);
    const auto it = textures_.find(id);
    if (it == textures_.end() || !it->second.dsv) {
        return gfx_err<DepthBufferHandle>(ShaderError::OutOfMemory,
            "D3D11: the texture was created but no depth-stencil view could be "
            "made over it.");
    }

    ViewRec v;
    v.view   = it->second.dsv;
    v.tex    = it->second.tex;
    v.format = d.format;
    v.width  = d.width;
    v.height = d.height;
    v.owned  = false;
    depth_buffers_[id] = v;
    return DepthBufferHandle{ id };
}

// ─────────────────────────────────────────────────────────────────────────────
//  Adoption — registering a view the game made
// ─────────────────────────────────────────────────────────────────────────────

void D3D11ResourceStore::describe_from_view(ViewRec& rec) noexcept
{
    if (!rec.tex) return;
    D3D11_TEXTURE2D_DESC td{};
    rec.tex->GetDesc(&td);
    rec.width  = td.Width;
    rec.height = td.Height;
    rec.format = fmtmap::from_dxgi(td.Format);
}

namespace {
ID3D11Texture2D* as_texture(ID3D11Resource* res) noexcept {
    if (!res) return nullptr;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))) || !tex) return nullptr;
    tex->Release();   // borrowed: the game owns it
    return tex;
}
}  // namespace

void D3D11ResourceStore::adopt_render_target(uint64_t id, ID3D11Resource* res) noexcept
{
    ID3D11Texture2D* tex = as_texture(res);
    if (!id || !tex) return;
    std::lock_guard lk(mutex_);
    if (render_targets_.contains(id)) return;
    ViewRec rec;
    rec.tex   = tex;
    rec.owned = false;
    describe_from_view(rec);
    render_targets_[id] = rec;
}

void D3D11ResourceStore::adopt_depth_buffer(uint64_t id, ID3D11Resource* res) noexcept
{
    ID3D11Texture2D* tex = as_texture(res);
    if (!id || !tex) return;
    std::lock_guard lk(mutex_);
    if (depth_buffers_.contains(id)) return;
    ViewRec rec;
    rec.tex   = tex;
    rec.owned = false;
    describe_from_view(rec);
    depth_buffers_[id] = rec;
}

ID3D11Resource* D3D11ResourceStore::game_resource(uint64_t id) const noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = render_targets_.find(id); it != render_targets_.end() && !it->second.owned) return it->second.tex;
    if (auto it = depth_buffers_.find(id);  it != depth_buffers_.end()  && !it->second.owned) return it->second.tex;
    if (auto it = buffers_.find(id); it != buffers_.end()) return it->second.buf;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Destruction and lookup
// ─────────────────────────────────────────────────────────────────────────────

void D3D11ResourceStore::destroy(TextureHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = textures_.find(h.id); it != textures_.end()) {
        render_targets_.erase(h.id);
        depth_buffers_.erase(h.id);
        safe_release(it->second.srv); safe_release(it->second.rtv);
        safe_release(it->second.dsv); safe_release(it->second.tex);
        textures_.erase(it);
    }
}

void D3D11ResourceStore::destroy(RenderTargetHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = render_targets_.find(h.id); it != render_targets_.end()) {
        if (it->second.owned) safe_release(it->second.view);
        render_targets_.erase(it);
    }
}

void D3D11ResourceStore::destroy(DepthBufferHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = depth_buffers_.find(h.id); it != depth_buffers_.end()) {
        if (it->second.owned) safe_release(it->second.view);
        depth_buffers_.erase(it);
    }
}

SurfaceFormat D3D11ResourceStore::format_of(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    return (it != textures_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

SurfaceFormat D3D11ResourceStore::format_of(RenderTargetHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = render_targets_.find(h.id);
    return (it != render_targets_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

SurfaceFormat D3D11ResourceStore::format_of(DepthBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = depth_buffers_.find(h.id);
    return (it != depth_buffers_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

ID3D11Texture2D* D3D11ResourceStore::texture(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    return (it != textures_.end()) ? it->second.tex : nullptr;
}

ID3D11ShaderResourceView* D3D11ResourceStore::srv(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    return (it != textures_.end()) ? it->second.srv : nullptr;
}

ID3D11RenderTargetView* D3D11ResourceStore::rtv(RenderTargetHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = render_targets_.find(h.id);
    return (it != render_targets_.end())
         ? static_cast<ID3D11RenderTargetView*>(it->second.view) : nullptr;
}

ID3D11DepthStencilView* D3D11ResourceStore::dsv(DepthBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = depth_buffers_.find(h.id);
    return (it != depth_buffers_.end())
         ? static_cast<ID3D11DepthStencilView*>(it->second.view) : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Readback
// ─────────────────────────────────────────────────────────────────────────────

Result<ReadbackData> D3D11ResourceStore::read_texture(ID3D11Texture2D* src)
{
    if (!src || !device_ || !ctx_) {
        return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
            "D3D11: readback of a resource that does not exist.");
    }
    InternalScope internal_work;

    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    const SurfaceFormat fmt = fmtmap::from_dxgi(sd.Format);
    const uint32_t pitch = fmtmap::row_pitch(fmt, sd.Width);
    const uint32_t rows  = fmtmap::row_count(fmt, sd.Height);
    if (pitch == 0 || rows == 0) {
        return gfx_err<ReadbackData>(ShaderError::InvalidFormat,
            "D3D11: readback of a format with no defined pixel size (a planar "
            "video format, or one this build does not know).");
    }

    // A staging twin: same size and format, no bind flags, CPU readable. This
    // is the only kind of resource Map will accept for reading.
    D3D11_TEXTURE2D_DESC td = sd;
    td.Usage          = D3D11_USAGE_STAGING;
    td.BindFlags      = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags      = 0;
    // CopyResource cannot resolve multisampling, so a multisampled source has
    // to be resolved into a single-sample intermediate first.
    td.SampleDesc.Count   = 1;
    td.SampleDesc.Quality = 0;

    if (sd.SampleDesc.Count > 1 && fmtmap::is_depth(fmt)) {
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
            "D3D11: a multisampled depth buffer cannot be resolved to read back.");
    }

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging)) || !staging) {
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
            "D3D11: could not create the staging texture to read back into.");
    }

    if (sd.SampleDesc.Count > 1) {
        // A staging texture cannot be a resolve destination: resolve into a
        // default-usage one and copy that.
        D3D11_TEXTURE2D_DESC rd = td;
        rd.Usage          = D3D11_USAGE_DEFAULT;
        rd.CPUAccessFlags = 0;
        ID3D11Texture2D* resolved = nullptr;
        if (FAILED(device_->CreateTexture2D(&rd, nullptr, &resolved)) || !resolved) {
            staging->Release();
            return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
                "D3D11: could not create the texture to resolve the multisampled source into.");
        }
        ctx_->ResolveSubresource(resolved, 0, src, 0, sd.Format);
        ctx_->CopyResource(staging, resolved);
        resolved->Release();
    } else {
        ctx_->CopyResource(staging, src);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
            "D3D11: Map failed on the staging texture.");
    }

    // Repack to a tight pitch; the mapped one is whatever the driver chose.
    scratch_.resize(static_cast<size_t>(pitch) * rows);
    const auto* p = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < rows; ++y) {
        std::memcpy(scratch_.data() + static_cast<size_t>(y) * pitch,
                    p + static_cast<size_t>(y) * mapped.RowPitch, pitch);
    }

    ctx_->Unmap(staging, 0);
    staging->Release();

    ReadbackData out;
    out.pixels    = std::span<const uint8_t>(scratch_.data(), scratch_.size());
    out.width     = sd.Width;
    out.height    = sd.Height;
    out.format    = fmt;
    out.row_pitch = pitch;
    return out;
}

Result<ReadbackData> D3D11ResourceStore::readback(TextureHandle h)
{
    ID3D11Texture2D* tex = nullptr;
    {
        std::lock_guard lk(mutex_);
        const auto it = textures_.find(h.id);
        if (it == textures_.end()) {
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D11: readback of a texture this context does not know.");
        }
        tex = it->second.tex;
    }
    return read_texture(tex);
}

Result<ReadbackData> D3D11ResourceStore::readback(RenderTargetHandle h)
{
    ID3D11Texture2D* tex = nullptr;
    {
        std::lock_guard lk(mutex_);
        const auto it = render_targets_.find(h.id);
        if (it == render_targets_.end()) {
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D11: readback of a render target this context does not know. "
                "Targets seen in an event are registered as they are bound, so "
                "this usually means the handle came from a different context.");
        }
        tex = it->second.tex;
    }
    return read_texture(tex);
}

Result<ReadbackData> D3D11ResourceStore::readback(DepthBufferHandle h)
{
    ID3D11Texture2D* tex = nullptr;
    {
        std::lock_guard lk(mutex_);
        const auto it = depth_buffers_.find(h.id);
        if (it == depth_buffers_.end()) {
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D11: readback of a depth buffer this context does not know.");
        }
        tex = it->second.tex;
    }
    // Unlike D3D9, a depth texture reads back like any other: CopyResource
    // accepts it, and the staging twin keeps the depth format.
    return read_texture(tex);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shaders
// ─────────────────────────────────────────────────────────────────────────────

Result<Shader>
D3D11ResourceStore::register_shader(uint64_t id, const ShaderBuilder& b)
{
    if (!device_) {
        return gfx_err<Shader>(ShaderError::NotInitialized, "D3D11: no device.");
    }

    InternalScope internal_work;
    const auto& blob = b.precompiled_bytecode();
    if (blob.empty()) {
        return gfx_err<Shader>(ShaderError::CompileFailed,
            "D3D11: no bytecode to make a shader from. Build shaders through "
            "ShaderBuilder::build(ctx), which compiles the source first.");
    }

    const std::string& profile = b.shader_profile();
    if (profile.size() < 2) {
        return gfx_err<Shader>(ShaderError::InvalidFormat,
            "D3D11: the shader profile is missing. Set one with "
            "ShaderBuilder::profile(), for example \"ps_5_0\".");
    }

    // Shader model 6 is DXIL, which D3D11 cannot load at all.
    if (profile.size() >= 4 && profile[3] >= '6') {
        return gfx_err<Shader>(ShaderError::Unsupported,
            "D3D11: shader model 6 is DXIL and only D3D12 accepts it. Use a "
            "5_0 or 5_1 profile here.");
    }

    ShaderRec rec;
    HRESULT hr = E_FAIL;
    const void* bc = blob.data();
    const SIZE_T n = blob.size();

    if      (profile.rfind("vs", 0) == 0) { rec.stage = ShaderStage::Vertex;   hr = device_->CreateVertexShader  (bc, n, nullptr, &rec.vs); }
    else if (profile.rfind("ps", 0) == 0) { rec.stage = ShaderStage::Pixel;    hr = device_->CreatePixelShader   (bc, n, nullptr, &rec.ps); }
    else if (profile.rfind("gs", 0) == 0) { rec.stage = ShaderStage::Geometry; hr = device_->CreateGeometryShader(bc, n, nullptr, &rec.gs); }
    else if (profile.rfind("hs", 0) == 0) { rec.stage = ShaderStage::Hull;     hr = device_->CreateHullShader    (bc, n, nullptr, &rec.hs); }
    else if (profile.rfind("ds", 0) == 0) { rec.stage = ShaderStage::Domain;   hr = device_->CreateDomainShader  (bc, n, nullptr, &rec.ds); }
    else if (profile.rfind("cs", 0) == 0) { rec.stage = ShaderStage::Compute;  hr = device_->CreateComputeShader (bc, n, nullptr, &rec.cs); }
    else {
        return gfx_err<Shader>(ShaderError::InvalidFormat,
            "D3D11: unrecognised shader profile '" + profile + "'.");
    }

    if (FAILED(hr)) {
        return gfx_err<Shader>(ShaderError::CompileFailed,
            "D3D11: the device rejected the compiled bytecode for profile '" +
            profile + "'. It may exceed the device's feature level.");
    }

    {
        std::lock_guard lk(mutex_);
        shaders_[id] = rec;
    }

    Shader out;
    out.handle   = ShaderHandle{ id };
    out.stage    = rec.stage;
    out.lang     = ShaderLang::DXBC;
    out.entry    = b.entry_point();
    out.profile  = profile;
    out.bytecode = blob;
    return out;
}

void D3D11ResourceStore::destroy_shader(ShaderHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = shaders_.find(h.id); it != shaders_.end()) {
        safe_release(it->second.vs); safe_release(it->second.ps);
        safe_release(it->second.gs); safe_release(it->second.hs);
        safe_release(it->second.ds); safe_release(it->second.cs);
        shaders_.erase(it);
    }
}

bool D3D11ResourceStore::has_shader(ShaderHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    return shaders_.contains(h.id);
}

ID3D11VertexShader* D3D11ResourceStore::vertex_shader(ShaderHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = shaders_.find(h.id);
    return it != shaders_.end() ? it->second.vs : nullptr;
}

ID3D11PixelShader* D3D11ResourceStore::pixel_shader(ShaderHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = shaders_.find(h.id);
    return it != shaders_.end() ? it->second.ps : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constant buffers
// ─────────────────────────────────────────────────────────────────────────────

Result<void>
D3D11ResourceStore::update_constant_buffer(ConstantBufferHandle h, const void* data, uint32_t size)
{
    if (!ctx_) return gfx_err<void>(ShaderError::NotInitialized, "D3D11: no immediate context.");
    if (!data || size == 0) return gfx_err<void>(ShaderError::InvalidFormat, "D3D11: constant buffer update with no data.");
    ID3D11Buffer* buf = nullptr;
    {
        std::lock_guard lk(mutex_);
        const auto it = buffers_.find(h.id);
        if (it == buffers_.end() || !it->second.buf) {
            return gfx_err<void>(ShaderError::InvalidHandle,
                "D3D11: no such buffer. Constant buffers are known from their Create event.");
        }
        buf = it->second.buf;
    }
    D3D11_BUFFER_DESC bd{};
    buf->GetDesc(&bd);
    if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
        return gfx_err<void>(ShaderError::InvalidHandle, "D3D11: that buffer is not a constant buffer.");
    if (size != bd.ByteWidth) {
        return gfx_err<void>(ShaderError::InvalidFormat,
            "D3D11: a constant buffer is rewritten whole; the update is " + std::to_string(size) +
            " bytes and the buffer " + std::to_string(bd.ByteWidth) + ".");
    }
    InternalScope internal_work;
    if (bd.Usage == D3D11_USAGE_DYNAMIC) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return gfx_err<void>(ShaderError::ReadbackFailed, "D3D11: Map failed on the constant buffer.");
        std::memcpy(m.pData, data, size);
        ctx_->Unmap(buf, 0);
    } else if (bd.Usage == D3D11_USAGE_DEFAULT) {
        ctx_->UpdateSubresource(buf, 0, nullptr, data, 0, 0);
    } else {
        return gfx_err<void>(ShaderError::Unsupported, "D3D11: the constant buffer is immutable.");
    }
    return {};
}

// ---------------------------------------------------------------------------
//  Buffer adoption and readback
//
//  A staging twin and a Map, the same shape as read_texture above -- D3D11 will
//  not map a resource the GPU can use, so reading anything means copying it to
//  one the GPU cannot. The only wrinkle is the range: CopySubresourceRegion
//  takes a D3D11_BOX measured in bytes for a buffer, which lets a caller ask for
//  64 bytes of a 4 MB vertex buffer and pay for 64.
// ---------------------------------------------------------------------------

void D3D11ResourceStore::adopt_buffer(uint64_t id, ID3D11Buffer* buf,
                                      uint32_t size) noexcept
{
    if (!buf || size == 0) return;
    std::lock_guard lk(mutex_);
    if (buffers_.contains(id)) return;
    buffers_[id] = BufferRec{ buf, size };
}

void D3D11ResourceStore::forget(uint64_t id) noexcept
{
    std::lock_guard lk(mutex_);
    buffers_.erase(id);
    if (auto it = render_targets_.find(id); it != render_targets_.end() && !it->second.owned && !textures_.contains(id))
        render_targets_.erase(it);
    if (auto it = depth_buffers_.find(id); it != depth_buffers_.end() && !it->second.owned && !textures_.contains(id))
        depth_buffers_.erase(it);
}

Result<BufferData> D3D11ResourceStore::read_buffer(uint64_t id,
                                                   uint64_t offset,
                                                   uint64_t size)
{
    ID3D11Buffer* src = nullptr;
    uint64_t      buf_size = 0;
    {
        std::lock_guard lk(mutex_);
        const auto it = buffers_.find(id);
        if (it == buffers_.end() || !it->second.buf) {
            return gfx_err<BufferData>(ShaderError::InvalidHandle,
                "D3D11: readback of a buffer this context does not know. "
                "Buffers are registered as the application creates them, so a "
                "handle from before VanGFX attached is not one of them.");
        }
        src      = it->second.buf;
        buf_size = it->second.size;
    }

    if (!device_ || !ctx_) {
        return gfx_err<BufferData>(ShaderError::NotInitialized,
            "D3D11: the context has no device; readback is unavailable.");
    }
    InternalScope internal_work;

    // Clamped rather than rejected, so asking for the first 256 bytes of every
    // buffer needs no special case for the short ones.
    if (offset >= buf_size) {
        return gfx_err<BufferData>(ShaderError::InvalidFormat,
            "D3D11: offset " + std::to_string(offset) + " is past the end of a "
            + std::to_string(buf_size) + "-byte buffer.");
    }
    const uint64_t avail = buf_size - offset;
    if (size == 0 || size > avail) size = avail;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = static_cast<UINT>(size);
    bd.Usage          = D3D11_USAGE_STAGING;
    bd.BindFlags      = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Buffer* staging = nullptr;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &staging)) || !staging) {
        return gfx_err<BufferData>(ShaderError::ReadbackFailed,
            "D3D11: could not create the staging buffer to read back into.");
    }

    D3D11_BOX box{};
    box.left   = static_cast<UINT>(offset);
    box.right  = static_cast<UINT>(offset + size);
    box.top    = 0; box.bottom = 1;
    box.front  = 0; box.back   = 1;
    ctx_->CopySubresourceRegion(staging, 0, 0, 0, 0, src, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        staging->Release();
        return gfx_err<BufferData>(ShaderError::ReadbackFailed,
            "D3D11: Map failed on the staging buffer.");
    }

    scratch_.resize(static_cast<size_t>(size));
    std::memcpy(scratch_.data(), mapped.pData, static_cast<size_t>(size));

    ctx_->Unmap(staging, 0);
    staging->Release();

    BufferData out;
    out.bytes       = std::span<const uint8_t>(scratch_.data(), scratch_.size());
    out.offset      = offset;
    out.buffer_size = buf_size;
    return out;
}

}  // namespace vangfx::backend::d3d11
