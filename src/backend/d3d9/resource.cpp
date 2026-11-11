/**
 * src/backend/d3d9/resource.cpp — resource creation on the device, and writes
 * through Unlock.
 *
 * Hooked on the device's own table (Heal::SameModule):
 *   CreateTexture, CreateVolumeTexture, CreateCubeTexture, CreateVertexBuffer,
 *   CreateIndexBuffer, CreateRenderTarget, CreateDepthStencilSurface,
 *   CreateOffscreenPlainSurface, and on an Ex device CreateRenderTargetEx,
 *   CreateOffscreenPlainSurfaceEx, CreateDepthStencilSurfaceEx -- each new
 *   resource is registered with the hub, which assigns its id and reports its
 *   destruction (compat/lifetime.hpp: IDirect3DResource9::SetPrivateData holds a
 *   sentinel the runtime releases with the resource; the old per-object Release
 *   patch was never installed, and would have recursed if it had been).
 * Hooked on the shared tables:
 *   IDirect3DVertexBuffer9::Unlock, IDirect3DIndexBuffer9::Unlock,
 *   IDirect3DTexture9::UnlockRect -- an Update event for a known resource.
 */

#include "d3d9_impl.hpp"
#include <compat/d3d9_slots.hpp>
#include <format_map.hpp>
#include <vangfx/events/resource.hpp>

namespace vangfx::backend::d3d9 {

namespace s  = ::vangfx::d3d9_slots;
namespace sx = ::vangfx::d3d9_slots::device_ex;
using hooks::Heal;

namespace {

using CreateTexFn    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                   IDirect3DTexture9**, HANDLE*);
using CreateVolFn    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                   IDirect3DVolumeTexture9**, HANDLE*);
using CreateCubeFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                   IDirect3DCubeTexture9**, HANDLE*);
using CreateVBFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                                   IDirect3DVertexBuffer9**, HANDLE*);
using CreateIBFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                   IDirect3DIndexBuffer9**, HANDLE*);
using CreateRTFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD,
                                                   BOOL, IDirect3DSurface9**, HANDLE*);
using CreateDSFn     = CreateRTFn;   // same shape: Discard in place of Lockable
using CreatePlainFn  = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL,
                                                   IDirect3DSurface9**, HANDLE*);
using CreateRTExFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD,
                                                   BOOL, IDirect3DSurface9**, HANDLE*, DWORD);
using CreateDSExFn   = CreateRTExFn;
using CreatePlainExFn= HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, UINT, UINT, D3DFORMAT, D3DPOOL,
                                                   IDirect3DSurface9**, HANDLE*, DWORD);
using UnlockFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DResource9*);
using UnlockRectFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT);

hooks::Site s_create_tex     { s::CreateTexture,               "CreateTexture",               Heal::SameModule };
hooks::Site s_create_vol     { s::CreateVolumeTexture,         "CreateVolumeTexture",         Heal::SameModule };
hooks::Site s_create_cube    { s::CreateCubeTexture,           "CreateCubeTexture",           Heal::SameModule };
hooks::Site s_create_vb      { s::CreateVertexBuffer,          "CreateVertexBuffer",          Heal::SameModule };
hooks::Site s_create_ib      { s::CreateIndexBuffer,           "CreateIndexBuffer",           Heal::SameModule };
hooks::Site s_create_rt      { s::CreateRenderTarget,          "CreateRenderTarget",          Heal::SameModule };
hooks::Site s_create_ds      { s::CreateDepthStencilSurface,   "CreateDepthStencilSurface",   Heal::SameModule };
hooks::Site s_create_plain   { s::CreateOffscreenPlainSurface, "CreateOffscreenPlainSurface", Heal::SameModule };
hooks::Site s_create_rt_ex   { sx::CreateRenderTargetEx,          "CreateRenderTargetEx",          Heal::SameModule };
hooks::Site s_create_ds_ex   { sx::CreateDepthStencilSurfaceEx,   "CreateDepthStencilSurfaceEx",   Heal::SameModule };
hooks::Site s_create_plain_ex{ sx::CreateOffscreenPlainSurfaceEx, "CreateOffscreenPlainSurfaceEx", Heal::SameModule };
hooks::Site s_vb_unlock      { s::vertex_buffer::Unlock, "IDirect3DVertexBuffer9::Unlock" };
hooks::Site s_ib_unlock      { s::index_buffer::Unlock,  "IDirect3DIndexBuffer9::Unlock" };
hooks::Site s_tex_unlock     { s::texture::UnlockRect,   "IDirect3DTexture9::UnlockRect" };

