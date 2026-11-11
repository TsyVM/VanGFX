/**
 * src/backend/d3d9/store.cpp — D3D9ResourceStore.
 *
 * WHAT D3D9 MAKES AWKWARD
 *
 * Pools. A D3D9 resource is created in a pool, and the pool decides what can be
 * done with it: DEFAULT lives in video memory and cannot be locked (and must be
 * released before a plain device's Reset), MANAGED is shadowed in system memory
 * and survives a reset, SYSTEMMEM cannot be used for rendering. A texture that
 * is both renderable and readable is not one resource; it is a DEFAULT one plus
 * a SYSTEMMEM one to copy into.
 *
 * Readback. There is no general "copy this surface to the CPU". The one path
 * that works is GetRenderTargetData, which copies a DEFAULT render target into
 * a SYSTEMMEM offscreen-plain surface of identical size and format -- and only
 * from a single-sampled one, so a multisampled target is resolved into another
 * first (StretchRect). Anything that is not a render target has to be lockable
 * in the first place.
 *
 * Shader models. vs_3_0 pairs only with ps_3_0; anything below 3 pairs with
 * anything below 3 and with the fixed-function pipeline. A replacement keeps
 * its source so the other form can be compiled when a draw needs it.
 */

#include "store.hpp"
#include "d3d9_impl.hpp"
#include <core/shader_compile.hpp>
#include <format_map.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace vangfx::backend::d3d9 {

namespace {

// D3D9's usage/pool pair for a requested texture.
//
// Rendering to a texture requires D3DUSAGE_RENDERTARGET and D3DPOOL_DEFAULT,
// which together make it unlockable. A dynamic texture is DEFAULT and
// lockable. Anything else is MANAGED, the pool that survives a device reset
// and needs no restore code -- except on an Ex device, which has no managed
// pool; DEFAULT survives an Ex device's reset instead.
void usage_and_pool_for(const TextureDesc& d, bool ex, DWORD& usage, D3DPOOL& pool) noexcept
{
    usage = 0;
    pool  = ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;

    if (d.is_render_target) {
        usage = D3DUSAGE_RENDERTARGET;
        pool  = D3DPOOL_DEFAULT;
    } else if (d.is_depth) {
        usage = D3DUSAGE_DEPTHSTENCIL;
        pool  = D3DPOOL_DEFAULT;
    } else if (d.is_dynamic || d.type == TextureType::TexDynamic) {
        usage = D3DUSAGE_DYNAMIC;
        pool  = D3DPOOL_DEFAULT;
    }
}

bool is_ex_device(IDirect3DDevice9* device) noexcept
{
    IDirect3DDevice9Ex* ex = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex))) || !ex)
        return false;
    ex->Release();
    return true;
}

// A MANAGED or SYSTEMMEM surface can be locked directly; a DEFAULT one cannot,
// and has to go through GetRenderTargetData first.
[[nodiscard]] bool pool_is_lockable(D3DPOOL pool) noexcept
{
    return pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH;
}

template <typename T>
void safe_release(T*& p) noexcept
{
    if (p) { p->Release(); p = nullptr; }
}

