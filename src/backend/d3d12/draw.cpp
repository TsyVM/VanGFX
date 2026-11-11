/**
 * src/backend/d3d12/draw.cpp — draws, dispatches, and the list state a draw
 * event reports: the pipeline state and the root bindings.
 *
 * Hooked on ID3D12GraphicsCommandList (every list class, once each):
 *   DrawInstanced, DrawIndexedInstanced, Dispatch, ExecuteIndirect,
 *   DispatchMesh (GraphicsCommandList6), SetPipelineState,
 *   Set{Graphics,Compute}RootSignature, ...RootDescriptorTable,
 *   ...Root32BitConstant(s), ...RootConstantBufferView,
 *   ...RootShaderResourceView, ...RootUnorderedAccessView.
 *
 * Every piece of state lives in the list's ListState (hub.hpp), written only by
 * the thread recording that list, so the binding hooks take no lock beyond the
 * hub's shared one. A draw event points at the list's bindings rather than
 * copying them.
 *
 * An effect that swaps the pipeline for one draw (effect_bridge.cpp) leaves the
 * application's pipeline in ListState::restore_pso; the draw thunks put it back
 * after the draw, so the application never sees its own pipeline replaced.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>

#include <algorithm>
#include <cstring>

namespace vangfx::backend::d3d12 {

using namespace ::vangfx::d3d12;
namespace lst = ::vangfx::d3d12_slots::graphics_command_list;

namespace {

using DrawInstFn     = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using DrawIdxInstFn  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using DispatchFn     = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using DispatchMeshFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList6*, UINT, UINT, UINT);
using ExecIndirectFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT,
                                                ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using SetPsoFn       = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using SetRootSigFn   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using SetTableFn     = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using SetConstFn     = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using SetConstsFn    = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using SetViewFn      = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using ExecBundleFn   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);

hooks::Site s_draw_inst      { lst::DrawInstanced,        "DrawInstanced" };
hooks::Site s_draw_idx_inst  { lst::DrawIndexedInstanced, "DrawIndexedInstanced" };
hooks::Site s_dispatch       { lst::Dispatch,             "Dispatch" };
hooks::Site s_exec_indirect  { lst::ExecuteIndirect,      "ExecuteIndirect" };
hooks::Site s_dispatch_mesh  { lst::DispatchMesh,         "DispatchMesh" };
hooks::Site s_set_pso        { lst::SetPipelineState,     "SetPipelineState" };
hooks::Site s_exec_bundle    { lst::ExecuteBundle,        "ExecuteBundle" };
hooks::Site s_gfx_root_sig   { lst::SetGraphicsRootSignature,           "SetGraphicsRootSignature" };
hooks::Site s_cmp_root_sig   { lst::SetComputeRootSignature,            "SetComputeRootSignature" };
hooks::Site s_gfx_table      { lst::SetGraphicsRootDescriptorTable,     "SetGraphicsRootDescriptorTable" };
hooks::Site s_cmp_table      { lst::SetComputeRootDescriptorTable,      "SetComputeRootDescriptorTable" };
hooks::Site s_gfx_const      { lst::SetGraphicsRoot32BitConstant,       "SetGraphicsRoot32BitConstant" };
hooks::Site s_cmp_const      { lst::SetComputeRoot32BitConstant,        "SetComputeRoot32BitConstant" };
hooks::Site s_gfx_consts     { lst::SetGraphicsRoot32BitConstants,      "SetGraphicsRoot32BitConstants" };
hooks::Site s_cmp_consts     { lst::SetComputeRoot32BitConstants,       "SetComputeRoot32BitConstants" };
hooks::Site s_gfx_cbv        { lst::SetGraphicsRootConstantBufferView,  "SetGraphicsRootConstantBufferView" };
hooks::Site s_cmp_cbv        { lst::SetComputeRootConstantBufferView,   "SetComputeRootConstantBufferView" };
hooks::Site s_gfx_srv        { lst::SetGraphicsRootShaderResourceView,  "SetGraphicsRootShaderResourceView" };
hooks::Site s_cmp_srv        { lst::SetComputeRootShaderResourceView,   "SetComputeRootShaderResourceView" };
hooks::Site s_gfx_uav        { lst::SetGraphicsRootUnorderedAccessView, "SetGraphicsRootUnorderedAccessView" };
hooks::Site s_cmp_uav        { lst::SetComputeRootUnorderedAccessView,  "SetComputeRootUnorderedAccessView" };

DrawEvent base_event(const ListState& st) noexcept {
    DrawEvent ev{};
    ev.command_list       = { st.id };
    ev.pso                = st.pso;
    ev.vs_hash            = st.vs_hash;
    ev.ps_hash            = st.ps_hash;
    ev.root_bindings      = st.root_bindings;
    ev.root_binding_count = st.root_binding_count;
    ev.d3d12_vb           = st.vb;
    ev.d3d12_ib           = st.ib;
    return ev;
}

bool wants_draw(const ListState& st) noexcept {
    for (D3D12ContextImpl* ctx : st.contexts) if (ctx->wants_draw()) return true;
    return false;
}

// Dispatches `ev` to every listening context. Returns whether to suppress the call.
bool fire(ID3D12GraphicsCommandList* self, ListState& st, DrawEvent& ev) {
    for (D3D12ContextImpl* ctx : st.contexts) {
        if (!ctx->wants_draw()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index(), nullptr, self, &st);
        ctx->dispatch_draw(fc, ev);
    }
    return ev.intercept;
}

// Put the application's pipeline back if an effect swapped it for this draw.
void finish(ID3D12GraphicsCommandList* self, ListState& st) noexcept {
    if (ID3D12PipelineState* back = st.restore_pso) {
        st.restore_pso = nullptr;
        s_set_pso.original<SetPsoFn>(self)(self, back);
    }
}

// Every draw thunk has the same shape; `call` forwards to the original.
// `rasterises`: the call draws into the bound targets (not a compute dispatch),
// which a capture counts; a bundle's draws count where the bundle is executed.
template <typename Build, typename Call>
void draw_thunk(ID3D12GraphicsCommandList* self, bool rasterises, Build&& build, Call&& call) {
    if (hub_idle() || internal()) { call(); return; }
    HubReader reader;
    ListState* st = list_state(self);
    if (!st) { call(); return; }
    if (rasterises && st->type != D3D12_COMMAND_LIST_TYPE_BUNDLE) capture_draw(self, *st);
    if (!wants_draw(*st)) { call(); return; }
    DrawEvent ev = base_event(*st);
    build(ev);
    const bool intercept = fire(self, *st, ev);
    if (!intercept) call();
    finish(self, *st);
}

void STDMETHODCALLTYPE hook_draw_instanced(ID3D12GraphicsCommandList* self, UINT verts, UINT inst, UINT start_vert, UINT start_inst) {
    draw_thunk(self, true,
        [&](DrawEvent& ev) {
            ev.type = DrawType::DrawInstanced;
            ev.vertex_count = verts; ev.instance_count = inst;
            ev.start_vertex = static_cast<int32_t>(start_vert);
        },
        [&] { s_draw_inst.original<DrawInstFn>(self)(self, verts, inst, start_vert, start_inst); });
}

void STDMETHODCALLTYPE hook_draw_indexed_instanced(ID3D12GraphicsCommandList* self, UINT idx, UINT inst, UINT start_idx, INT base, UINT start_inst) {
    draw_thunk(self, true,
        [&](DrawEvent& ev) {
            ev.type = DrawType::DrawIndexedInstanced;
            ev.index_count = idx; ev.instance_count = inst;
            ev.start_index = start_idx; ev.base_vertex = base;
        },
        [&] { s_draw_idx_inst.original<DrawIdxInstFn>(self)(self, idx, inst, start_idx, base, start_inst); });
}

void STDMETHODCALLTYPE hook_dispatch(ID3D12GraphicsCommandList* self, UINT x, UINT y, UINT z) {
    draw_thunk(self, false,
        [&](DrawEvent& ev) {
            ev.type = DrawType::Dispatch;
            ev.dispatch_x = x; ev.dispatch_y = y; ev.dispatch_z = z;
        },
        [&] { s_dispatch.original<DispatchFn>(self)(self, x, y, z); });
}

void STDMETHODCALLTYPE hook_dispatch_mesh(ID3D12GraphicsCommandList6* self, UINT x, UINT y, UINT z) {
    draw_thunk(self, true,
        [&](DrawEvent& ev) {
            ev.type = DrawType::DispatchMesh;
            ev.dispatch_x = x; ev.dispatch_y = y; ev.dispatch_z = z;
        },
        [&] { s_dispatch_mesh.original<DispatchMeshFn>(self)(self, x, y, z); });
}

void STDMETHODCALLTYPE hook_execute_indirect(ID3D12GraphicsCommandList* self, ID3D12CommandSignature* sig, UINT max_count,
                                             ID3D12Resource* args, UINT64 args_offset, ID3D12Resource* count, UINT64 count_offset) {
    draw_thunk(self, true,
        [&](DrawEvent& ev) {
            ev.type          = DrawType::ExecuteIndirect;
            ev.vertex_count  = max_count;   // an upper bound on the commands it runs
            ev.indirect_args = { resource_id(args) };
        },
        [&] { s_exec_indirect.original<ExecIndirectFn>(self)(self, sig, max_count, args, args_offset, count, count_offset); });
}

// A bundle's draws land in the targets of the list executing it; a capture
// counts the bundle as one draw there. (Its draws were reported as it was recorded.)
void STDMETHODCALLTYPE hook_exec_bundle(ID3D12GraphicsCommandList* self, ID3D12GraphicsCommandList* bundle) {
    s_exec_bundle.original<ExecBundleFn>(self)(self, bundle);
    if (hub_idle() || internal()) return;
    HubReader reader;
    if (ListState* st = list_state(self)) capture_draw(self, *st);
}

// ── State the draw event reports ─────────────────────────────────────────────

void STDMETHODCALLTYPE hook_set_pso(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    s_set_pso.original<SetPsoFn>(self)(self, pso);
    if (hub_idle() || internal()) return;
    HubReader reader;
    if (ListState* st = list_state(self)) {
        const PsoRecord rec = pso_find(pso);
        st->pso         = { rec.id };
        st->pso_raw     = pso;
        st->restore_pso = nullptr;
        st->vs_hash     = rec.vs_hash;
        st->ps_hash     = rec.ps_hash;
    }
}

template <typename Fn>
void with_state(ID3D12GraphicsCommandList* self, Fn&& fn) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    if (ListState* st = list_state(self)) fn(*st);
}

void STDMETHODCALLTYPE hook_gfx_root_sig(ID3D12GraphicsCommandList* self, ID3D12RootSignature* rs) {
    s_gfx_root_sig.original<SetRootSigFn>(self)(self, rs);
    with_state(self, [](ListState& st) { st.clear_bindings(false); });
}
void STDMETHODCALLTYPE hook_cmp_root_sig(ID3D12GraphicsCommandList* self, ID3D12RootSignature* rs) {
    s_cmp_root_sig.original<SetRootSigFn>(self)(self, rs);
    with_state(self, [](ListState& st) { st.clear_bindings(true); });
}

void STDMETHODCALLTYPE hook_gfx_table(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    s_gfx_table.original<SetTableFn>(self)(self, index, base);
    with_state(self, [&](ListState& st) { st.binding(index, RootParameterType::DescriptorTable, false).descriptor_table = { base.ptr }; });
}
void STDMETHODCALLTYPE hook_cmp_table(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    s_cmp_table.original<SetTableFn>(self)(self, index, base);
    with_state(self, [&](ListState& st) { st.binding(index, RootParameterType::DescriptorTable, true).descriptor_table = { base.ptr }; });
}

void store_constants(ListState& st, bool compute, UINT index, UINT count, const void* src, UINT offset) noexcept {
    auto& b = st.binding(index, RootParameterType::Constants32, compute);
    constexpr UINT kCap = static_cast<UINT>(sizeof(b.constants) / sizeof(b.constants[0]));
    if (!src || offset >= kCap) return;
    const UINT n = std::min(count, kCap - offset);
    std::memcpy(&b.constants[offset], src, n * sizeof(uint32_t));
}

void STDMETHODCALLTYPE hook_gfx_const(ID3D12GraphicsCommandList* self, UINT index, UINT value, UINT offset) {
    s_gfx_const.original<SetConstFn>(self)(self, index, value, offset);
    with_state(self, [&](ListState& st) { store_constants(st, false, index, 1, &value, offset); });
}
void STDMETHODCALLTYPE hook_cmp_const(ID3D12GraphicsCommandList* self, UINT index, UINT value, UINT offset) {
    s_cmp_const.original<SetConstFn>(self)(self, index, value, offset);
    with_state(self, [&](ListState& st) { store_constants(st, true, index, 1, &value, offset); });
}
void STDMETHODCALLTYPE hook_gfx_consts(ID3D12GraphicsCommandList* self, UINT index, UINT count, const void* src, UINT offset) {
    s_gfx_consts.original<SetConstsFn>(self)(self, index, count, src, offset);
    with_state(self, [&](ListState& st) { store_constants(st, false, index, count, src, offset); });
}
void STDMETHODCALLTYPE hook_cmp_consts(ID3D12GraphicsCommandList* self, UINT index, UINT count, const void* src, UINT offset) {
    s_cmp_consts.original<SetConstsFn>(self)(self, index, count, src, offset);
    with_state(self, [&](ListState& st) { store_constants(st, true, index, count, src, offset); });
}

#define VANGFX_VIEW_HOOK(name, site, kind, compute)                                                    \
    void STDMETHODCALLTYPE name(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_VIRTUAL_ADDRESS va) { \
        site.original<SetViewFn>(self)(self, index, va);                                              \
        with_state(self, [&](ListState& st) { st.binding(index, RootParameterType::kind, compute).gpu_virtual_addr = va; }); \
    }
VANGFX_VIEW_HOOK(hook_gfx_cbv, s_gfx_cbv, Cbv, false)
VANGFX_VIEW_HOOK(hook_cmp_cbv, s_cmp_cbv, Cbv, true)
VANGFX_VIEW_HOOK(hook_gfx_srv, s_gfx_srv, Srv, false)
VANGFX_VIEW_HOOK(hook_cmp_srv, s_cmp_srv, Srv, true)
VANGFX_VIEW_HOOK(hook_gfx_uav, s_gfx_uav, Uav, false)
VANGFX_VIEW_HOOK(hook_cmp_uav, s_cmp_uav, Uav, true)
#undef VANGFX_VIEW_HOOK

hooks::Site* const kSites[] = {
    &s_draw_inst, &s_draw_idx_inst, &s_dispatch, &s_exec_indirect, &s_dispatch_mesh, &s_set_pso, &s_exec_bundle,
    &s_gfx_root_sig, &s_cmp_root_sig, &s_gfx_table, &s_cmp_table, &s_gfx_const, &s_cmp_const,
    &s_gfx_consts, &s_cmp_consts, &s_gfx_cbv, &s_cmp_cbv, &s_gfx_srv, &s_cmp_srv, &s_gfx_uav, &s_cmp_uav,
};

} // namespace

void install_draw_hooks(ID3D12GraphicsCommandList* list) noexcept {
    s_draw_inst.install(list, &hook_draw_instanced);
    s_draw_idx_inst.install(list, &hook_draw_indexed_instanced);
    s_dispatch.install(list, &hook_dispatch);
    s_exec_indirect.install(list, &hook_execute_indirect);
    s_set_pso.install(list, &hook_set_pso);
    s_exec_bundle.install(list, &hook_exec_bundle);
    s_gfx_root_sig.install(list, &hook_gfx_root_sig);
    s_cmp_root_sig.install(list, &hook_cmp_root_sig);
    s_gfx_table.install(list, &hook_gfx_table);
    s_cmp_table.install(list, &hook_cmp_table);
    s_gfx_const.install(list, &hook_gfx_const);
    s_cmp_const.install(list, &hook_cmp_const);
    s_gfx_consts.install(list, &hook_gfx_consts);
    s_cmp_consts.install(list, &hook_cmp_consts);
    s_gfx_cbv.install(list, &hook_gfx_cbv);
    s_cmp_cbv.install(list, &hook_cmp_cbv);
    s_gfx_srv.install(list, &hook_gfx_srv);
    s_cmp_srv.install(list, &hook_cmp_srv);
    s_gfx_uav.install(list, &hook_gfx_uav);
    s_cmp_uav.install(list, &hook_cmp_uav);
    // DispatchMesh is past the end of the base interface: only through a list
    // that really is an ID3D12GraphicsCommandList6.
    ID3D12GraphicsCommandList6* l6 = nullptr;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l6))) && l6) {
        s_dispatch_mesh.install(l6, &hook_dispatch_mesh);
        l6->Release();
    }
}

void uninstall_draw_hooks() noexcept {
    for (hooks::Site* s : kSites) s->uninstall();
}

// Used by effect_bridge.cpp to switch pipelines without being reported.
void set_pipeline_unreported(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) noexcept {
    s_set_pso.original<SetPsoFn>(list)(list, pso);
}

} // namespace vangfx::backend::d3d12
