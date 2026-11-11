/**
 * src/backend/d3d8/draw.cpp — D3D8 draw and shader hooks via vtable patching.
 *
 * Hooked vtable slots (IDirect3DDevice8):
 *   DrawPrimitive        (slot 70)
 *   DrawIndexedPrimitive (slot 71)
 *   SetVertexShader      (slot 76)
 *   SetPixelShader       (slot 88)
 *
 * D3D8 shader handles are DWORDs.  SetVertexShader/SetPixelShader update
 * DevState.vs_hash/ps_hash so draw dispatch has fresh hashes.
 *
 * Vtable patching requires the target page to be PAGE_READWRITE for the
 * duration of the write.  We patch per device on first adopt.
 */

#include "d3d8_impl.hpp"
#include <internal/compat/d3d8_slots.hpp>
#include <vangfx/events/draw.hpp>

namespace vangfx::backend::d3d8 {

namespace {

// ── Original function pointer types ───────────────────────────────────────────

using DrawPrimitiveFn =
    HRESULT(STDMETHODCALLTYPE*)(void* dev, /*D3DPRIMITIVETYPE*/UINT pt,
                                 UINT start, UINT count);
using DrawIndexedPrimitiveFn =
    HRESULT(STDMETHODCALLTYPE*)(void* dev, /*D3DPRIMITIVETYPE*/UINT pt,
                                 UINT min_vtx, UINT num_vtx,
                                 UINT start_idx, UINT count);
using SetVertexShaderFn =
    HRESULT(STDMETHODCALLTYPE*)(void* dev, DWORD handle);
using SetPixelShaderFn =
    HRESULT(STDMETHODCALLTYPE*)(void* dev, DWORD handle);

// Per-device saved originals.  We store one entry per patched device; in
// practice there is only ever one IDirect3DDevice8 at a time.
struct DevHooks {
    void*               device    = nullptr;
    DrawPrimitiveFn     draw_prim = nullptr;
    DrawIndexedPrimitiveFn draw_idx = nullptr;
    SetVertexShaderFn   set_vs    = nullptr;
    SetPixelShaderFn    set_ps    = nullptr;
};

static DevHooks s_dev_hooks;

// ── Dispatch helper ───────────────────────────────────────────────────────────

static void dispatch_draw(IDirect3DDevice8* dev, DrawEvent ev) {
    HubReader r;
    DevState* st = dev_state(dev);
    if (!st) return;
    ev.vs_hash = st->vs_hash;
    ev.ps_hash = st->ps_hash;
    for (D3D8ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_draw()) continue;
        D3D8FrameContextImpl fc(*ctx, ctx->current_frame_index(), dev, st);
        ctx->dispatch_draw(fc, ev);
    }
}

// ── Hook implementations ───────────────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE hook_draw_primitive(void* dev, UINT pt,
                                               UINT start, UINT count) {
    HRESULT hr = s_dev_hooks.draw_prim(dev, pt, start, count);
    if (hub_idle() || internal()) return hr;
    auto* device = static_cast<IDirect3DDevice8*>(dev);
    DrawEvent ev{}; ev.type = DrawType::Draw;
    ev.vertex_count = count;
    ev.start_vertex = start;
    dispatch_draw(device, ev);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_draw_indexed_primitive(void* dev, UINT pt,
                                                       UINT min_vtx, UINT num_vtx,
                                                       UINT start_idx, UINT count) {
    HRESULT hr = s_dev_hooks.draw_idx(dev, pt, min_vtx, num_vtx, start_idx, count);
    if (hub_idle() || internal()) return hr;
    auto* device = static_cast<IDirect3DDevice8*>(dev);
    DrawEvent ev{}; ev.type = DrawType::DrawIndexed;
    ev.index_count  = count;
    ev.start_index  = start_idx;
    ev.vertex_count = num_vtx;
    dispatch_draw(device, ev);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_set_vertex_shader(void* dev, DWORD handle) {
    HRESULT hr = s_dev_hooks.set_vs(dev, handle);
    if (hub_idle() || internal()) return hr;
    auto* device = static_cast<IDirect3DDevice8*>(dev);
    HubReader r;
    if (DevState* st = dev_state(device)) {
        st->vs_handle = static_cast<uint32_t>(handle);
        st->vs_hash   = make_shader_hash(static_cast<uint32_t>(handle));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_set_pixel_shader(void* dev, DWORD handle) {
    HRESULT hr = s_dev_hooks.set_ps(dev, handle);
    if (hub_idle() || internal()) return hr;
    auto* device = static_cast<IDirect3DDevice8*>(dev);
    HubReader r;
    if (DevState* st = dev_state(device)) {
        st->ps_handle = static_cast<uint32_t>(handle);
        st->ps_hash   = make_shader_hash(static_cast<uint32_t>(handle));
    }
    return hr;
}

// ── Vtable patcher ────────────────────────────────────────────────────────────

static void patch_slot(void* obj, uint32_t slot, void* fn, void** original) {
    void** vtbl = *reinterpret_cast<void***>(obj);
    *original   = vtbl[slot];
    DWORD old;
    VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
    vtbl[slot] = fn;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
}

static void restore_slot(void* obj, uint32_t slot, void* original) {
    void** vtbl = *reinterpret_cast<void***>(obj);
    DWORD old;
    VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
    vtbl[slot] = original;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
}

} // namespace

bool install_draw_hooks(IDirect3DDevice8* dev) noexcept {
    if (s_dev_hooks.device == dev) return true; // already patched
    s_dev_hooks.device = dev;
    patch_slot(dev, d3d8_slots::DrawPrimitive,
               reinterpret_cast<void*>(&hook_draw_primitive),
               reinterpret_cast<void**>(&s_dev_hooks.draw_prim));
    patch_slot(dev, d3d8_slots::DrawIndexedPrimitive,
               reinterpret_cast<void*>(&hook_draw_indexed_primitive),
               reinterpret_cast<void**>(&s_dev_hooks.draw_idx));
    patch_slot(dev, d3d8_slots::SetVertexShader,
               reinterpret_cast<void*>(&hook_set_vertex_shader),
               reinterpret_cast<void**>(&s_dev_hooks.set_vs));
    patch_slot(dev, d3d8_slots::SetPixelShader,
               reinterpret_cast<void*>(&hook_set_pixel_shader),
               reinterpret_cast<void**>(&s_dev_hooks.set_ps));
    return s_dev_hooks.draw_prim != nullptr;
}

void uninstall_all_hooks(IDirect3DDevice8* dev) noexcept {
    if (!dev || s_dev_hooks.device != dev) return;
    if (s_dev_hooks.draw_prim)
        restore_slot(dev, d3d8_slots::DrawPrimitive,        s_dev_hooks.draw_prim);
    if (s_dev_hooks.draw_idx)
        restore_slot(dev, d3d8_slots::DrawIndexedPrimitive,  s_dev_hooks.draw_idx);
    if (s_dev_hooks.set_vs)
        restore_slot(dev, d3d8_slots::SetVertexShader,       s_dev_hooks.set_vs);
    if (s_dev_hooks.set_ps)
        restore_slot(dev, d3d8_slots::SetPixelShader,        s_dev_hooks.set_ps);
    s_dev_hooks = {};
}

} // namespace vangfx::backend::d3d8