std::string hex(HRESULT hr)
{
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Lifetime
// ─────────────────────────────────────────────────────────────────────────────

void D3D9ResourceStore::release_all() noexcept
{
    std::lock_guard lk(mutex_);
    for (auto& [id, rec] : textures_) safe_release(rec.tex);
    for (auto& [id, rec] : render_targets_) if (rec.owned) safe_release(rec.surf);
    for (auto& [id, rec] : depth_buffers_)  if (rec.owned) safe_release(rec.surf);
    for (auto& [id, rec] : shaders_) {
        safe_release(rec.vs); safe_release(rec.ps);
        safe_release(rec.other_vs); safe_release(rec.other_ps);
    }
    textures_.clear();
    render_targets_.clear();
    depth_buffers_.clear();
    shaders_.clear();
    buffers_.clear();
    scratch_.clear();
    scratch_.shrink_to_fit();
}

void D3D9ResourceStore::release_default_pool() noexcept
{
    std::lock_guard lk(mutex_);
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.pool != D3DPOOL_DEFAULT) { ++it; continue; }
        safe_release(it->second.tex);
        it = textures_.erase(it);
    }
    for (auto* map : { &render_targets_, &depth_buffers_ }) {
        for (auto& [id, rec] : *map) if (rec.owned) safe_release(rec.surf);
        map->clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Creation
// ─────────────────────────────────────────────────────────────────────────────

Result<TextureHandle>
D3D9ResourceStore::create_texture(uint64_t id, const TextureDesc& d)
{
    if (!device_) return gfx_err<TextureHandle>(ShaderError::NotInitialized, "D3D9: no device.");
    if (d.width == 0 || d.height == 0)
        return gfx_err<TextureHandle>(ShaderError::InvalidFormat, "D3D9: texture needs a non-zero width and height.");

    const D3DFORMAT fmt = fmtmap::to_d3d9(d.format);
    if (fmt == D3DFMT_UNKNOWN)
        return gfx_err<TextureHandle>(ShaderError::InvalidFormat, "D3D9: no Direct3D 9 equivalent for the requested SurfaceFormat.");

    // Volume and cube textures are separate creation calls with separate
    // interface types. Rejecting them here is honest; silently making a 2D
    // texture instead would be worse than an error.
    if (d.type == TextureType::TexCube || d.type == TextureType::TexVolume ||
        d.type == TextureType::TexArray || d.type == TextureType::Tex1D) {
        return gfx_err<TextureHandle>(ShaderError::Unsupported,
            "D3D9: only 2D textures are supported here. Cube, volume, array and 1D textures are different "
            "interfaces in this API.");
    }

    DWORD   usage{};
    D3DPOOL pool{};
    usage_and_pool_for(d, is_ex_device(device_), usage, pool);

    InternalScope internal_work;
    IDirect3DTexture9* tex = nullptr;
    const HRESULT hr = device_->CreateTexture(d.width, d.height, d.mip_levels, usage, fmt, pool, &tex, nullptr);
    if (FAILED(hr) || !tex) {
        return gfx_err<TextureHandle>(ShaderError::OutOfMemory,
            "D3D9: CreateTexture failed (hr=" + hex(hr) + "). Check the format is supported for this usage with "
            "CheckDeviceFormat.");
    }

    std::lock_guard lk(mutex_);
    textures_[id] = TextureRec{ tex, d.format, d.width, d.height, pool };
    return TextureHandle{ id };
}

Result<RenderTargetHandle>
D3D9ResourceStore::create_render_target(uint64_t id, const RenderTargetDesc& d)
{
    if (!device_) return gfx_err<RenderTargetHandle>(ShaderError::NotInitialized, "D3D9: no device.");
    const D3DFORMAT fmt = fmtmap::to_d3d9(d.format);
    if (fmt == D3DFMT_UNKNOWN)
        return gfx_err<RenderTargetHandle>(ShaderError::InvalidFormat, "D3D9: no Direct3D 9 equivalent for the requested SurfaceFormat.");

    // Lockable is deliberately FALSE. A lockable render target is markedly
    // slower on most drivers, and readback here goes through
    // GetRenderTargetData into a system-memory surface instead.
    InternalScope internal_work;
    IDirect3DSurface9* surf = nullptr;
    const HRESULT hr = device_->CreateRenderTarget(
        d.width, d.height, fmt, static_cast<D3DMULTISAMPLE_TYPE>(d.msaa_samples > 1 ? d.msaa_samples : 0),
        0, FALSE, &surf, nullptr);
    if (FAILED(hr) || !surf) {
        return gfx_err<RenderTargetHandle>(ShaderError::OutOfMemory,
            "D3D9: CreateRenderTarget failed (hr=" + hex(hr) + "). The format or the multisample count may not "
            "be supported for rendering on this device.");
    }

    std::lock_guard lk(mutex_);
    render_targets_[id] = SurfaceRec{ surf, d.format, d.width, d.height, true };
    return RenderTargetHandle{ id };
}

Result<DepthBufferHandle>
D3D9ResourceStore::create_depth_buffer(uint64_t id, const DepthBufferDesc& d)
{
    if (!device_) return gfx_err<DepthBufferHandle>(ShaderError::NotInitialized, "D3D9: no device.");
    const D3DFORMAT fmt = fmtmap::to_d3d9(d.format);
    if (fmt == D3DFMT_UNKNOWN)
        return gfx_err<DepthBufferHandle>(ShaderError::InvalidFormat, "D3D9: no Direct3D 9 equivalent for the requested depth format.");

    // Discard is FALSE so the contents survive to be read.
    InternalScope internal_work;
    IDirect3DSurface9* surf = nullptr;
    const HRESULT hr = device_->CreateDepthStencilSurface(
        d.width, d.height, fmt, static_cast<D3DMULTISAMPLE_TYPE>(d.msaa_samples > 1 ? d.msaa_samples : 0),
        0, FALSE, &surf, nullptr);
    if (FAILED(hr) || !surf)
        return gfx_err<DepthBufferHandle>(ShaderError::OutOfMemory, "D3D9: CreateDepthStencilSurface failed (hr=" + hex(hr) + ").");

    std::lock_guard lk(mutex_);
    depth_buffers_[id] = SurfaceRec{ surf, d.format, d.width, d.height, true };
    return DepthBufferHandle{ id };
}

// ─────────────────────────────────────────────────────────────────────────────
//  Destruction, adoption and lookup
// ─────────────────────────────────────────────────────────────────────────────

void D3D9ResourceStore::destroy(TextureHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = textures_.find(h.id); it != textures_.end()) {
        safe_release(it->second.tex);
        textures_.erase(it);
    }
}

