/**
 * src/backend/d3d11/draw.cpp — draws and dispatches, the shaders a draw runs,
 * and keeping the device-context hooks in place.
 *
 * Hooked on ID3D11DeviceContext (immediate and deferred classes, once each,
 * every site Heal::SameModule):
 *   Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawAuto,
 *   DrawInstancedIndirect, DrawIndexedInstancedIndirect, Dispatch,
 *   DispatchIndirect — reported as draw events;
 *   VSSetShader, PSSetShader, CSSetShader — the shaders each draw reports (by
 *   bytecode hash) and puts back after a per-draw swap;
 *   Flush, ClearState, ExecuteCommandList, FinishCommandList — the calls after
 *   which the runtime may have swapped its own table entries, and the ones that
 *   clear a context's bindings.
 *
 * A per-draw effect (FrameContext::bind_effect) sets the replacement shader
 * past the hook; the draw thunk puts the game's shader back after the draw.
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>
#include <compat/lifetime.hpp>
#include <vangfx/events/draw.hpp>

#include <algorithm>
#include <vector>

namespace vangfx::backend::d3d11 {

namespace dc = ::vangfx::d3d11_slots::device_context;
using hooks::Heal;

namespace {

using DrawFn        = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIdxFn     = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawInstFn    = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIdxInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawAutoFn    = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using IndirectFn    = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispatchFn    = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using SetVSFn       = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
using SetPSFn       = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using SetCSFn       = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
using VoidFn        = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using ExecuteFn     = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishFn      = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);

hooks::Site s_draw          { dc::Draw,                         "Draw",                         Heal::SameModule };
hooks::Site s_draw_idx      { dc::DrawIndexed,                  "DrawIndexed",                  Heal::SameModule };
hooks::Site s_draw_inst     { dc::DrawInstanced,                "DrawInstanced",                Heal::SameModule };
hooks::Site s_draw_idx_inst { dc::DrawIndexedInstanced,         "DrawIndexedInstanced",         Heal::SameModule };
hooks::Site s_draw_auto     { dc::DrawAuto,                     "DrawAuto",                     Heal::SameModule };
hooks::Site s_draw_ind      { dc::DrawInstancedIndirect,        "DrawInstancedIndirect",        Heal::SameModule };
hooks::Site s_draw_idx_ind  { dc::DrawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect", Heal::SameModule };
hooks::Site s_dispatch      { dc::Dispatch,                     "Dispatch",                     Heal::SameModule };
hooks::Site s_dispatch_ind  { dc::DispatchIndirect,             "DispatchIndirect",             Heal::SameModule };
hooks::Site s_set_vs        { dc::VSSetShader,                  "VSSetShader",                  Heal::SameModule };
hooks::Site s_set_ps        { dc::PSSetShader,                  "PSSetShader",                  Heal::SameModule };
hooks::Site s_set_cs        { dc::CSSetShader,                  "CSSetShader",                  Heal::SameModule };
hooks::Site s_flush         { dc::Flush,                        "Flush",                        Heal::SameModule };
hooks::Site s_clear_state   { dc::ClearState,                   "ClearState",                   Heal::SameModule };
hooks::Site s_execute       { dc::ExecuteCommandList,           "ExecuteCommandList",           Heal::SameModule };
hooks::Site s_finish        { dc::FinishCommandList,            "FinishCommandList",            Heal::SameModule };

// Every device-context site of every module, for heal_context. Append-only and
// published with release ordering, so the walk on every draw takes no lock.
constexpr int             kMaxSites = 64;
std::mutex                g_sites_mutex;   // registration only
hooks::Site*              g_sites[kMaxSites]{};
std::atomic<int>          g_site_count{ 0 };
// The ID3D11DeviceContext1..4 sites (context1.cpp), healed only on tables they
// were installed on: a table without those entries is shorter.
hooks::Site*              g_ext_sites[kMaxSites]{};
std::atomic<int>          g_ext_count{ 0 };

DrawEvent base_event(const DcState& st, DrawType type) noexcept {
    DrawEvent ev{};
    ev.type    = type;
    const bool compute = type == DrawType::Dispatch || type == DrawType::DispatchIndirect;
    ev.vs_hash = compute ? 0 : st.vs_hash;
    ev.ps_hash = compute ? st.cs_hash : st.ps_hash;
    return ev;
}

bool fire(ID3D11DeviceContext* self, DcState& st, DrawEvent& ev) {
    for (D3D11ContextImpl* ctx : st.contexts) {
        if (!ctx->wants_draw()) continue;
        D3D11FrameContextImpl fc(*ctx, ctx->current_frame_index(), self, &st);
        ctx->dispatch_draw(fc, ev);
    }
    return ev.intercept;
}

template <typename Build, typename Call>
void draw_thunk(ID3D11DeviceContext* self, DrawType type, Build&& build, Call&& call) {
    if (hub_idle() || internal()) { call(); return; }
    {
        HubReader reader;
        DcState* st = dc_state(self);
        bool wants = false;
        if (st) {
            if (type != DrawType::Dispatch && type != DrawType::DispatchIndirect) capture_draw(self, *st);
            for (D3D11ContextImpl* ctx : st->contexts) wants |= ctx->wants_draw();
        }
        if (!wants) {
            call();
        } else {
            DrawEvent ev = base_event(*st, type);
            build(ev);
            if (!fire(self, *st, ev)) call();
            if (st->vs_swapped || st->ps_swapped) restore_shaders(self, *st);
        }
    }
    heal_context(self);
}

void STDMETHODCALLTYPE hook_draw(ID3D11DeviceContext* self, UINT n, UINT start) {
    draw_thunk(self, DrawType::Draw,
        [&](DrawEvent& ev) { ev.vertex_count = n; ev.start_vertex = static_cast<int32_t>(start); },
        [&] { s_draw.original<DrawFn>(self)(self, n, start); });
}
void STDMETHODCALLTYPE hook_draw_idx(ID3D11DeviceContext* self, UINT n, UINT start, INT base) {
    draw_thunk(self, DrawType::DrawIndexed,
        [&](DrawEvent& ev) { ev.index_count = n; ev.start_index = start; ev.base_vertex = base; },
        [&] { s_draw_idx.original<DrawIdxFn>(self)(self, n, start, base); });
}
void STDMETHODCALLTYPE hook_draw_inst(ID3D11DeviceContext* self, UINT n, UINT inst, UINT start, UINT start_inst) {
    draw_thunk(self, DrawType::DrawInstanced,
        [&](DrawEvent& ev) { ev.vertex_count = n; ev.instance_count = inst; ev.start_vertex = static_cast<int32_t>(start); },
        [&] { s_draw_inst.original<DrawInstFn>(self)(self, n, inst, start, start_inst); });
}
void STDMETHODCALLTYPE hook_draw_idx_inst(ID3D11DeviceContext* self, UINT n, UINT inst, UINT start, INT base, UINT start_inst) {
    draw_thunk(self, DrawType::DrawIndexedInstanced,
        [&](DrawEvent& ev) { ev.index_count = n; ev.instance_count = inst; ev.start_index = start; ev.base_vertex = base; },
        [&] { s_draw_idx_inst.original<DrawIdxInstFn>(self)(self, n, inst, start, base, start_inst); });
}
void STDMETHODCALLTYPE hook_draw_auto(ID3D11DeviceContext* self) {
    draw_thunk(self, DrawType::DrawAuto, [](DrawEvent&) {},
        [&] { s_draw_auto.original<DrawAutoFn>(self)(self); });
}
void STDMETHODCALLTYPE hook_draw_ind(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset) {
    draw_thunk(self, DrawType::DrawIndirect,
        [&](DrawEvent& ev) { ev.indirect_args = { resource_id(args) }; },
        [&] { s_draw_ind.original<IndirectFn>(self)(self, args, offset); });
}
void STDMETHODCALLTYPE hook_draw_idx_ind(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset) {
    draw_thunk(self, DrawType::DrawIndexedIndirect,
        [&](DrawEvent& ev) { ev.indirect_args = { resource_id(args) }; },
        [&] { s_draw_idx_ind.original<IndirectFn>(self)(self, args, offset); });
}
void STDMETHODCALLTYPE hook_dispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    draw_thunk(self, DrawType::Dispatch,
        [&](DrawEvent& ev) { ev.dispatch_x = x; ev.dispatch_y = y; ev.dispatch_z = z; },
        [&] { s_dispatch.original<DispatchFn>(self)(self, x, y, z); });
}
void STDMETHODCALLTYPE hook_dispatch_ind(ID3D11DeviceContext* self, ID3D11Buffer* args, UINT offset) {
    draw_thunk(self, DrawType::DispatchIndirect,
        [&](DrawEvent& ev) { ev.indirect_args = { resource_id(args) }; },
        [&] { s_dispatch_ind.original<IndirectFn>(self)(self, args, offset); });
}

// ── Shader bindings ──────────────────────────────────────────────────────────

template <typename Fn>
void with_state(ID3D11DeviceContext* self, Fn&& fn) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    if (DcState* st = dc_state(self)) fn(*st);
}

void STDMETHODCALLTYPE hook_set_vs(ID3D11DeviceContext* self, ID3D11VertexShader* vs, ID3D11ClassInstance* const* inst, UINT n) {
    const Healer healer{ self };
    s_set_vs.original<SetVSFn>(self)(self, vs, inst, n);
    with_state(self, [&](DcState& st) {
        st.vs = vs; st.vs_hash = shader_hash(vs); st.vs_swapped = false;
        st.vs_inst_n = std::min<UINT>(n, DcState::kMaxInstances);
        for (UINT i = 0; i < st.vs_inst_n; ++i) st.vs_inst[i] = inst ? inst[i] : nullptr;
    });
}
void STDMETHODCALLTYPE hook_set_ps(ID3D11DeviceContext* self, ID3D11PixelShader* ps, ID3D11ClassInstance* const* inst, UINT n) {
    const Healer healer{ self };
    s_set_ps.original<SetPSFn>(self)(self, ps, inst, n);
    with_state(self, [&](DcState& st) {
        st.ps = ps; st.ps_hash = shader_hash(ps); st.ps_swapped = false;
        st.ps_inst_n = std::min<UINT>(n, DcState::kMaxInstances);
        for (UINT i = 0; i < st.ps_inst_n; ++i) st.ps_inst[i] = inst ? inst[i] : nullptr;
    });
}
void STDMETHODCALLTYPE hook_set_cs(ID3D11DeviceContext* self, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* inst, UINT n) {
    const Healer healer{ self };
    s_set_cs.original<SetCSFn>(self)(self, cs, inst, n);
    with_state(self, [&](DcState& st) { st.cs = cs; st.cs_hash = shader_hash(cs); });
}

// ── Calls after which the table may have changed, or the bindings cleared ────

void STDMETHODCALLTYPE hook_flush(ID3D11DeviceContext* self) {
    s_flush.original<VoidFn>(self)(self);
    settle_context(self);
}

void STDMETHODCALLTYPE hook_clear_state(ID3D11DeviceContext* self) {
    s_clear_state.original<VoidFn>(self)(self);
    with_state(self, [&](DcState& st) { st.clear(); capture_bind(self, st); });
    heal_context(self);
}

void STDMETHODCALLTYPE hook_execute(ID3D11DeviceContext* self, ID3D11CommandList* list, BOOL restore) {
    s_execute.original<ExecuteFn>(self)(self, list, restore);
    // Without RestoreContextState the immediate context is left cleared.
    if (!restore) with_state(self, [&](DcState& st) { st.clear(); capture_bind(self, st); });
    heal_context(self);
}

HRESULT STDMETHODCALLTYPE hook_finish(ID3D11DeviceContext* self, BOOL restore, ID3D11CommandList** out) {
    const HRESULT hr = s_finish.original<FinishFn>(self)(self, restore, out);
    if (!restore) with_state(self, [&](DcState& st) { st.clear(); capture_bind(self, st); });
    heal_context(self);
    return hr;
}

// A device context's table dies with it; every site is told, so the entry can
// be reused and a new table at the same address is patched afresh.
char g_table_key;
void on_context_destroyed(void*, const void*, uint64_t table) noexcept {
    const int n = g_site_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) g_sites[i]->forget(reinterpret_cast<const void*>(table));
    const int nx = g_ext_count.load(std::memory_order_acquire);
    for (int i = 0; i < nx; ++i) g_ext_sites[i]->forget(reinterpret_cast<const void*>(table));
}

hooks::Site* const kSites[] = {
    &s_draw, &s_draw_idx, &s_draw_inst, &s_draw_idx_inst, &s_draw_auto, &s_draw_ind, &s_draw_idx_ind,
    &s_dispatch, &s_dispatch_ind, &s_set_vs, &s_set_ps, &s_set_cs, &s_flush, &s_clear_state, &s_execute, &s_finish,
};

} // namespace

namespace {
void add_site(hooks::Site** list, std::atomic<int>& count, hooks::Site& site) noexcept {
    std::lock_guard lk(g_sites_mutex);
    const int n = count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) if (list[i] == &site) return;
    if (n == kMaxSites) return;
    list[n] = &site;
    count.store(n + 1, std::memory_order_release);
}
} // namespace

void register_context_site(hooks::Site& site) noexcept     { add_site(g_sites, g_site_count, site); }
void register_ext_context_site(hooks::Site& site) noexcept { add_site(g_ext_sites, g_ext_count, site); }

void heal_context(ID3D11DeviceContext* self) noexcept {
    const int n = g_site_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) g_sites[i]->heal(self);
    const int nx = g_ext_count.load(std::memory_order_acquire);
    for (int i = 0; i < nx; ++i)
        if (g_ext_sites[i]->installed_on(self)) g_ext_sites[i]->heal(self);
}

void settle_context(ID3D11DeviceContext* self) noexcept {
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    self->IAGetPrimitiveTopology(&topology);   // any call into the context but GetType finishes a lazy rewrite
    heal_context(self);
}

void set_vs_unreported(ID3D11DeviceContext* dc_, ID3D11VertexShader* vs, ID3D11ClassInstance* const* inst, UINT n) noexcept {
    s_set_vs.original<SetVSFn>(dc_)(dc_, vs, inst, n);
}

void set_ps_unreported(ID3D11DeviceContext* dc_, ID3D11PixelShader* ps, ID3D11ClassInstance* const* inst, UINT n) noexcept {
    s_set_ps.original<SetPSFn>(dc_)(dc_, ps, inst, n);
}

void restore_shaders(ID3D11DeviceContext* dc_, DcState& st) noexcept {
    if (st.vs_swapped) set_vs_unreported(dc_, st.vs, st.vs_inst, st.vs_inst_n);
    if (st.ps_swapped) set_ps_unreported(dc_, st.ps, st.ps_inst, st.ps_inst_n);
    st.vs_swapped = st.ps_swapped = false;
}

bool install_context_hooks(ID3D11DeviceContext* context) noexcept {
    if (!context) return false;
    bool ok = s_draw.install(context, &hook_draw);
    ok &= s_draw_idx.install(context, &hook_draw_idx);
    ok &= s_draw_inst.install(context, &hook_draw_inst);
    ok &= s_draw_idx_inst.install(context, &hook_draw_idx_inst);
    ok &= s_draw_auto.install(context, &hook_draw_auto);
    ok &= s_draw_ind.install(context, &hook_draw_ind);
    ok &= s_draw_idx_ind.install(context, &hook_draw_idx_ind);
    ok &= s_dispatch.install(context, &hook_dispatch);
    ok &= s_dispatch_ind.install(context, &hook_dispatch_ind);
    ok &= s_set_vs.install(context, &hook_set_vs);
    ok &= s_set_ps.install(context, &hook_set_ps);
    ok &= s_set_cs.install(context, &hook_set_cs);
    ok &= s_flush.install(context, &hook_flush);
    ok &= s_clear_state.install(context, &hook_clear_state);
    ok &= s_execute.install(context, &hook_execute);
    ok &= s_finish.install(context, &hook_finish);
    for (hooks::Site* s : kSites) register_context_site(*s);
    lifetime::watch(context, &on_context_destroyed, &g_table_key,
                    reinterpret_cast<uint64_t>(hooks::vtable_of(context)));
    install_state_hooks(context);
    install_rendertarget_hooks(context);
    install_query_hooks(context);
    install_map_hooks(context);
    install_heal_hooks(context);
    install_context1_hooks(context);
    return ok;
}

void uninstall_context_hooks() noexcept {
    const int n = g_site_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) g_sites[i]->uninstall();
    const int nx = g_ext_count.load(std::memory_order_acquire);
    for (int i = 0; i < nx; ++i) g_ext_sites[i]->uninstall();
    uninstall_context1_hooks();
}

}  // namespace vangfx::backend::d3d11
