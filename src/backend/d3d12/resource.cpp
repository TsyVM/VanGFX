/**
 * src/backend/d3d12/resource.cpp — resource creation.
 *
 * Hooked on ID3D12Device and its extensions (each through an interface pointer
 * that really implements it; see command.cpp on slots past the base table):
 *   CreateCommittedResource, CreatePlacedResource, CreateReservedResource,
 *   CreateCommittedResource1 / CreateReservedResource1 (ID3D12Device4),
 *   CreateCommittedResource2 / CreatePlacedResource1   (ID3D12Device8),
 *   CreateCommittedResource3 / CreatePlacedResource2 /
 *   CreateReservedResource2                            (ID3D12Device10).
 *
 * Each new resource is registered with the hub, which assigns its id and
 * reports its destruction (through compat/lifetime.hpp: the old Release hook
 * is what overflowed the stack on the second buffer). Buffers are handed to
 * every listening context's store so readback and buffer_at() can reach them.
 *
 * ResourceKind is a guess for buffers: D3D12 cannot tell a vertex buffer from a
 * constant buffer at creation, so an unflagged buffer is reported as a
 * ConstantBuffer, one allowing unordered access as a UAV. The handle works with
 * any of the buffer readback overloads whichever kind it was reported as.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <format_map.hpp>
#include <vangfx/events/resource.hpp>

namespace vangfx::backend::d3d12 {

using namespace ::vangfx::d3d12;
namespace dev = ::vangfx::d3d12_slots::device;

namespace {

// The SDK does not define aliases for these extension signatures; each
// forwards to the original with every argument untouched.
using CommittedFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, REFIID, void**);
using PlacedFn     = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                 D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using ReservedFn   = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, REFIID, void**);
using Committed1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using Reserved1Fn  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using Committed2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using Placed1Fn    = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
                                                 D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using Committed3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                 const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                                 UINT32, const DXGI_FORMAT*, REFIID, void**);
using Placed2Fn    = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
                                                 D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32,
                                                 const DXGI_FORMAT*, REFIID, void**);
using Reserved2Fn  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_RESOURCE_DESC*, D3D12_BARRIER_LAYOUT,
                                                 const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                                 UINT32, const DXGI_FORMAT*, REFIID, void**);

hooks::Site s_committed { dev::CreateCommittedResource,  "ID3D12Device::CreateCommittedResource" };
hooks::Site s_placed    { dev::CreatePlacedResource,     "ID3D12Device::CreatePlacedResource" };
hooks::Site s_reserved  { dev::CreateReservedResource,   "ID3D12Device::CreateReservedResource" };
hooks::Site s_committed1{ dev::CreateCommittedResource1, "ID3D12Device4::CreateCommittedResource1" };
hooks::Site s_reserved1 { dev::CreateReservedResource1,  "ID3D12Device4::CreateReservedResource1" };
hooks::Site s_committed2{ dev::CreateCommittedResource2, "ID3D12Device8::CreateCommittedResource2" };
hooks::Site s_placed1   { dev::CreatePlacedResource1,    "ID3D12Device8::CreatePlacedResource1" };
hooks::Site s_committed3{ dev::CreateCommittedResource3, "ID3D12Device10::CreateCommittedResource3" };
hooks::Site s_placed2   { dev::CreatePlacedResource2,    "ID3D12Device10::CreatePlacedResource2" };
hooks::Site s_reserved2 { dev::CreateReservedResource2,  "ID3D12Device10::CreateReservedResource2" };

HeapType to_heap_type(D3D12_HEAP_TYPE t) noexcept {
    switch (t) {
    case D3D12_HEAP_TYPE_DEFAULT:  return HeapType::Default;
    case D3D12_HEAP_TYPE_UPLOAD:   return HeapType::Upload;
    case D3D12_HEAP_TYPE_READBACK: return HeapType::Readback;
    default:                       return HeapType::Custom;
    }
}

// What the creation call said, whichever of the ten entry points it came through.
struct Creation {
    D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    D3D12_RESOURCE_FLAGS     flags     = D3D12_RESOURCE_FLAG_NONE;
    DXGI_FORMAT              format    = DXGI_FORMAT_UNKNOWN;
    uint64_t                 width     = 0;
    uint32_t                 height    = 0;
    uint16_t                 depth     = 1;
    uint16_t                 mips      = 1;
    uint32_t                 samples   = 1;
    uint64_t                 alignment = 0;
    D3D12_TEXTURE_LAYOUT     layout    = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const D3D12_HEAP_PROPERTIES* heap_props = nullptr;
    D3D12_HEAP_FLAGS             heap_flags = D3D12_HEAP_FLAG_NONE;
    ID3D12Heap*                  heap       = nullptr;
    uint64_t                     heap_offset = 0;
    uint32_t                     initial_state = 0;
    uint32_t                     initial_layout = 0;    // D3D12_BARRIER_LAYOUT, for the creations that take one
    bool                         has_layout = false;
    bool                         state_known = true;    // false: met after its creation (announce_resource)
    ResourceAllocationKind       kind = ResourceAllocationKind::Committed;

    template <typename Desc>
    void from(const Desc& d) noexcept {
        dimension = d.Dimension; flags = d.Flags; format = d.Format;
        width = d.Width; height = d.Height; depth = d.DepthOrArraySize; mips = d.MipLevels;
        samples = d.SampleDesc.Count; alignment = d.Alignment; layout = d.Layout;
    }
};

ResourceKind classify(const Creation& c) noexcept {
    const bool buffer = c.dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    if (buffer && (c.flags & D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE)) return ResourceKind::AccelStructure;
    if (c.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)                          return ResourceKind::DepthBuffer;
    if (!buffer && (c.flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))             return ResourceKind::RenderTarget;
    if (c.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)                       return ResourceKind::UAV;
    if (buffer)                                                                     return ResourceKind::ConstantBuffer;
    return ResourceKind::Texture;
}

void fill_descs(ResourceEvent& ev, const Creation& c) noexcept {
    const SurfaceFormat fmt = fmtmap::from_dxgi(c.format);
    switch (ev.kind) {
    case ResourceKind::RenderTarget:
        ev.rt_desc.format = fmt; ev.rt_desc.width = static_cast<uint32_t>(c.width);
        ev.rt_desc.height = c.height; ev.rt_desc.msaa_samples = c.samples;
        break;
    case ResourceKind::DepthBuffer:
        ev.db_desc.format = fmt; ev.db_desc.width = static_cast<uint32_t>(c.width);
        ev.db_desc.height = c.height; ev.db_desc.msaa_samples = c.samples;
        ev.db_desc.has_stencil = c.format == DXGI_FORMAT_D24_UNORM_S8_UINT || c.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                                 c.format == DXGI_FORMAT_R24G8_TYPELESS   || c.format == DXGI_FORMAT_R32G8X24_TYPELESS;
        break;
    case ResourceKind::Texture:
    case ResourceKind::UAV:
        if (c.dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            ev.sb_desc.size = static_cast<uint32_t>(c.width);
            break;
        }
        ev.tex_desc.format = fmt; ev.tex_desc.width = static_cast<uint32_t>(c.width);
        ev.tex_desc.height = c.height; ev.tex_desc.depth = c.depth; ev.tex_desc.mip_levels = c.mips;
        ev.tex_desc.msaa_samples = c.samples;
        ev.tex_desc.is_uav = (c.flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        ev.tex_desc.type = c.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? TextureType::TexVolume
                         : c.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? TextureType::Tex1D
                                                                             : TextureType::Tex2D;
        break;
    default:
        ev.sb_desc.size = static_cast<uint32_t>(c.width);
        break;
    }
}

// Called with every successful creation. `out` is what the application asked
// for, which may be an interface other than ID3D12Resource.
void created(ID3D12Device* device, const Creation& c, void* out) {
    if (!out || hub_idle() || internal()) return;
    ID3D12Resource* res = nullptr;
    if (FAILED(static_cast<IUnknown*>(out)->QueryInterface(IID_PPV_ARGS(&res))) || !res) return;
    res->Release();   // the application holds it

    HubReader reader;
    const ContextSet cs = contexts_for(device);
    if (!cs) return;

    const ResourceKind kind = classify(c);
    const uint64_t id = resource_register(res, kind, device);
    if (c.has_layout)          state_set(res, { c.initial_layout, true, true });
    else if (c.state_known)    state_set(res, { c.initial_state, false, true });
    const bool is_buffer = c.dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    const uint64_t va = (is_buffer && c.kind != ResourceAllocationKind::Reserved) ? res->GetGPUVirtualAddress() : 0;

    ResourceEvent ev{};
    ev.kind   = kind;
    ev.action = ResourceAction::Create;
    set_resource_handle(ev, id);
    fill_descs(ev, c);
    auto& d = ev.d3d12_desc;
    d.dimension = c.dimension == D3D12_RESOURCE_DIMENSION_BUFFER    ? ResourceDimension::Buffer
                : c.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? ResourceDimension::Texture1D
                : c.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? ResourceDimension::Texture2D
                : c.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? ResourceDimension::Texture3D
                                                                    : ResourceDimension::Unknown;
    d.flags           = static_cast<ResourceFlags>(static_cast<uint16_t>(c.flags));
    d.allocation_kind = c.kind;
    d.initial_state   = static_cast<ResourceState>(c.initial_state);
    d.alignment       = c.alignment;
    d.layout          = static_cast<uint16_t>(c.layout);
    d.heap_offset     = c.heap_offset;
    d.gpu_virtual_address = va;
    if (c.heap) {
        d.heap_id = id_of(c.heap);
        const D3D12_HEAP_DESC hd = c.heap->GetDesc();
        d.heap_type  = to_heap_type(hd.Properties.Type);
        d.heap_flags = static_cast<HeapFlags>(static_cast<uint32_t>(hd.Flags));
    } else if (c.heap_props) {
        d.heap_type  = to_heap_type(c.heap_props->Type);
        d.heap_flags = static_cast<HeapFlags>(static_cast<uint32_t>(c.heap_flags));
    }

    for (D3D12ContextImpl* ctx : cs) {
        if (is_buffer) ctx->store().adopt_buffer(id, res, c.width, va);
        if (!ctx->wants_resource()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_resource(fc, ev);
    }
}

// ── Thunks ───────────────────────────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE hook_committed(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                         const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                         const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const HRESULT hr = s_committed.original<CommittedFn>(self)(self, props, flags, desc, state, clear, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.heap_props = props; c.heap_flags = flags; c.initial_state = state;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_placed(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const HRESULT hr = s_placed.original<PlacedFn>(self)(self, heap, offset, desc, state, clear, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.heap = heap; c.heap_offset = offset; c.initial_state = state;
        c.kind = ResourceAllocationKind::Placed;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_reserved(ID3D12Device* self, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                        const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const HRESULT hr = s_reserved.original<ReservedFn>(self)(self, desc, state, clear, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.initial_state = state; c.kind = ResourceAllocationKind::Reserved;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_committed1(ID3D12Device4* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                          const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                          const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                          REFIID riid, void** out) {
    const HRESULT hr = s_committed1.original<Committed1Fn>(self)(self, props, flags, desc, state, clear, session, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.heap_props = props; c.heap_flags = flags; c.initial_state = state;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_reserved1(ID3D12Device4* self, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                         const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                         REFIID riid, void** out) {
    const HRESULT hr = s_reserved1.original<Reserved1Fn>(self)(self, desc, state, clear, session, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.initial_state = state; c.kind = ResourceAllocationKind::Reserved;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_committed2(ID3D12Device8* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                          const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state,
                                          const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                          REFIID riid, void** out) {
    const HRESULT hr = s_committed2.original<Committed2Fn>(self)(self, props, flags, desc, state, clear, session, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.heap_props = props; c.heap_flags = flags; c.initial_state = state;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_placed1(ID3D12Device8* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
                                       D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
    const HRESULT hr = s_placed1.original<Placed1Fn>(self)(self, heap, offset, desc, state, clear, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.heap = heap; c.heap_offset = offset; c.initial_state = state;
        c.kind = ResourceAllocationKind::Placed;
        created(self, c, *out);
    }
    return hr;
}

// Enhanced-barrier creation takes a layout, not a state; the event reports
// Common for these, which is what a buffer created this way starts in.
HRESULT STDMETHODCALLTYPE hook_committed3(ID3D12Device10* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                          const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                          const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                          UINT32 n_formats, const DXGI_FORMAT* formats, REFIID riid, void** out) {
    const HRESULT hr = s_committed3.original<Committed3Fn>(self)(self, props, flags, desc, layout, clear, session,
                                                                 n_formats, formats, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.heap_props = props; c.heap_flags = flags; c.initial_layout = static_cast<uint32_t>(layout); c.has_layout = true;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_placed2(ID3D12Device10* self, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC1* desc,
                                       D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear,
                                       UINT32 n_formats, const DXGI_FORMAT* formats, REFIID riid, void** out) {
    const HRESULT hr = s_placed2.original<Placed2Fn>(self)(self, heap, offset, desc, layout, clear, n_formats, formats, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.heap = heap; c.heap_offset = offset; c.kind = ResourceAllocationKind::Placed;
        c.initial_layout = static_cast<uint32_t>(layout); c.has_layout = true;
        created(self, c, *out);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_reserved2(ID3D12Device10* self, const D3D12_RESOURCE_DESC* desc, D3D12_BARRIER_LAYOUT layout,
                                         const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session,
                                         UINT32 n_formats, const DXGI_FORMAT* formats, REFIID riid, void** out) {
    const HRESULT hr = s_reserved2.original<Reserved2Fn>(self)(self, desc, layout, clear, session, n_formats, formats, riid, out);
    if (SUCCEEDED(hr) && desc && out) {
        Creation c; c.from(*desc); c.kind = ResourceAllocationKind::Reserved; c.initial_layout = static_cast<uint32_t>(layout); c.has_layout = true;
        created(self, c, *out);
    }
    return hr;
}

template <typename I, typename Fn>
void install_through(ID3D12Device* device, hooks::Site& site, Fn thunk) noexcept {
    I* p = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&p))) && p) {
        site.install(p, thunk);
        p->Release();
    }
}

} // namespace

void announce_resource(ID3D12Device* device, ID3D12Resource* res) noexcept {
    if (!res) return;
    Creation c;
    c.from(res->GetDesc());
    c.state_known = false;
    D3D12_HEAP_PROPERTIES props{};
    D3D12_HEAP_FLAGS      flags = D3D12_HEAP_FLAG_NONE;
    if (SUCCEEDED(res->GetHeapProperties(&props, &flags))) {
        c.heap_props = &props;
        c.heap_flags = flags;
    } else {
        c.kind = ResourceAllocationKind::Reserved;   // the one kind with no heap
    }
    created(device, c, res);
}

void install_resource_hooks(ID3D12Device* device) noexcept {
    s_committed.install(device, &hook_committed);
    s_placed.install(device, &hook_placed);
    s_reserved.install(device, &hook_reserved);
    install_through<ID3D12Device4>(device, s_committed1, &hook_committed1);
    install_through<ID3D12Device4>(device, s_reserved1, &hook_reserved1);
    install_through<ID3D12Device8>(device, s_committed2, &hook_committed2);
    install_through<ID3D12Device8>(device, s_placed1, &hook_placed1);
    install_through<ID3D12Device10>(device, s_committed3, &hook_committed3);
    install_through<ID3D12Device10>(device, s_placed2, &hook_placed2);
    install_through<ID3D12Device10>(device, s_reserved2, &hook_reserved2);
}

void uninstall_resource_hooks() noexcept {
    for (hooks::Site* s : { &s_committed, &s_placed, &s_reserved, &s_committed1, &s_reserved1,
                            &s_committed2, &s_placed1, &s_committed3, &s_placed2, &s_reserved2 })
        s->uninstall();
}

} // namespace vangfx::backend::d3d12