void D3D9ResourceStore::destroy(RenderTargetHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = render_targets_.find(h.id); it != render_targets_.end() && it->second.owned) {
        safe_release(it->second.surf);
        render_targets_.erase(it);
    }
}

void D3D9ResourceStore::destroy(DepthBufferHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = depth_buffers_.find(h.id); it != depth_buffers_.end() && it->second.owned) {
        safe_release(it->second.surf);
        depth_buffers_.erase(it);
    }
}

void D3D9ResourceStore::adopt(std::unordered_map<uint64_t, SurfaceRec>& map, uint64_t id,
                              IDirect3DSurface9* surface) noexcept
{
    if (!id || !surface) return;
    D3DSURFACE_DESC d{};
    if (FAILED(surface->GetDesc(&d))) return;
    std::lock_guard lk(mutex_);
    auto it = map.find(id);
    if (it != map.end() && (it->second.owned || it->second.surf == surface)) return;
    map[id] = SurfaceRec{ surface, fmtmap::from_d3d9(d.Format), d.Width, d.Height, false };
}

void D3D9ResourceStore::adopt_render_target(uint64_t id, IDirect3DSurface9* surface) noexcept
{
    adopt(render_targets_, id, surface);
}

void D3D9ResourceStore::adopt_depth_buffer(uint64_t id, IDirect3DSurface9* surface) noexcept
{
    adopt(depth_buffers_, id, surface);
}

void D3D9ResourceStore::forget(uint64_t id) noexcept
{
    std::lock_guard lk(mutex_);
    buffers_.erase(id);
    if (auto it = render_targets_.find(id); it != render_targets_.end() && !it->second.owned) render_targets_.erase(it);
    if (auto it = depth_buffers_.find(id); it != depth_buffers_.end() && !it->second.owned) depth_buffers_.erase(it);
}

