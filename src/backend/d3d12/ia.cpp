/**
 * src/backend/d3d12/ia.cpp — IASetVertexBuffers / IASetIndexBuffer.
 *
 * No event of their own: they keep the list's vertex and index buffer views,
 * which every draw event carries (DrawEvent::d3d12_vb / d3d12_ib) and which
 * FrameContext::buffer_at() turns back into the buffers behind them.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>

namespace vangfx::backend::d3d12 {

namespace lst = ::vangfx::d3d12_slots::graphics_command_list;

namespace {

using SetVBFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using SetIBFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);

hooks::Site s_set_vb{ lst::IASetVertexBuffers, "IASetVertexBuffers" };
hooks::Site s_set_ib{ lst::IASetIndexBuffer,   "IASetIndexBuffer" };

void STDMETHODCALLTYPE hook_set_vb(ID3D12GraphicsCommandList* self, UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
    s_set_vb.original<SetVBFn>(self)(self, start, count, views);
    if (hub_idle() || internal()) return;
    HubReader reader;
    ListState* st = list_state(self);
    if (!st) return;
    constexpr UINT kSlots = static_cast<UINT>(sizeof(st->vb.views) / sizeof(st->vb.views[0]));
    for (UINT i = 0; i < count && start + i < kSlots; ++i) {
        auto& v = st->vb.views[start + i];
        if (views) {
            v.gpu_virtual_address = views[i].BufferLocation;
            v.size_in_bytes       = views[i].SizeInBytes;
            v.stride_in_bytes     = views[i].StrideInBytes;
        } else {
            v = {};   // null unbinds the slots
        }
    }
    st->vb.start_slot = start;
    st->vb.count      = count;
}

void STDMETHODCALLTYPE hook_set_ib(ID3D12GraphicsCommandList* self, const D3D12_INDEX_BUFFER_VIEW* view) {
    s_set_ib.original<SetIBFn>(self)(self, view);
    if (hub_idle() || internal()) return;
    HubReader reader;
    ListState* st = list_state(self);
    if (!st) return;
    if (!view) { st->ib = {}; return; }
    st->ib.bound                    = true;
    st->ib.view.gpu_virtual_address = view->BufferLocation;
    st->ib.view.size_in_bytes       = view->SizeInBytes;
    st->ib.view.format = view->Format == DXGI_FORMAT_R32_UINT ? SurfaceFormat::R32_UINT : SurfaceFormat::R16_UINT;
}

} // namespace

void install_ia_hooks(ID3D12GraphicsCommandList* list) noexcept {
    s_set_vb.install(list, &hook_set_vb);
    s_set_ib.install(list, &hook_set_ib);
}

void uninstall_ia_hooks() noexcept {
    s_set_vb.uninstall();
    s_set_ib.uninstall();
}

} // namespace vangfx::backend::d3d12