// What a resource is, in the terms the event reports.
struct Made {
    ResourceKind  kind    = ResourceKind::Texture;
    TextureType   type    = TextureType::Tex2D;
    SurfaceFormat format  = SurfaceFormat::Unknown;
    uint32_t      width = 0, height = 0, depth = 1, mips = 1, samples = 1, size = 0;
    DWORD         usage   = 0;
    D3DPOOL       pool    = D3DPOOL_DEFAULT;
    bool          ok      = false;
};

uint32_t samples_of(D3DMULTISAMPLE_TYPE t) noexcept {
    // NONMASKABLE is multisampled with a count only the quality level implies.
    return t == D3DMULTISAMPLE_NONE ? 1u : t == D3DMULTISAMPLE_NONMASKABLE ? 2u : static_cast<uint32_t>(t);
}

ResourceKind kind_of_usage(DWORD usage) noexcept {
    if (usage & D3DUSAGE_DEPTHSTENCIL) return ResourceKind::DepthBuffer;
    if (usage & D3DUSAGE_RENDERTARGET) return ResourceKind::RenderTarget;
    return ResourceKind::Texture;
}

void from_surface_desc(Made& m, const D3DSURFACE_DESC& d) noexcept {
    m.format  = fmtmap::from_d3d9(d.Format);
    m.width   = d.Width;
    m.height  = d.Height;
    m.samples = samples_of(d.MultiSampleType);
    m.usage   = d.Usage;
    m.pool    = d.Pool;
    m.kind    = kind_of_usage(d.Usage);
    m.ok      = true;
}

Made describe(IDirect3DResource9* res) noexcept {
    Made m;
    switch (res->GetType()) {
    case D3DRTYPE_SURFACE: {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DSurface9*>(res)->GetDesc(&d))) from_surface_desc(m, d);
        break;
    }
    case D3DRTYPE_TEXTURE: {
        auto* t = static_cast<IDirect3DTexture9*>(res);
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(t->GetLevelDesc(0, &d))) from_surface_desc(m, d);
        m.mips = t->GetLevelCount();
        break;
    }
    case D3DRTYPE_CUBETEXTURE: {
        auto* t = static_cast<IDirect3DCubeTexture9*>(res);
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(t->GetLevelDesc(0, &d))) from_surface_desc(m, d);
        m.type = TextureType::TexCube;
        m.mips = t->GetLevelCount();
        break;
    }
    case D3DRTYPE_VOLUMETEXTURE: {
        auto* t = static_cast<IDirect3DVolumeTexture9*>(res);
        D3DVOLUME_DESC d{};
        if (SUCCEEDED(t->GetLevelDesc(0, &d))) {
            m.format = fmtmap::from_d3d9(d.Format);
            m.width = d.Width; m.height = d.Height; m.depth = d.Depth;
            m.usage = d.Usage; m.pool = d.Pool; m.ok = true;
        }
        m.kind = ResourceKind::Texture;
        m.type = TextureType::TexVolume;
        m.mips = t->GetLevelCount();
        break;
    }
    case D3DRTYPE_VERTEXBUFFER: {
        D3DVERTEXBUFFER_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DVertexBuffer9*>(res)->GetDesc(&d))) {
            m.size = d.Size; m.usage = d.Usage; m.pool = d.Pool; m.ok = true;
        }
        m.kind = ResourceKind::VertexBuffer;
        break;
    }
    case D3DRTYPE_INDEXBUFFER: {
        D3DINDEXBUFFER_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DIndexBuffer9*>(res)->GetDesc(&d))) {
            m.size = d.Size; m.usage = d.Usage; m.pool = d.Pool; m.ok = true;
        }
        m.kind = ResourceKind::IndexBuffer;
        break;
    }
    default: break;
    }
    return m;
}

void set_handle(ResourceEvent& ev, uint64_t id) noexcept {
    switch (ev.kind) {
    case ResourceKind::Texture:      ev.texture       = { id }; break;
    case ResourceKind::RenderTarget: ev.render_target = { id }; break;
    case ResourceKind::DepthBuffer:  ev.depth_buffer  = { id }; break;
    case ResourceKind::VertexBuffer: ev.vertex_buffer = { id }; break;
    case ResourceKind::IndexBuffer:  ev.index_buffer  = { id }; break;
    default: break;
    }
}

