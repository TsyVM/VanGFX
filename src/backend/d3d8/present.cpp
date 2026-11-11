/**
 * src/backend/d3d8/present.cpp — IDirect3DDevice8::Present vtable hook.
 *
 * Slot 15 — Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion).
 * Unlike D3D9+, D3D8's Present takes RECT*,RECT*,HWND,RGNDATA*.
 *
 * On first Present we also install the draw hooks if they haven't been
 * installed yet (belt-and-suspenders in case install_draw_hooks was not
 * called at context construction time).
 */

#include "d3d8_impl.hpp"
#include <internal/compat/d3d8_slots.hpp>
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::d3d8 {

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(void* dev,
                                                const RECT* src,
                                                const RECT* dst,
                                                HWND        wnd,
                                                const void* dirty);

static PresentFn s_orig_present  = nullptr;
static void*     s_patched_device = nullptr;

static void before_present(IDirect3DDevice8* dev) {
    HubReader reader;
    ContextSet set = contexts_for(dev);
    for (D3D8ContextImpl* ctx : set) {
        const auto     timing = ctx->tick();
        const uint32_t frame  = ctx->next_frame();
        DevState* st = dev_state(dev);

        D3D8FrameContextImpl cap_fc(*ctx, frame, dev, st);
        ctx->capture_present(cap_fc, timing.first);

        PresentEvent ev{};
        ev.sync_interval = 1;
        ev.flags         = 0;
        D3D8FrameContextImpl fc(*ctx, frame, dev, st);
        ctx->dispatch_present(fc, ev);
    }
}

HRESULT STDMETHODCALLTYPE hook_present(void* dev,
                                        const RECT* src, const RECT* dst,
                                        HWND wnd, const void* dirty) {
    if (!hub_idle() && !internal()) {
        before_present(static_cast<IDirect3DDevice8*>(dev));
    }
    return s_orig_present(dev, src, dst, wnd, dirty);
}

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

bool install_present_hooks(IDirect3DDevice8* dev) noexcept {
    if (s_patched_device == dev) return true;
    s_patched_device = dev;
    patch_slot(dev, d3d8_slots::Present,
               reinterpret_cast<void*>(&hook_present),
               reinterpret_cast<void**>(&s_orig_present));
    return s_orig_present != nullptr;
}

void uninstall_present_hooks(IDirect3DDevice8* dev) noexcept {
    if (!dev || s_patched_device != dev || !s_orig_present) return;
    restore_slot(dev, d3d8_slots::Present, s_orig_present);
    s_orig_present   = nullptr;
    s_patched_device = nullptr;
}

} // namespace vangfx::backend::d3d8
