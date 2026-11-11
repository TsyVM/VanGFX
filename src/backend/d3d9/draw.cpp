/**
 * src/backend/d3d9/draw.cpp — draws, the shaders a draw runs, state blocks,
 * the device's end, and installing every hook.
 *
 * Hooked on the device's own table (every site Heal::SameModule):
 *   DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP,
 *   DrawIndexedPrimitiveUP — reported as draw events;
 *   CreateVertexShader, CreatePixelShader — the bytecode hash each shader is
 *   known by; SetVertexShader, SetPixelShader — the shaders each draw reports,
 *   put back after a per-draw swap;
 *   BeginStateBlock, EndStateBlock — the runtime swaps the setters in and out
 *   of recording here, so the table is repaired after each;
 *   Release — the last one frees the device and its table.
 * Hooked on the shared IDirect3DStateBlock9 table:
 *   Apply — the bindings it changed are read back.
 */

#include "d3d9_impl.hpp"
#include <compat/d3d9_slots.hpp>
#include <hash.hpp>
#include <vangfx/events/draw.hpp>
#include <vangfx/events/state.hpp>

namespace vangfx::backend::d3d9 {

namespace s  = ::vangfx::d3d9_slots;
using hooks::Heal;

namespace {

using DrawFn        = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIdxFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUPFn      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIdxUPFn   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                  D3DFORMAT, const void*, UINT);
using CreateVSFn    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using CreatePSFn    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetVSFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetPSFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using BeginSBFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using EndSBFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DStateBlock9**);
using ReleaseFn     = ULONG(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using ApplyFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);

hooks::Site s_draw       { s::DrawPrimitive,          "DrawPrimitive",          Heal::SameModule };
hooks::Site s_draw_idx   { s::DrawIndexedPrimitive,   "DrawIndexedPrimitive",   Heal::SameModule };
hooks::Site s_draw_up    { s::DrawPrimitiveUP,        "DrawPrimitiveUP",        Heal::SameModule };
hooks::Site s_draw_idx_up{ s::DrawIndexedPrimitiveUP, "DrawIndexedPrimitiveUP", Heal::SameModule };
hooks::Site s_create_vs  { s::CreateVertexShader,     "CreateVertexShader",     Heal::SameModule };
hooks::Site s_create_ps  { s::CreatePixelShader,      "CreatePixelShader",      Heal::SameModule };
hooks::Site s_set_vs     { s::SetVertexShader,        "SetVertexShader",        Heal::SameModule };
hooks::Site s_set_ps     { s::SetPixelShader,         "SetPixelShader",         Heal::SameModule };
hooks::Site s_begin_sb   { s::BeginStateBlock,        "BeginStateBlock",        Heal::SameModule };
hooks::Site s_end_sb     { s::EndStateBlock,          "EndStateBlock",          Heal::SameModule };
hooks::Site s_release    { s::Release,                "IDirect3DDevice9::Release", Heal::SameModule };
hooks::Site s_apply      { s::state_block::Apply,     "IDirect3DStateBlock9::Apply" };

// Every site, for healing, forgetting a device's table and uninstalling.
// Append-only and published with release ordering, so the walk after every
// device call takes no lock. The Ex sites' slots lie past the end of a plain
// device's table, so they are kept apart and walked only for an Ex device.
constexpr int         kMaxSites = 64;
std::mutex            g_sites_mutex;   // registration only
hooks::Site*          g_device_sites[kMaxSites]{};
std::atomic<int>      g_device_count{ 0 };
hooks::Site*          g_ex_sites[kMaxSites]{};
std::atomic<int>      g_ex_count{ 0 };
hooks::Site*          g_class_sites[kMaxSites]{};
std::atomic<int>      g_class_count{ 0 };

void add_site(hooks::Site** list, std::atomic<int>& count, hooks::Site& site) noexcept {
    std::lock_guard lk(g_sites_mutex);
    const int n = count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) if (list[i] == &site) return;
    if (n == kMaxSites) return;
    list[n] = &site;
    count.store(n + 1, std::memory_order_release);
}

// ── Draws ────────────────────────────────────────────────────────────────────

uint32_t vertices_for(D3DPRIMITIVETYPE type, UINT prims) noexcept {
    switch (type) {
    case D3DPT_POINTLIST:     return prims;
    case D3DPT_LINELIST:      return prims * 2;
    case D3DPT_LINESTRIP:     return prims + 1;
    case D3DPT_TRIANGLELIST:  return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return prims + 2;
    default:                  return prims;
    }
}

