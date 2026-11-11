/**
 * src/backend/gl/draw.cpp — glDraw* and glUseProgram hooks via VanHooks.
 *
 * Hooked: glDrawArrays, glDrawElements, glDrawArraysInstanced,
 *         glDrawElementsInstanced, glUseProgram.
 *
 * All hooks use vh::api_hook on "opengl32.dll".  glDrawArrays and
 * glDrawElements are GL 1.1 exports; glUseProgram and the instanced draws
 * are GL 2.0+/3.1 exports present in opengl32.dll on Windows 10+.
 *
 * wglGetCurrentContext() returns the HGLRC active on the calling thread,
 * which is the draw-call thread for all GL applications.
 */

#include "gl_impl.hpp"
#include <vh/vh.vh>
#include <vangfx/events/draw.hpp>

namespace vangfx::backend::gl {

namespace {

// ── Original function pointers (set by api_hook) ──────────────────────────────

using DrawArraysFn        = void (WINAPI*)(GLenum, GLint, GLsizei);
using DrawElementsFn      = void (WINAPI*)(GLenum, GLsizei, GLenum, const void*);
using DrawArraysInstFn    = void (WINAPI*)(GLenum, GLint, GLsizei, GLsizei);
using DrawElementsInstFn  = void (WINAPI*)(GLenum, GLsizei, GLenum, const void*, GLsizei);
using UseProgramFn        = void (WINAPI*)(GLuint);

static DrawArraysFn       orig_draw_arrays        = nullptr;
static DrawElementsFn     orig_draw_elements       = nullptr;
static DrawArraysInstFn   orig_draw_arrays_inst    = nullptr;
static DrawElementsInstFn orig_draw_elements_inst  = nullptr;
static UseProgramFn       orig_use_program         = nullptr;

// ── Persistent hook handles (RAII; destructor calls remove()) ─────────────────

static vh::Hook s_draw_arrays_hook;
static vh::Hook s_draw_elements_hook;
static vh::Hook s_draw_arrays_inst_hook;
static vh::Hook s_draw_elements_inst_hook;
static vh::Hook s_use_program_hook;

// ── Draw dispatch ─────────────────────────────────────────────────────────────

static void dispatch_draw(HGLRC hglrc, DrawEvent ev) {
    HubReader r;
    DevState* st = dev_state(hglrc);
    if (!st) return;
    ev.vs_hash = 0;
    ev.ps_hash = st->program_hash;
    for (GLContextImpl* ctx : st->contexts) {
        if (!ctx->wants_draw()) continue;
        GLFrameContextImpl fc(*ctx, ctx->current_frame_index(), hglrc, st);
        ctx->dispatch_draw(fc, ev);
    }
}

// ── Hooks ─────────────────────────────────────────────────────────────────────

void WINAPI hook_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    orig_draw_arrays(mode, first, count);
    if (hub_idle() || internal()) return;
    HGLRC hglrc = wglGetCurrentContext();
    if (!hglrc) return;
    DrawEvent ev{}; ev.type = DrawType::Draw;
    ev.vertex_count = static_cast<uint32_t>(count);
    ev.start_vertex = static_cast<uint32_t>(first);
    dispatch_draw(hglrc, ev);
}

void WINAPI hook_draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    orig_draw_elements(mode, count, type, indices);
    if (hub_idle() || internal()) return;
    HGLRC hglrc = wglGetCurrentContext();
    if (!hglrc) return;
    DrawEvent ev{}; ev.type = DrawType::DrawIndexed;
    ev.index_count = static_cast<uint32_t>(count);
    dispatch_draw(hglrc, ev);
}

void WINAPI hook_draw_arrays_inst(GLenum mode, GLint first, GLsizei count, GLsizei instcount) {
    orig_draw_arrays_inst(mode, first, count, instcount);
    if (hub_idle() || internal()) return;
    HGLRC hglrc = wglGetCurrentContext();
    if (!hglrc) return;
    DrawEvent ev{}; ev.type = DrawType::DrawInstanced;
    ev.vertex_count   = static_cast<uint32_t>(count);
    ev.instance_count = static_cast<uint32_t>(instcount);
    dispatch_draw(hglrc, ev);
}

void WINAPI hook_draw_elements_inst(GLenum mode, GLsizei count, GLenum type,
                                     const void* indices, GLsizei instcount) {
    orig_draw_elements_inst(mode, count, type, indices, instcount);
    if (hub_idle() || internal()) return;
    HGLRC hglrc = wglGetCurrentContext();
    if (!hglrc) return;
    DrawEvent ev{}; ev.type = DrawType::DrawIndexedInstanced;
    ev.index_count    = static_cast<uint32_t>(count);
    ev.instance_count = static_cast<uint32_t>(instcount);
    dispatch_draw(hglrc, ev);
}

void WINAPI hook_use_program(GLuint program) {
    orig_use_program(program);
    if (hub_idle() || internal()) return;
    HGLRC hglrc = wglGetCurrentContext();
    if (!hglrc) return;
    HubReader r;
    if (DevState* st = dev_state(hglrc)) {
        st->current_program = program;
        st->program_hash    = make_program_hash(hglrc, program);
    }
}

} // namespace

bool install_draw_hooks() noexcept {
    s_draw_arrays_hook        = vh::api_hook("opengl32.dll", "glDrawArrays",
                                              &hook_draw_arrays,        &orig_draw_arrays);
    s_draw_elements_hook      = vh::api_hook("opengl32.dll", "glDrawElements",
                                              &hook_draw_elements,      &orig_draw_elements);
    s_draw_arrays_inst_hook   = vh::api_hook("opengl32.dll", "glDrawArraysInstanced",
                                              &hook_draw_arrays_inst,   &orig_draw_arrays_inst);
    s_draw_elements_inst_hook = vh::api_hook("opengl32.dll", "glDrawElementsInstanced",
                                              &hook_draw_elements_inst, &orig_draw_elements_inst);
    s_use_program_hook        = vh::api_hook("opengl32.dll", "glUseProgram",
                                              &hook_use_program,        &orig_use_program);
    // The core GL 1.1 hooks (draw_arrays, draw_elements) are essential.
    // The others are optional (not all games use instanced draws or GLSL).
    return orig_draw_arrays && orig_draw_elements;
}

// present.cpp exports this so we can clear it here.
extern void uninstall_present_hooks() noexcept;

void uninstall_all_hooks() noexcept {
    s_draw_arrays_hook        = vh::Hook{};
    s_draw_elements_hook      = vh::Hook{};
    s_draw_arrays_inst_hook   = vh::Hook{};
    s_draw_elements_inst_hook = vh::Hook{};
    s_use_program_hook        = vh::Hook{};
    orig_draw_arrays        = nullptr;
    orig_draw_elements      = nullptr;
    orig_draw_arrays_inst   = nullptr;
    orig_draw_elements_inst = nullptr;
    orig_use_program        = nullptr;
    uninstall_present_hooks();
}

} // namespace vangfx::backend::gl
