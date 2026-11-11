/**
 * src/backend/gl/present.cpp — wglSwapBuffers hook for OpenGL.
 *
 * wglSwapBuffers is called on the render thread with the GL context current,
 * so wglGetCurrentContext() returns the right HGLRC at hook time.
 *
 * A GLContextImpl that was created without a specific HGLRC auto-adopts the
 * first HGLRC it sees during SwapBuffers (like D3D10 auto-adopts swap chains).
 */

#include "gl_impl.hpp"
#include <vh/vh.vh>
#include <vangfx/events/surface.hpp>

namespace vangfx::backend::gl {

namespace {

using SwapBuffersFn = BOOL (WINAPI*)(HDC);
static SwapBuffersFn orig_wgl_swap = nullptr;
static vh::Hook      s_wgl_swap_hook;

static void before_swap(HGLRC hglrc) {
    if (!hglrc) return;
    HubReader reader;
    ContextSet set = contexts_for(hglrc);
    for (GLContextImpl* ctx : set) {
        // Auto-adopt HGLRC if not yet assigned.
        if (!ctx->hglrc()) ctx->adopt_hglrc(hglrc);
        if (ctx->hglrc() != hglrc) continue;

        const auto     timing = ctx->tick();
        const uint32_t frame  = ctx->next_frame();
        DevState* st = dev_state(hglrc);

        GLFrameContextImpl cap_fc(*ctx, frame, hglrc, st);
        ctx->capture_present(cap_fc, timing.first);

        PresentEvent ev{};
        ev.sync_interval = 1;
        ev.flags         = 0;
        GLFrameContextImpl fc(*ctx, frame, hglrc, st);
        ctx->dispatch_present(fc, ev);
    }
}

BOOL WINAPI hook_wgl_swap(HDC hdc) {
    if (hub_idle() || internal()) return orig_wgl_swap(hdc);
    HGLRC hglrc = wglGetCurrentContext();
    before_swap(hglrc);
    return orig_wgl_swap(hdc);
}

} // namespace

bool install_present_hooks() noexcept {
    s_wgl_swap_hook = vh::api_hook("opengl32.dll", "wglSwapBuffers",
                                    &hook_wgl_swap, &orig_wgl_swap);
    return orig_wgl_swap != nullptr;
}

void uninstall_present_hooks() noexcept {
    s_wgl_swap_hook = vh::Hook{};
    orig_wgl_swap   = nullptr;
}

} // namespace vangfx::backend::gl
