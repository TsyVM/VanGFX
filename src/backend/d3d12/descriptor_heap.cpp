/**
 * src/backend/d3d12/descriptor_heap.cpp — descriptor heaps and the views that
 * relate a descriptor to a resource.
 *
 * Hooked on ID3D12Device (every device class, once each):
 *   CreateDescriptorHeap    — records the heap's CPU range, so a descriptor
 *                             handle can be resolved to (heap, index).
 *   CreateRenderTargetView,
 *   CreateDepthStencilView  — the only place D3D12 shows which resource a
 *                             descriptor describes; OMSetRenderTargets reports
 *                             targets by resource id through what these record.
 *
 * Root signatures, memory heaps and query heaps need no creation hook: the hub
 * gives any D3D12 object an id the first time one is asked for (id_of), which
 * also covers objects created before VanGFX attached.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <vangfx/d3d12/heap.hpp>

namespace vangfx::backend::d3d12 {

using namespace ::vangfx::d3d12;
namespace dev = ::vangfx::d3d12_slots::device;

namespace {

using CreateHeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using CreateRtvFn  = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateDsvFn  = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);

hooks::Site s_create_heap{ dev::CreateDescriptorHeap,   "ID3D12Device::CreateDescriptorHeap" };
hooks::Site s_create_rtv { dev::CreateRenderTargetView, "ID3D12Device::CreateRenderTargetView" };
hooks::Site s_create_dsv { dev::CreateDepthStencilView, "ID3D12Device::CreateDepthStencilView" };

DescriptorHeapType to_heap_type(D3D12_DESCRIPTOR_HEAP_TYPE t) noexcept {
    switch (t) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return DescriptorHeapType::Sampler;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:     return DescriptorHeapType::Rtv;
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:     return DescriptorHeapType::Dsv;
    default:                                 return DescriptorHeapType::CbvSrvUav;
    }
}

HRESULT STDMETHODCALLTYPE hook_create_heap(ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    const HRESULT hr = s_create_heap.original<CreateHeapFn>(self)(self, desc, riid, out);
    if (FAILED(hr) || !desc || !out || !*out || hub_idle() || internal()) return hr;

    ID3D12DescriptorHeap* heap = nullptr;
    if (FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&heap))) || !heap) return hr;
    heap->Release();   // the application holds it

    DescriptorHeapRecord rec{};
    rec.id                   = id_of(heap);
    rec.desc.type            = to_heap_type(desc->Type);
    rec.desc.flags           = (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? DescriptorHeapFlags::ShaderVisible
                                                                                          : DescriptorHeapFlags::None;
    rec.desc.num_descriptors = desc->NumDescriptors;
    rec.desc.node_mask       = desc->NodeMask;
    rec.cpu_base             = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        rec.gpu_base = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    rec.increment            = descriptor_increment(self, desc->Type);
    descriptor_heap_register(heap, rec);
    return hr;
}

// Records descriptor → resource, and makes the resource readable by handle on
// every context listening: a render target is left in RENDER_TARGET between
// binds and a depth buffer in DEPTH_WRITE, which is what readback transitions
// from and back to.
void view_created(ID3D12Device* self, ID3D12Resource* res, uint64_t cpu, bool depth, DXGI_FORMAT format) {
    if (!cpu || hub_idle() || internal()) return;
    if (res && format == DXGI_FORMAT_UNKNOWN) format = res->GetDesc().Format;
    view_bind(cpu, res, format);
    if (!res) return;

    HubReader reader;
    const ContextSet cs = contexts_for(self);
    if (!cs) return;
    uint64_t id = resource_id(res);
    if (!id) {
        // Made before VanGFX attached, or by DXGI (swap chain buffers never pass
        // through the device's creation methods): report it now, as the moment
        // VanGFX learned of it.
        announce_resource(self, res);
        id = resource_id(res);
    }
    for (D3D12ContextImpl* ctx : cs) {
        if (depth) ctx->store().adopt_depth_buffer(id, res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        else       ctx->store().adopt_render_target(id, res, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

void STDMETHODCALLTYPE hook_create_rtv(ID3D12Device* self, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    s_create_rtv.original<CreateRtvFn>(self)(self, res, desc, dest);
    view_created(self, res, dest.ptr, false, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
}

void STDMETHODCALLTYPE hook_create_dsv(ID3D12Device* self, ID3D12Resource* res, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    s_create_dsv.original<CreateDsvFn>(self)(self, res, desc, dest);
    view_created(self, res, dest.ptr, true, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
}

} // namespace

void install_descriptor_hooks(ID3D12Device* device) noexcept {
    s_create_heap.install(device, &hook_create_heap);
    s_create_rtv.install(device, &hook_create_rtv);
    s_create_dsv.install(device, &hook_create_dsv);
}

void uninstall_descriptor_hooks() noexcept {
    s_create_heap.uninstall();
    s_create_rtv.uninstall();
    s_create_dsv.uninstall();
}

} // namespace vangfx::backend::d3d12