SurfaceFormat D3D9ResourceStore::format_of(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    return (it != textures_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

SurfaceFormat D3D9ResourceStore::format_of(RenderTargetHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = render_targets_.find(h.id);
    return (it != render_targets_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

SurfaceFormat D3D9ResourceStore::format_of(DepthBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = depth_buffers_.find(h.id);
    return (it != depth_buffers_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

IDirect3DTexture9* D3D9ResourceStore::texture(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    return (it != textures_.end()) ? it->second.tex : nullptr;
}

IDirect3DSurface9* D3D9ResourceStore::surface(RenderTargetHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = render_targets_.find(h.id);
    return (it != render_targets_.end()) ? it->second.surf : nullptr;
}

IDirect3DSurface9* D3D9ResourceStore::surface(DepthBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = depth_buffers_.find(h.id);
    return (it != depth_buffers_.end()) ? it->second.surf : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Readback
// ─────────────────────────────────────────────────────────────────────────────

Result<ReadbackData>
D3D9ResourceStore::read_surface(IDirect3DSurface9* src, bool src_is_lockable)
{
    if (!src || !device_)
        return gfx_err<ReadbackData>(ShaderError::InvalidHandle, "D3D9: readback of a surface that does not exist.");

    D3DSURFACE_DESC desc{};
    src->GetDesc(&desc);
    const SurfaceFormat fmt   = fmtmap::from_d3d9(desc.Format);
    const uint32_t      pitch = fmtmap::row_pitch(fmt, desc.Width);
    const uint32_t      rows  = fmtmap::row_count(fmt, desc.Height);
    if (fmt == SurfaceFormat::Unknown || pitch == 0 || rows == 0) {
        return gfx_err<ReadbackData>(ShaderError::InvalidFormat,
            "D3D9: readback of a format with no defined pixel size (D3DFORMAT " +
            std::to_string(static_cast<unsigned>(desc.Format)) + ").");
    }

    InternalScope internal_work;
    IDirect3DSurface9* lockable = src;
    IDirect3DSurface9* resolved = nullptr;
    IDirect3DSurface9* staging  = nullptr;
    auto cleanup = [&] {
        if (resolved) resolved->Release();
        if (staging) staging->Release();
    };

    if (!src_is_lockable) {
        IDirect3DSurface9* single = src;
        if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
            // GetRenderTargetData takes only a single-sampled target.
            if (FAILED(device_->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0,
                                                   FALSE, &resolved, nullptr)) ||
                FAILED(device_->StretchRect(src, nullptr, resolved, nullptr, D3DTEXF_NONE))) {
                cleanup();
                return gfx_err<ReadbackData>(ShaderError::ReadbackFailed, "D3D9: could not resolve the multisampled target.");
            }
            single = resolved;
        }
        if (FAILED(device_->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                                        &staging, nullptr)) || !staging) {
            cleanup();
            return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
                "D3D9: could not create the system-memory surface to read back into.");
        }
        if (FAILED(device_->GetRenderTargetData(single, staging))) {
            cleanup();
            return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
                "D3D9: GetRenderTargetData failed. It only copies from a render target in the default pool to a "
                "system-memory surface of identical size and format -- a plain texture or a depth surface cannot "
                "be read this way.");
        }
        lockable = staging;
    }

    D3DLOCKED_RECT locked{};
    if (FAILED(lockable->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        cleanup();
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed, "D3D9: LockRect failed on the surface being read back.");
    }

    // Repacked to a tight pitch: the locked pitch is whatever the driver chose.
    scratch_.resize(static_cast<size_t>(pitch) * rows);
    const auto* srcp = static_cast<const uint8_t*>(locked.pBits);
    for (uint32_t y = 0; y < rows; ++y)
        std::memcpy(scratch_.data() + static_cast<size_t>(y) * pitch, srcp + static_cast<size_t>(y) * locked.Pitch, pitch);
    lockable->UnlockRect();
    cleanup();

    ReadbackData out;
    out.pixels    = std::span<const uint8_t>(scratch_.data(), scratch_.size());
    out.width     = desc.Width;
    out.height    = desc.Height;
    out.format    = fmt;
    out.row_pitch = pitch;
    return out;
}

Result<ReadbackData> D3D9ResourceStore::readback(RenderTargetHandle h)
{
    IDirect3DSurface9* surf = nullptr;
    IDirect3DTexture9* tex  = nullptr;
    {
        std::lock_guard lk(mutex_);
        if (const auto it = render_targets_.find(h.id); it != render_targets_.end()) {
            surf = it->second.surf;
        } else if (const auto t = textures_.find(h.id); t != textures_.end()) {
            tex = t->second.tex;   // a render-target texture this store made
        } else {
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D9: readback of a render target this context neither created nor has seen bound.");
        }
    }
    if (tex) return readback(TextureHandle{ h.id });
    D3DSURFACE_DESC d{};
    surf->GetDesc(&d);
    return read_surface(surf, pool_is_lockable(d.Pool));
}

Result<ReadbackData> D3D9ResourceStore::readback(TextureHandle h)
{
    IDirect3DTexture9* tex = nullptr;
    bool lockable = false;
    {
        std::lock_guard lk(mutex_);
        const auto it = textures_.find(h.id);
        if (it == textures_.end())
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle, "D3D9: readback of a texture this context did not create.");
        tex      = it->second.tex;
        lockable = pool_is_lockable(it->second.pool);
    }
    IDirect3DSurface9* level0 = nullptr;
    if (!tex || FAILED(tex->GetSurfaceLevel(0, &level0)) || !level0)
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed, "D3D9: could not reach the top mip of the texture.");
    auto result = read_surface(level0, lockable);
    level0->Release();
    return result;
}

