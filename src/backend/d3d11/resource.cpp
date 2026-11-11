/**
 * src/backend/d3d11/resource.cpp — resource and shader creation on the device,
 * and Map/Unmap on a device context.
 *
 * Hooked on ID3D11Device (every device class, once each):
 *   CreateBuffer, CreateTexture1D, CreateTexture2D, CreateTexture3D — each new
 *   resource is registered with the hub, which assigns its id and reports its
 *   destruction (through compat/lifetime.hpp: the old per-object Release patch
 *   lost Destroy events once the first-registered buffer died and restored
 *   the shared slot for everyone);
 *   CreateVertexShader, CreatePixelShader, CreateComputeShader — the bytecode
 *   hash each shader is known by in draw events.
 * Hooked on ID3D11DeviceContext (Heal::SameModule):
 *   Map, Unmap — an Update event when a known resource is unmapped.
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>
#include <d3d11_3.h>
#include <format_map.hpp>
#include <hash.hpp>
#include <vangfx/events/resource.hpp>

namespace vangfx::backend::d3d11 {

namespace dev = ::vangfx::d3d11_slots::device;
namespace dcs = ::vangfx::d3d11_slots::device_context;
using hooks::Heal;

namespace {

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CreateTex1DFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
using CreateTex2DFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateTex3DFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);
template <typename T>
using CreateShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, T**);
using UnmapFn        = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using CreateTex2D1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device3*, const D3D11_TEXTURE2D_DESC1*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D1**);
using CreateTex3D1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device3*, const D3D11_TEXTURE3D_DESC1*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D1**);
using DeferredFn     = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using Deferred1Fn    = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device1*, UINT, ID3D11DeviceContext1**);
using Deferred2Fn    = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device2*, UINT, ID3D11DeviceContext2**);
using Deferred3Fn    = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device3*, UINT, ID3D11DeviceContext3**);

hooks::Site s_create_buffer{ dev::CreateBuffer,       "ID3D11Device::CreateBuffer" };
hooks::Site s_create_tex1d { dev::CreateTexture1D,    "ID3D11Device::CreateTexture1D" };
hooks::Site s_create_tex2d { dev::CreateTexture2D,    "ID3D11Device::CreateTexture2D" };
hooks::Site s_create_tex3d { dev::CreateTexture3D,    "ID3D11Device::CreateTexture3D" };
hooks::Site s_create_vs    { dev::CreateVertexShader, "ID3D11Device::CreateVertexShader" };
hooks::Site s_create_ps    { dev::CreatePixelShader,  "ID3D11Device::CreatePixelShader" };
hooks::Site s_create_cs    { dev::CreateComputeShader,"ID3D11Device::CreateComputeShader" };
hooks::Site s_unmap        { dcs::Unmap,              "Unmap", Heal::SameModule };
hooks::Site s_create_tex2d1{ dev::CreateTexture2D1,       "ID3D11Device3::CreateTexture2D1" };
hooks::Site s_create_tex3d1{ dev::CreateTexture3D1,       "ID3D11Device3::CreateTexture3D1" };
hooks::Site s_deferred     { dev::CreateDeferredContext,  "ID3D11Device::CreateDeferredContext" };
hooks::Site s_deferred1    { dev::CreateDeferredContext1, "ID3D11Device1::CreateDeferredContext1" };
hooks::Site s_deferred2    { dev::CreateDeferredContext2, "ID3D11Device2::CreateDeferredContext2" };
hooks::Site s_deferred3    { dev::CreateDeferredContext3, "ID3D11Device3::CreateDeferredContext3" };

// What was made, in the terms the event reports.
struct Made {
    ResourceKind kind = ResourceKind::Texture;
    uint32_t width = 0, height = 0, depth = 1, mips = 1, samples = 1, size = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    TextureType type = TextureType::Tex2D;
    bool uav = false, buffer = false;
};

ResourceKind buffer_kind(UINT bind) noexcept {
    if (bind & D3D11_BIND_CONSTANT_BUFFER)   return ResourceKind::ConstantBuffer;
    if (bind & D3D11_BIND_VERTEX_BUFFER)     return ResourceKind::VertexBuffer;
    if (bind & D3D11_BIND_INDEX_BUFFER)      return ResourceKind::IndexBuffer;
    if (bind & D3D11_BIND_UNORDERED_ACCESS)  return ResourceKind::UAV;
    return ResourceKind::StructuredBuffer;   // shader-resource, staging or stream-output buffers
}

ResourceKind texture_kind(UINT bind) noexcept {
    if (bind & D3D11_BIND_DEPTH_STENCIL) return ResourceKind::DepthBuffer;
    if (bind & D3D11_BIND_RENDER_TARGET) return ResourceKind::RenderTarget;
    return ResourceKind::Texture;
}

Made describe(ID3D11Resource* res) noexcept {
    Made m;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        m.kind = buffer_kind(d.BindFlags); m.size = d.ByteWidth; m.buffer = true;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d{};
        static_cast<ID3D11Texture1D*>(res)->GetDesc(&d);
        m.kind = texture_kind(d.BindFlags); m.width = d.Width; m.mips = d.MipLevels; m.format = d.Format;
        m.type = TextureType::Tex1D; m.uav = (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        m.kind = texture_kind(d.BindFlags); m.width = d.Width; m.height = d.Height; m.depth = d.ArraySize;
        m.mips = d.MipLevels; m.format = d.Format; m.samples = d.SampleDesc.Count;
        m.type = (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? TextureType::TexCube : TextureType::Tex2D;
        m.uav = (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d{};
        static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
        m.kind = texture_kind(d.BindFlags); m.width = d.Width; m.height = d.Height; m.depth = d.Depth;
        m.mips = d.MipLevels; m.format = d.Format; m.type = TextureType::TexVolume;
        m.uav = (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        break;
    }
    default: break;
    }
    return m;
}

void set_handle(ResourceEvent& ev, uint64_t id) noexcept {
    switch (ev.kind) {
    case ResourceKind::Texture:          ev.texture         = { id }; break;
    case ResourceKind::RenderTarget:     ev.render_target   = { id }; break;
    case ResourceKind::DepthBuffer:      ev.depth_buffer    = { id }; break;
    case ResourceKind::VertexBuffer:     ev.vertex_buffer   = { id }; break;
    case ResourceKind::IndexBuffer:      ev.index_buffer    = { id }; break;
    case ResourceKind::ConstantBuffer:   ev.constant_buffer = { id }; break;
    case ResourceKind::StructuredBuffer: ev.structured_buf  = { id }; break;
    case ResourceKind::UAV:              ev.uav             = { id }; break;
    default: break;
    }
}

// Registers a resource and reports its creation to the device's contexts.
void created(ID3D11Device* device, ID3D11Resource* res) {
    if (!res || hub_idle() || internal()) return;
    HubReader reader;
    const ContextSet cs = contexts_for(device);
    if (!cs) return;
    const Made m = describe(res);
    const uint64_t id = resource_register(res, m.kind, device);

    ResourceEvent ev{};
    ev.kind   = m.kind;
    ev.action = ResourceAction::Create;
    set_handle(ev, id);
    const SurfaceFormat fmt = fmtmap::from_dxgi(m.format);
    switch (m.kind) {
    case ResourceKind::RenderTarget:
        ev.rt_desc = { m.width, m.height, fmt, m.samples };
        break;
    case ResourceKind::DepthBuffer:
        ev.db_desc.width = m.width; ev.db_desc.height = m.height; ev.db_desc.format = fmt;
        ev.db_desc.msaa_samples = m.samples; ev.db_desc.has_stencil = fmtmap::has_stencil(fmt);
        break;
    case ResourceKind::Texture:
        ev.tex_desc.width = m.width; ev.tex_desc.height = m.height; ev.tex_desc.depth = m.depth;
        ev.tex_desc.mip_levels = m.mips; ev.tex_desc.format = fmt; ev.tex_desc.msaa_samples = m.samples;
        ev.tex_desc.type = m.type; ev.tex_desc.is_uav = m.uav;
        break;
    case ResourceKind::VertexBuffer:
        ev.vb_desc.size = m.size;
        break;
    default:
        ev.sb_desc.size = m.size;
        break;
    }
    ID3D11Buffer* buf = m.buffer ? static_cast<ID3D11Buffer*>(res) : nullptr;
    for (D3D11ContextImpl* ctx : cs) {
        if (buf) ctx->store().adopt_buffer(id, buf, m.size);
        if (!ctx->wants_resource()) continue;
        D3D11FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_resource(fc, ev);
    }
}

HRESULT STDMETHODCALLTYPE hook_create_buffer(ID3D11Device* self, const D3D11_BUFFER_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    const HRESULT hr = s_create_buffer.original<CreateBufferFn>(self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out) created(self, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_create_tex1d(ID3D11Device* self, const D3D11_TEXTURE1D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture1D** out) {
    const HRESULT hr = s_create_tex1d.original<CreateTex1DFn>(self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out) created(self, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_create_tex2d(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    const HRESULT hr = s_create_tex2d.original<CreateTex2DFn>(self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out) created(self, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_create_tex3d(ID3D11Device* self, const D3D11_TEXTURE3D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture3D** out) {
    const HRESULT hr = s_create_tex3d.original<CreateTex3DFn>(self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out) created(self, *out);
    return hr;
}

// Shaders are hashed whether or not anything is attached yet: a draw names its
// shaders by these hashes, and a game creates most of them at load.
template <typename T>
HRESULT create_shader(hooks::Site& site, ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* link, T** out) {
    const HRESULT hr = site.original<CreateShaderFn<T>>(self)(self, bc, n, link, out);
    if (SUCCEEDED(hr) && out && *out && !internal()) shader_register(*out, hash_bytes(bc, n));
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_create_vs(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* link, ID3D11VertexShader** out) {
    return create_shader(s_create_vs, self, bc, n, link, out);
}
HRESULT STDMETHODCALLTYPE hook_create_ps(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* link, ID3D11PixelShader** out) {
    return create_shader(s_create_ps, self, bc, n, link, out);
}
HRESULT STDMETHODCALLTYPE hook_create_cs(ID3D11Device* self, const void* bc, SIZE_T n, ID3D11ClassLinkage* link, ID3D11ComputeShader** out) {
    return create_shader(s_create_cs, self, bc, n, link, out);
}

HRESULT STDMETHODCALLTYPE hook_create_tex2d1(ID3D11Device3* self, const D3D11_TEXTURE2D_DESC1* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D1** out) {
    const HRESULT hr = s_create_tex2d1.original<CreateTex2D1Fn>(self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out) created(self, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_create_tex3d1(ID3D11Device3* self, const D3D11_TEXTURE3D_DESC1* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture3D1** out) {
    const HRESULT hr = s_create_tex3d1.original<CreateTex3D1Fn>(self)(self, d, init, out);
    if (SUCCEEDED(hr) && out && *out) created(self, *out);
    return hr;
}

// Every device context has a vtable of its own, so each deferred context is
// hooked as it is made -- whether or not a context is attached yet, since a
// game makes its deferred contexts once, at load.
void adopt_deferred(void* out) noexcept {
    if (!out || internal()) return;
    install_context_hooks(static_cast<ID3D11DeviceContext*>(out));
}
HRESULT STDMETHODCALLTYPE hook_deferred(ID3D11Device* self, UINT flags, ID3D11DeviceContext** out) {
    const HRESULT hr = s_deferred.original<DeferredFn>(self)(self, flags, out);
    if (SUCCEEDED(hr) && out) adopt_deferred(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_deferred1(ID3D11Device1* self, UINT flags, ID3D11DeviceContext1** out) {
    const HRESULT hr = s_deferred1.original<Deferred1Fn>(self)(self, flags, out);
    if (SUCCEEDED(hr) && out) adopt_deferred(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_deferred2(ID3D11Device2* self, UINT flags, ID3D11DeviceContext2** out) {
    const HRESULT hr = s_deferred2.original<Deferred2Fn>(self)(self, flags, out);
    if (SUCCEEDED(hr) && out) adopt_deferred(*out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_deferred3(ID3D11Device3* self, UINT flags, ID3D11DeviceContext3** out) {
    const HRESULT hr = s_deferred3.original<Deferred3Fn>(self)(self, flags, out);
    if (SUCCEEDED(hr) && out) adopt_deferred(*out);
    return hr;
}

template <typename I, typename Fn>
void install_through(ID3D11Device* device, hooks::Site& site, Fn thunk) noexcept {
    I* p = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&p))) && p) {
        site.install(p, thunk);
        p->Release();
    }
}

void STDMETHODCALLTYPE hook_unmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT sub) {
    const Healer healer{ self };
    s_unmap.original<UnmapFn>(self)(self, res, sub);
    if (hub_idle() || internal() || !res) return;
    const uint64_t id = resource_id(res);
    if (!id) return;
    HubReader reader;
    DcState* st = dc_state(self);
    if (!st) return;
    ResourceEvent ev{};
    ev.kind   = describe(res).kind;
    ev.action = ResourceAction::Update;
    set_handle(ev, id);
    for (D3D11ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_resource()) continue;
        D3D11FrameContextImpl fc(*ctx, ctx->current_frame_index(), self, st);
        ctx->dispatch_resource(fc, ev);
    }
}

} // namespace

void announce_resource(ID3D11Device* device, ID3D11Resource* res) noexcept {
    created(device, res);
}

bool install_device_hooks(ID3D11Device* device) noexcept {
    bool ok = s_create_buffer.install(device, &hook_create_buffer);
    ok &= s_create_tex1d.install(device, &hook_create_tex1d);
    ok &= s_create_tex2d.install(device, &hook_create_tex2d);
    ok &= s_create_tex3d.install(device, &hook_create_tex3d);
    ok &= s_create_vs.install(device, &hook_create_vs);
    ok &= s_create_ps.install(device, &hook_create_ps);
    ok &= s_create_cs.install(device, &hook_create_cs);
    ok &= s_deferred.install(device, &hook_deferred);
    install_through<ID3D11Device1>(device, s_deferred1, &hook_deferred1);
    install_through<ID3D11Device2>(device, s_deferred2, &hook_deferred2);
    install_through<ID3D11Device3>(device, s_deferred3, &hook_deferred3);
    install_through<ID3D11Device3>(device, s_create_tex2d1, &hook_create_tex2d1);
    install_through<ID3D11Device3>(device, s_create_tex3d1, &hook_create_tex3d1);
    return ok;
}

void install_map_hooks(ID3D11DeviceContext* context) noexcept {
    s_unmap.install(context, &hook_unmap);
    register_context_site(s_unmap);
}

void uninstall_device_hooks() noexcept {
    for (hooks::Site* s : { &s_create_buffer, &s_create_tex1d, &s_create_tex2d, &s_create_tex3d,
                            &s_create_vs, &s_create_ps, &s_create_cs, &s_deferred, &s_deferred1,
                            &s_deferred2, &s_deferred3, &s_create_tex2d1, &s_create_tex3d1 })
        s->uninstall();
}

}  // namespace vangfx::backend::d3d11