// Registers a resource and reports its creation to the device's contexts.
void created(IDirect3DDevice9* device, IDirect3DResource9* res) {
    if (!res || hub_idle() || internal()) return;
    HubReader reader;
    const ContextSet cs = contexts_for(device);
    if (!cs) return;
    const Made m = describe(res);
    if (!m.ok) return;
    const uint64_t id = resource_register(res, m.kind, device);

    ResourceEvent ev{};
    ev.kind   = m.kind;
    ev.action = ResourceAction::Create;
    set_handle(ev, id);
    const bool dynamic = (m.usage & D3DUSAGE_DYNAMIC) != 0;
    switch (m.kind) {
    case ResourceKind::VertexBuffer:
        ev.vb_desc.size = m.size; ev.vb_desc.is_dynamic = dynamic;
        break;
    case ResourceKind::IndexBuffer:
        ev.sb_desc.size = m.size;
        break;
    default:
        // Every texture-like resource carries a TextureDesc; render targets and
        // depth buffers the desc of their kind as well.
        ev.tex_desc.type = m.type; ev.tex_desc.format = m.format;
        ev.tex_desc.width = m.width; ev.tex_desc.height = m.height; ev.tex_desc.depth = m.depth;
        ev.tex_desc.mip_levels = m.mips; ev.tex_desc.msaa_samples = m.samples;
        ev.tex_desc.is_render_target = m.kind == ResourceKind::RenderTarget;
        ev.tex_desc.is_depth = m.kind == ResourceKind::DepthBuffer;
        ev.tex_desc.is_dynamic = dynamic;
        if (m.kind == ResourceKind::RenderTarget) ev.rt_desc = { m.width, m.height, m.format, m.samples };
        if (m.kind == ResourceKind::DepthBuffer) {
            ev.db_desc.width = m.width; ev.db_desc.height = m.height; ev.db_desc.format = m.format;
            ev.db_desc.msaa_samples = m.samples; ev.db_desc.has_stencil = fmtmap::has_stencil(m.format);
        }
        break;
    }
    for (D3D9ContextImpl* ctx : cs) {
        if (m.kind == ResourceKind::VertexBuffer)
            ctx->store().adopt_vertex_buffer(id, static_cast<IDirect3DVertexBuffer9*>(res), m.size, m.usage, m.pool);
        else if (m.kind == ResourceKind::IndexBuffer)
            ctx->store().adopt_index_buffer(id, static_cast<IDirect3DIndexBuffer9*>(res), m.size, m.usage, m.pool);
        if (!ctx->wants_resource()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_resource(fc, ev);
    }
}

template <typename Out>
HRESULT after_create(IDirect3DDevice9* self, HRESULT hr, Out** out) {
    if (SUCCEEDED(hr) && out && *out) created(self, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_tex(IDirect3DDevice9* self, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                          D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_tex.original<CreateTexFn>(self)(self, w, h, levels, usage, fmt, pool, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_vol(IDirect3DDevice9* self, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                                          D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_vol.original<CreateVolFn>(self)(self, w, h, d, levels, usage, fmt, pool, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_cube(IDirect3DDevice9* self, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt,
                                           D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_cube.original<CreateCubeFn>(self)(self, edge, levels, usage, fmt, pool, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_vb(IDirect3DDevice9* self, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool,
                                         IDirect3DVertexBuffer9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_vb.original<CreateVBFn>(self)(self, size, usage, fvf, pool, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_ib(IDirect3DDevice9* self, UINT size, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                         IDirect3DIndexBuffer9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_ib.original<CreateIBFn>(self)(self, size, usage, fmt, pool, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_rt(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                         DWORD quality, BOOL lockable, IDirect3DSurface9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_rt.original<CreateRTFn>(self)(self, w, h, fmt, ms, quality, lockable, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_ds(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                         DWORD quality, BOOL discard, IDirect3DSurface9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_ds.original<CreateDSFn>(self)(self, w, h, fmt, ms, quality, discard, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_plain(IDirect3DDevice9* self, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                            IDirect3DSurface9** out, HANDLE* shared) {
    const Healer healer{ self };
    return after_create(self, s_create_plain.original<CreatePlainFn>(self)(self, w, h, fmt, pool, out, shared), out);
}

HRESULT STDMETHODCALLTYPE hook_create_rt_ex(IDirect3DDevice9Ex* self, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                            DWORD quality, BOOL lockable, IDirect3DSurface9** out, HANDLE* shared, DWORD usage) {
    const Healer healer{ self };
    return after_create<IDirect3DSurface9>(self,
        s_create_rt_ex.original<CreateRTExFn>(self)(self, w, h, fmt, ms, quality, lockable, out, shared, usage), out);
}

HRESULT STDMETHODCALLTYPE hook_create_ds_ex(IDirect3DDevice9Ex* self, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                            DWORD quality, BOOL discard, IDirect3DSurface9** out, HANDLE* shared, DWORD usage) {
    const Healer healer{ self };
    return after_create<IDirect3DSurface9>(self,
        s_create_ds_ex.original<CreateDSExFn>(self)(self, w, h, fmt, ms, quality, discard, out, shared, usage), out);
}

HRESULT STDMETHODCALLTYPE hook_create_plain_ex(IDirect3DDevice9Ex* self, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                               IDirect3DSurface9** out, HANDLE* shared, DWORD usage) {
    const Healer healer{ self };
    return after_create<IDirect3DSurface9>(self,
        s_create_plain_ex.original<CreatePlainExFn>(self)(self, w, h, fmt, pool, out, shared, usage), out);
}

// ── Writes ───────────────────────────────────────────────────────────────────

void updated(IDirect3DResource9* res) {
    if (hub_idle() || internal()) return;
    uint64_t id = 0;
    ResourceKind kind{};
    if (!resource_lookup(res, id, kind)) return;
    IDirect3DDevice9* device = nullptr;
    if (FAILED(res->GetDevice(&device)) || !device) return;
    device->Release();   // the resource holds it
    HubReader reader;
    ResourceEvent ev{};
    ev.kind   = kind;
    ev.action = ResourceAction::Update;
    set_handle(ev, id);
    for (D3D9ContextImpl* ctx : contexts_for(device)) {
        if (!ctx->wants_resource()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_resource(fc, ev);
    }
}

HRESULT STDMETHODCALLTYPE hook_vb_unlock(IDirect3DResource9* self) {
    const HRESULT hr = s_vb_unlock.original<UnlockFn>(self)(self);
    if (SUCCEEDED(hr)) updated(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_ib_unlock(IDirect3DResource9* self) {
    const HRESULT hr = s_ib_unlock.original<UnlockFn>(self)(self);
    if (SUCCEEDED(hr)) updated(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_tex_unlock(IDirect3DTexture9* self, UINT level) {
    const HRESULT hr = s_tex_unlock.original<UnlockRectFn>(self)(self, level);
    if (SUCCEEDED(hr)) updated(self);
    return hr;
}

// The buffer and texture classes are hooked through objects made for the
// purpose, in system memory (the one pool every device, Ex or not, accepts).
void prime_class_hooks(IDirect3DDevice9* device) noexcept {
    InternalScope internal_work;
    IDirect3DVertexBuffer9* vb = nullptr;
    if (SUCCEEDED(device->CreateVertexBuffer(64, 0, 0, D3DPOOL_SYSTEMMEM, &vb, nullptr)) && vb) {
        s_vb_unlock.install(vb, &hook_vb_unlock);
        vb->Release();
    }
    IDirect3DIndexBuffer9* ib = nullptr;
    if (SUCCEEDED(device->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_SYSTEMMEM, &ib, nullptr)) && ib) {
        s_ib_unlock.install(ib, &hook_ib_unlock);
        ib->Release();
    }
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &tex, nullptr)) && tex) {
        s_tex_unlock.install(tex, &hook_tex_unlock);
        tex->Release();
    }
    register_class_site(s_vb_unlock);
    register_class_site(s_ib_unlock);
    register_class_site(s_tex_unlock);
}

} // namespace

void announce_resource(IDirect3DDevice9* device, IDirect3DResource9* res) noexcept {
    created(device, res);
}

void install_resource_hooks(IDirect3DDevice9* device, bool ex) noexcept {
    s_create_tex.install(device, &hook_create_tex);
    s_create_vol.install(device, &hook_create_vol);
    s_create_cube.install(device, &hook_create_cube);
    s_create_vb.install(device, &hook_create_vb);
    s_create_ib.install(device, &hook_create_ib);
    s_create_rt.install(device, &hook_create_rt);
    s_create_ds.install(device, &hook_create_ds);
    s_create_plain.install(device, &hook_create_plain);
    for (hooks::Site* site : { &s_create_tex, &s_create_vol, &s_create_cube, &s_create_vb, &s_create_ib,
                               &s_create_rt, &s_create_ds, &s_create_plain })
        register_device_site(*site);
    if (ex) {
        s_create_rt_ex.install(device, &hook_create_rt_ex);
        s_create_ds_ex.install(device, &hook_create_ds_ex);
        s_create_plain_ex.install(device, &hook_create_plain_ex);
        register_ex_device_site(s_create_rt_ex);
        register_ex_device_site(s_create_ds_ex);
        register_ex_device_site(s_create_plain_ex);
    }
    prime_class_hooks(device);
}

}  // namespace vangfx::backend::d3d9