PrimitiveTopology topology_for(D3DPRIMITIVETYPE type) noexcept {
    switch (type) {
    case D3DPT_POINTLIST:     return PrimitiveTopology::PointList;
    case D3DPT_LINELIST:      return PrimitiveTopology::LineList;
    case D3DPT_LINESTRIP:     return PrimitiveTopology::LineStrip;
    case D3DPT_TRIANGLESTRIP: return PrimitiveTopology::TriangleStrip;
    case D3DPT_TRIANGLEFAN:   return PrimitiveTopology::TriangleFan;
    default:                  return PrimitiveTopology::TriangleList;
    }
}

// D3D9 names the topology with each draw rather than as state; a change is
// reported as a state change, so the pipeline reads the same on every backend.
void note_topology(DevState& st, D3DPRIMITIVETYPE type) {
    const PrimitiveTopology t = topology_for(type);
    if (st.pipeline.topology == t) return;
    st.pipeline.topology = t;
    StateChangeEvent ev{};
    ev.dirty    = StateDirty_Topology;
    ev.topology = t;
    for (D3D9ContextImpl* ctx : st.contexts) {
        if (!ctx->wants_state()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), &st);
        ctx->dispatch_state(fc, ev);
    }
}

template <typename Build, typename Call>
HRESULT draw_thunk(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, DrawType kind, Build&& build, Call&& call) {
    const Healer healer{ self };
    if (hub_idle() || internal()) return call();
    HubReader reader;
    DevState* st = dev_state(self);
    if (!st) return call();
    note_topology(*st, type);
    if (!st->recording) capture_draw(self, *st);
    bool wants = false;
    for (D3D9ContextImpl* ctx : st->contexts) wants |= ctx->wants_draw();
    if (!wants) return call();

    DrawEvent ev{};
    ev.type    = kind;
    ev.vs_hash = st->vs_hash;
    ev.ps_hash = st->ps_hash;
    build(ev);
    for (D3D9ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_draw()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), st);
        ctx->dispatch_draw(fc, ev);
    }
    const HRESULT hr = ev.intercept ? S_OK : call();
    if (st->vs_swapped || st->ps_swapped) restore_shaders(self, *st);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_draw(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
    return draw_thunk(self, type, DrawType::Draw,
        [&](DrawEvent& ev) { ev.vertex_count = vertices_for(type, prims); ev.start_vertex = static_cast<int32_t>(start); },
        [&] { return s_draw.original<DrawFn>(self)(self, type, start, prims); });
}

HRESULT STDMETHODCALLTYPE hook_draw_idx(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base, UINT min_index,
                                        UINT verts, UINT start, UINT prims) {
    return draw_thunk(self, type, DrawType::DrawIndexed,
        [&](DrawEvent& ev) {
            ev.vertex_count = verts;
            ev.index_count  = vertices_for(type, prims);
            ev.start_index  = start;
            ev.base_vertex  = base;
        },
        [&] { return s_draw_idx.original<DrawIdxFn>(self)(self, type, base, min_index, verts, start, prims); });
}

// The UP ("user pointer") draws take their vertices from the call rather than a
// bound buffer; to a callback they are the same draws.
HRESULT STDMETHODCALLTYPE hook_draw_up(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT prims, const void* data, UINT stride) {
    return draw_thunk(self, type, DrawType::Draw,
        [&](DrawEvent& ev) { ev.vertex_count = vertices_for(type, prims); },
        [&] { return s_draw_up.original<DrawUPFn>(self)(self, type, prims, data, stride); });
}

HRESULT STDMETHODCALLTYPE hook_draw_idx_up(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT min_index, UINT verts,
                                           UINT prims, const void* idx, D3DFORMAT idx_fmt, const void* data, UINT stride) {
    return draw_thunk(self, type, DrawType::DrawIndexed,
        [&](DrawEvent& ev) { ev.vertex_count = verts; ev.index_count = vertices_for(type, prims); },
        [&] { return s_draw_idx_up.original<DrawIdxUPFn>(self)(self, type, min_index, verts, prims, idx, idx_fmt, data, stride); });
}

// ── Shaders ──────────────────────────────────────────────────────────────────