Result<ReadbackData> D3D9ResourceStore::readback(DepthBufferHandle h)
{
    {
        std::lock_guard lk(mutex_);
        if (!depth_buffers_.contains(h.id))
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D9: readback of a depth buffer this context neither created nor has seen bound.");
    }
    // Direct3D 9 cannot read a depth-stencil surface: GetRenderTargetData and
    // LockRect both reject it. In practice depth is rendered into a texture of
    // a FourCC format the driver recognises (INTZ) and read as a texture.
    return gfx_err<ReadbackData>(ShaderError::Unsupported,
        "D3D9: a depth-stencil surface cannot be read back on this API. Create the depth buffer as a texture with "
        "SurfaceFormat::INTZ (or DF24/DF16 if the driver prefers those), bind it as the depth target, and read "
        "it back as a texture.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shaders
// ─────────────────────────────────────────────────────────────────────────────

Result<Shader>
D3D9ResourceStore::register_shader(uint64_t id, const ShaderBuilder& b)
{
    if (!device_) return gfx_err<Shader>(ShaderError::NotInitialized, "D3D9: no device.");
    const auto& blob = b.precompiled_bytecode();
    if (blob.size() < sizeof(DWORD)) {
        return gfx_err<Shader>(ShaderError::CompileFailed,
            "D3D9: no bytecode to make a shader from. Build shaders through ShaderBuilder::build(ctx), which "
            "compiles the source first.");
    }

    const std::string& profile = b.shader_profile();
    const auto*        dwords  = reinterpret_cast<const DWORD*>(blob.data());
    // The first token says what the bytecode is: 0xFFFE.... vertex, 0xFFFF.... pixel.
    const DWORD kind = dwords[0] & 0xFFFF0000u;
    if (kind != 0xFFFE0000u && kind != 0xFFFF0000u) {
        return gfx_err<Shader>(ShaderError::Unsupported,
            "D3D9: the bytecode is not a Direct3D 9 vertex or pixel shader (shader model 4 and later is not "
            "expressible on this API). Use a 2_0 or 3_0 profile, for example ps_3_0. The profile was '" +
            profile + "'.");
    }
    const bool is_pixel = kind == 0xFFFF0000u;

    ShaderRec rec;
    rec.stage  = is_pixel ? ShaderStage::Pixel : ShaderStage::Vertex;
    rec.major  = shader_major(dwords);
    rec.source = b;
    rec.source.bytecode(nullptr, 0);   // the source is what is kept

    InternalScope internal_work;
    const HRESULT hr = is_pixel ? device_->CreatePixelShader(dwords, &rec.ps) : device_->CreateVertexShader(dwords, &rec.vs);
    if (FAILED(hr) || (!rec.ps && !rec.vs)) {
        return gfx_err<Shader>(ShaderError::CompileFailed,
            "D3D9: the device rejected the compiled bytecode (hr=" + hex(hr) + "). The profile may exceed what "
            "this device supports -- check the vertex and pixel shader versions in D3DCAPS9.");
    }
    {
        std::lock_guard lk(mutex_);
        shaders_[id] = std::move(rec);
    }

    Shader out;
    out.handle   = ShaderHandle{ id };
    out.stage    = is_pixel ? ShaderStage::Pixel : ShaderStage::Vertex;
    out.lang     = ShaderLang::DXBC;
    out.entry    = b.entry_point();
    out.profile  = profile;
    out.bytecode = blob;
    return out;
}

void D3D9ResourceStore::destroy_shader(ShaderHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = shaders_.find(h.id); it != shaders_.end()) {
        safe_release(it->second.vs); safe_release(it->second.ps);
        safe_release(it->second.other_vs); safe_release(it->second.other_ps);
        shaders_.erase(it);
    }
}

bool D3D9ResourceStore::has_shader(ShaderHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    return shaders_.contains(h.id);
}

IDirect3DVertexShader9* D3D9ResourceStore::vertex_shader(ShaderHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = shaders_.find(h.id);
    return (it != shaders_.end()) ? it->second.vs : nullptr;
}

IDirect3DPixelShader9* D3D9ResourceStore::pixel_shader(ShaderHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = shaders_.find(h.id);
    return (it != shaders_.end()) ? it->second.ps : nullptr;
}

template <typename S>
S* D3D9ResourceStore::shader_for(ShaderHandle h, bool sm3, std::string& why, bool& first_failure)
{
    constexpr bool pixel = std::is_same_v<S, IDirect3DPixelShader9>;
    first_failure = false;
    ShaderBuilder source;
    {
        std::lock_guard lk(mutex_);
        const auto it = shaders_.find(h.id);
        if (it == shaders_.end()) { why = "the shader was destroyed"; return nullptr; }
        ShaderRec& r = it->second;
        S* base  = nullptr;
        S* other = nullptr;
        if constexpr (pixel) { base = r.ps; other = r.other_ps; } else { base = r.vs; other = r.other_vs; }
        if (!base) { why = pixel ? "it is not a pixel shader" : "it is not a vertex shader"; return nullptr; }
        if ((r.major >= 3) == sm3) return base;
        if (other) return other;
        if (r.other_failed) { why = r.other_why; return nullptr; }
        source = r.source;
    }

    // Below 3: the most capable shader model 2 profiles first.
    const char* const profiles_sm3[] = { pixel ? "ps_3_0" : "vs_3_0" };
    const char* const profiles_sm2[] = { pixel ? "ps_2_b" : "vs_2_0", pixel ? "ps_2_a" : "vs_2_a", pixel ? "ps_2_0" : "vs_1_1" };
    std::string err;
    std::vector<uint8_t> bytes;
    auto try_profiles = [&](const char* const* list, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            auto r = detail::compile_hlsl(source, list[i]);
            if (r) { bytes = std::move(*r); return; }
            if (err.empty()) err = std::string("recompiling it as ") + list[i] + " failed: " + r.error().message;
        }
    };
    if (sm3) try_profiles(profiles_sm3, std::size(profiles_sm3));
    else     try_profiles(profiles_sm2, std::size(profiles_sm2));

    S* made = nullptr;
    if (!bytes.empty()) {
        err.clear();
        InternalScope internal_work;
        const auto* code = reinterpret_cast<const DWORD*>(bytes.data());
        HRESULT hr;
        if constexpr (pixel) hr = device_->CreatePixelShader(code, &made);
        else                 hr = device_->CreateVertexShader(code, &made);
        if (FAILED(hr)) { made = nullptr; err = "the device refused the recompiled shader (hr=" + hex(hr) + ")"; }
    }

    std::lock_guard lk(mutex_);
    const auto it = shaders_.find(h.id);
    if (it == shaders_.end()) { if (made) made->Release(); why = "the shader was destroyed"; return nullptr; }
    ShaderRec& r = it->second;
    S*& slot = [&]() -> S*& { if constexpr (pixel) return r.other_ps; else return r.other_vs; }();
    if (made) {
        if (slot) made->Release(); else slot = made;   // another thread got there first
        return slot;
    }
    if (!r.other_failed) {
        r.other_failed = true;
        r.other_why    = err;
        first_failure  = true;
    }
    why = r.other_why;
    return nullptr;
}