// Hashed as they are created: a draw names its shaders by these hashes, and a
// game creates most of them at load. The bytecode's size is the shader's own
// answer (GetFunction), not a scan for the end token.
template <typename Shader, typename Fn>
HRESULT create_shader(hooks::Site& site, IDirect3DDevice9* self, const DWORD* fn, Shader** out) {
    const Healer healer{ self };
    const HRESULT hr = site.original<Fn>(self)(self, fn, out);
    if (SUCCEEDED(hr) && fn && out && *out && !internal()) {
        UINT size = 0;
        if (SUCCEEDED((*out)->GetFunction(nullptr, &size)) && size)
            shader_register(*out, fn, size);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_vs(IDirect3DDevice9* self, const DWORD* fn, IDirect3DVertexShader9** out) {
    return create_shader<IDirect3DVertexShader9, CreateVSFn>(s_create_vs, self, fn, out);
}
HRESULT STDMETHODCALLTYPE hook_create_ps(IDirect3DDevice9* self, const DWORD* fn, IDirect3DPixelShader9** out) {
    return create_shader<IDirect3DPixelShader9, CreatePSFn>(s_create_ps, self, fn, out);
}

template <typename Fn>
void with_state(IDirect3DDevice9* self, Fn&& fn) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    DevState* st = dev_state(self);
    if (st && !st->recording) fn(*st);
}

HRESULT STDMETHODCALLTYPE hook_set_vs(IDirect3DDevice9* self, IDirect3DVertexShader9* vs) {
    const Healer healer{ self };
    const HRESULT hr = s_set_vs.original<SetVSFn>(self)(self, vs);
    if (SUCCEEDED(hr)) {
        with_state(self, [&](DevState& st) {
            const ShaderInfo info = shader_info(vs);
            st.vs = vs; st.vs_hash = info.hash; st.vs_major = info.major; st.vs_swapped = false;
        });
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_set_ps(IDirect3DDevice9* self, IDirect3DPixelShader9* ps) {
    const Healer healer{ self };
    const HRESULT hr = s_set_ps.original<SetPSFn>(self)(self, ps);
    if (SUCCEEDED(hr)) {
        with_state(self, [&](DevState& st) {
            const ShaderInfo info = shader_info(ps);
            st.ps = ps; st.ps_hash = info.hash; st.ps_major = info.major; st.ps_swapped = false;
        });
    }
    return hr;
}

// ── State blocks ─────────────────────────────────────────────────────────────

void set_recording(IDirect3DDevice9* self, bool on) {
    if (hub_idle() || internal()) return;
    HubReader reader;
    if (DevState* st = dev_state(self)) st->recording = on;
}

HRESULT STDMETHODCALLTYPE hook_begin_sb(IDirect3DDevice9* self) {
    const Healer healer{ self };   // the setters were just swapped for recorders
    const HRESULT hr = s_begin_sb.original<BeginSBFn>(self)(self);
    if (SUCCEEDED(hr)) set_recording(self, true);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_end_sb(IDirect3DDevice9* self, IDirect3DStateBlock9** out) {
    const Healer healer{ self };   // and swapped back
    const HRESULT hr = s_end_sb.original<EndSBFn>(self)(self, out);
    set_recording(self, false);
    return hr;
}

// A state block sets whatever it captured without calling a setter: the
// shaders are read back, and when anyone listens for state, the rest too.
HRESULT STDMETHODCALLTYPE hook_apply(IDirect3DStateBlock9* self) {
    const HRESULT hr = s_apply.original<ApplyFn>(self)(self);
    if (FAILED(hr) || hub_idle() || internal()) return hr;
    IDirect3DDevice9* dev = nullptr;
    if (FAILED(self->GetDevice(&dev)) || !dev) return hr;
    dev->Release();   // the state block holds it
    HubReader reader;
    DevState* st = dev_state(dev);
    if (!st || st->recording) return hr;
    read_shaders(dev, *st);
    bool wants = false;
    for (D3D9ContextImpl* ctx : st->contexts) wants |= ctx->wants_state();
    if (!wants) return hr;
    const PrimitiveTopology topology = st->pipeline.topology;
    read_pipeline(dev, st->pipeline);
    st->pipeline.topology = topology;
    StateChangeEvent ev{};
    ev.dirty = StateDirty_Blend | StateDirty_DepthStencil | StateDirty_Rasterizer | StateDirty_Samplers |
               StateDirty_Viewports | StateDirty_Scissors | StateDirty_VertexDecl;
    ev.blend  = st->pipeline.blend;
    ev.depth  = st->pipeline.depth_stencil;
    ev.raster = st->pipeline.raster;
    std::copy(std::begin(st->pipeline.samplers), std::end(st->pipeline.samplers), std::begin(ev.samplers));
    std::copy(std::begin(st->pipeline.viewports), std::end(st->pipeline.viewports), std::begin(ev.viewports));
    std::copy(std::begin(st->pipeline.scissors), std::end(st->pipeline.scissors), std::begin(ev.scissors));
    ev.topology = topology;
    for (D3D9ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_state()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), st);
        ctx->dispatch_state(fc, ev);
    }
    return hr;
}

// ── The device's end ─────────────────────────────────────────────────────────

// The last Release frees the device and the table it was hooked through; every
// site forgets that table, so a device later given the same address is hooked
// afresh rather than taken for one already hooked. No Healer: after the last
// Release there is no table to repair.
ULONG STDMETHODCALLTYPE hook_release(IDirect3DDevice9* self) {
    const void* table = hooks::vtable_of(self);
    const ULONG left = s_release.original<ReleaseFn>(self)(self);
    if (left == 0) {
        const int n = g_device_count.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) g_device_sites[i]->forget(table);
        const int nx = g_ex_count.load(std::memory_order_acquire);
        for (int i = 0; i < nx; ++i) g_ex_sites[i]->forget(table);
        forget_device(self);
    }
    return left;
}

// The state block class is hooked through one made for the purpose.
HRESULT prime_state_block(IDirect3DDevice9* device) noexcept {
    InternalScope internal_work;
    IDirect3DStateBlock9* sb = nullptr;
    const HRESULT hr = device->CreateStateBlock(D3DSBT_PIXELSTATE, &sb);
    if (SUCCEEDED(hr) && sb) {
        s_apply.install(sb, &hook_apply);
        sb->Release();
    }
    return hr;
}

hooks::Site* const kDeviceSites[] = {
    &s_draw, &s_draw_idx, &s_draw_up, &s_draw_idx_up, &s_create_vs, &s_create_ps, &s_set_vs, &s_set_ps,
    &s_begin_sb, &s_end_sb, &s_release,
};

} // namespace

void register_device_site(hooks::Site& site) noexcept    { add_site(g_device_sites, g_device_count, site); }
void register_ex_device_site(hooks::Site& site) noexcept { add_site(g_ex_sites, g_ex_count, site); }
void register_class_site(hooks::Site& site) noexcept     { add_site(g_class_sites, g_class_count, site); }

void heal_device(IDirect3DDevice9* device) noexcept {
    const int n = g_device_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) g_device_sites[i]->heal(device);
    // An Ex table is one the first Ex site was installed on; a plain device's
    // table is never read at those slots.
    const int nx = g_ex_count.load(std::memory_order_acquire);
    if (nx > 0 && g_ex_sites[0]->installed_on(device))
        for (int i = 0; i < nx; ++i) g_ex_sites[i]->heal(device);
}

void set_vs_unreported(IDirect3DDevice9* device, IDirect3DVertexShader9* vs) noexcept {
    s_set_vs.original<SetVSFn>(device)(device, vs);
}

void set_ps_unreported(IDirect3DDevice9* device, IDirect3DPixelShader9* ps) noexcept {
    s_set_ps.original<SetPSFn>(device)(device, ps);
}

void restore_shaders(IDirect3DDevice9* device, DevState& st) noexcept {
    if (st.vs_swapped) set_vs_unreported(device, st.vs);
    if (st.ps_swapped) set_ps_unreported(device, st.ps);
    st.vs_swapped = st.ps_swapped = false;
}

bool install_device_hooks(IDirect3DDevice9* device, bool ex) noexcept {
    if (!device) return false;
    bool ok = s_draw.install(device, &hook_draw);
    ok &= s_draw_idx.install(device, &hook_draw_idx);
    ok &= s_draw_up.install(device, &hook_draw_up);
    ok &= s_draw_idx_up.install(device, &hook_draw_idx_up);
    ok &= s_create_vs.install(device, &hook_create_vs);
    ok &= s_create_ps.install(device, &hook_create_ps);
    ok &= s_set_vs.install(device, &hook_set_vs);
    ok &= s_set_ps.install(device, &hook_set_ps);
    ok &= s_begin_sb.install(device, &hook_begin_sb);
    ok &= s_end_sb.install(device, &hook_end_sb);
    ok &= s_release.install(device, &hook_release);
    for (hooks::Site* site : kDeviceSites) register_device_site(*site);
    (void)prime_state_block(device);
    register_class_site(s_apply);
    install_state_hooks(device);
    install_rendertarget_hooks(device);
    install_resource_hooks(device, ex);
    install_query_hooks(device);
    install_present_hooks(device, ex);
    return ok;
}

void uninstall_all_hooks() noexcept {
    const int nd = g_device_count.load(std::memory_order_acquire);
    for (int i = 0; i < nd; ++i) g_device_sites[i]->uninstall();
    const int nx = g_ex_count.load(std::memory_order_acquire);
    for (int i = 0; i < nx; ++i) g_ex_sites[i]->uninstall();
    const int nc = g_class_count.load(std::memory_order_acquire);
    for (int i = 0; i < nc; ++i) g_class_sites[i]->uninstall();
}

}  // namespace vangfx::backend::d3d9