IDirect3DVertexShader9* D3D9ResourceStore::vertex_shader_for(ShaderHandle h, bool sm3, std::string& why, bool& first_failure)
{
    return shader_for<IDirect3DVertexShader9>(h, sm3, why, first_failure);
}

IDirect3DPixelShader9* D3D9ResourceStore::pixel_shader_for(ShaderHandle h, bool sm3, std::string& why, bool& first_failure)
{
    return shader_for<IDirect3DPixelShader9>(h, sm3, why, first_failure);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Vertex and index buffer readback
//
//  D3D9 needs no staging copy and no fence: a buffer is locked in place. What
//  it needs instead is permission, and that was decided when the buffer was
//  created. D3DUSAGE_WRITEONLY tells the driver the application will never
//  read the buffer back, which frees it to put the memory somewhere the CPU
//  cannot usefully read -- so Lock either fails or hands back something that
//  is not the contents. Most games set it on static geometry; the error says
//  so rather than returning bytes that would be nonsense.
// ─────────────────────────────────────────────────────────────────────────────

void D3D9ResourceStore::adopt_vertex_buffer(uint64_t id, IDirect3DVertexBuffer9* vb, uint32_t size, uint32_t usage,
                                            uint32_t pool) noexcept
{
    if (!vb || size == 0) return;
    std::lock_guard lk(mutex_);
    if (buffers_.contains(id)) return;
    BufferRec rec; rec.vb = vb; rec.size = size; rec.usage = usage; rec.pool = pool;
    buffers_[id] = rec;
}

void D3D9ResourceStore::adopt_index_buffer(uint64_t id, IDirect3DIndexBuffer9* ib, uint32_t size, uint32_t usage,
                                           uint32_t pool) noexcept
{
    if (!ib || size == 0) return;
    std::lock_guard lk(mutex_);
    if (buffers_.contains(id)) return;
    BufferRec rec; rec.ib = ib; rec.size = size; rec.usage = usage; rec.pool = pool;
    buffers_[id] = rec;
}

Result<BufferData> D3D9ResourceStore::read_buffer(uint64_t id, uint64_t offset, uint64_t size)
{
    BufferRec rec;
    {
        std::lock_guard lk(mutex_);
        const auto it = buffers_.find(id);
        if (it == buffers_.end() || (!it->second.vb && !it->second.ib)) {
            return gfx_err<BufferData>(ShaderError::InvalidHandle,
                "D3D9: readback of a buffer this context does not know. Buffers are registered as the application "
                "creates them, so one made before VanGFX attached is not among them; D3D9 has no constant, "
                "structured, raw or UAV buffers at all.");
        }
        rec = it->second;
    }

    if (rec.usage & D3DUSAGE_WRITEONLY) {
        return gfx_err<BufferData>(ShaderError::Unsupported,
            "D3D9: this buffer was created D3DUSAGE_WRITEONLY, which tells the driver it will never be read back "
            "and lets it hold the memory somewhere the CPU cannot read. The flag is the application's choice at "
            "creation and cannot be undone from here.");
    }

    const uint64_t buf_size = rec.size;
    if (offset >= buf_size) {
        return gfx_err<BufferData>(ShaderError::InvalidFormat,
            "D3D9: offset " + std::to_string(offset) + " is past the end of a " + std::to_string(buf_size) + "-byte buffer.");
    }
    const uint64_t avail = buf_size - offset;
    if (size == 0 || size > avail) size = avail;

    InternalScope internal_work;   // the Unlock below is ours, not an update of the game's
    void* p = nullptr;
    const HRESULT hr = rec.vb
        ? rec.vb->Lock(static_cast<UINT>(offset), static_cast<UINT>(size), &p, D3DLOCK_READONLY)
        : rec.ib->Lock(static_cast<UINT>(offset), static_cast<UINT>(size), &p, D3DLOCK_READONLY);
    if (FAILED(hr) || !p) {
        return gfx_err<BufferData>(ShaderError::ReadbackFailed,
            "D3D9: Lock failed on the buffer. A D3DPOOL_DEFAULT buffer that is not D3DUSAGE_DYNAMIC cannot be "
            "locked for reading.");
    }
    scratch_.resize(static_cast<size_t>(size));
    std::memcpy(scratch_.data(), p, static_cast<size_t>(size));
    if (rec.vb) rec.vb->Unlock(); else rec.ib->Unlock();

    BufferData out;
    out.bytes       = std::span<const uint8_t>(scratch_.data(), scratch_.size());
    out.offset      = offset;
    out.buffer_size = buf_size;
    return out;
}

}  // namespace vangfx::backend::d3d9
