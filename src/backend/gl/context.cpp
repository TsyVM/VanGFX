/**
 * src/backend/gl/context.cpp — GLContextImpl + make_gl_context().
 */

#include "gl_impl.hpp"
#include <vangfx/core/shader.hpp>
#include <vangfx/core/error.hpp>

namespace vangfx::backend::gl {

// present.cpp hook (we share uninstall with draw.cpp).
bool install_present_hooks() noexcept;

// ── GLContextImpl ─────────────────────────────────────────────────────────────

GLContextImpl::GLContextImpl(const ContextBuilder& b, HGLRC hglrc)
    : ContextImpl(b), hglrc_(hglrc)
{
    hub_attach(this);
    install_draw_hooks();
    install_present_hooks();
}

GLContextImpl::~GLContextImpl() {
    hub_detach(this);
    if (hub_idle()) uninstall_all_hooks();
}

void GLContextImpl::adopt_hglrc(HGLRC h) noexcept {
    if (!hglrc_) hglrc_ = h;
}

GLContextImpl::Timing GLContextImpl::tick() noexcept {
    std::lock_guard lk(timing_mutex_);
    const auto now   = Clock::now();
    const float time  = std::chrono::duration<float>(now - start_).count();
    const float delta = std::chrono::duration<float>(now - last_).count();
    last_             = now;
    const bool first  = first_frame_;
    first_frame_      = false;
    return { time, delta, first };
}

// ── Shader compilation (GLSL source, no bytecode yet) ────────────────────────

Result<Shader> GLContextImpl::compile_shader(const ShaderBuilder& b) {
    // For now, store the GLSL source text as the "bytecode".
    // A full implementation would call glCreateShader/glCompileShader here via
    // a helper that uses the current GL context, but that couples compilation
    // to the GL thread. Keep it lazy: compile at link_effect time.
    const std::string& src = b.inline_source_text();
    if (src.empty())
        return gfx_err<Shader>(ShaderError::CompileFailed,
            "GLContextImpl::compile_shader: empty shader source");
    Shader s;
    s.handle.id = alloc_id();
    s.bytecode.assign(reinterpret_cast<const uint8_t*>(src.data()),
                      reinterpret_cast<const uint8_t*>(src.data()) + src.size());
    return s;
}

Result<Effect> GLContextImpl::link_effect(Shader vs, Shader ps,
                                           Shader /*gs*/, Shader /*hs*/, Shader /*ds*/) {
    Effect e;
    e.handle.id = alloc_id();
    e.vs = std::move(vs);
    e.ps = std::move(ps);
    return e;
}

Result<Effect> GLContextImpl::link_compute_effect(Shader /*cs*/) {
    return gfx_err<Effect>(ShaderError::Unsupported, "compute shaders not yet implemented on OpenGL");
}

void GLContextImpl::destroy(ShaderHandle h) noexcept { ContextImpl::destroy(h); }
void GLContextImpl::destroy(EffectHandle h) noexcept { ContextImpl::destroy(h); }

// ── GLFrameContextImpl stubs ──────────────────────────────────────────────────

Result<TextureHandle> GLFrameContextImpl::create_texture(const TextureDesc&) {
    return gfx_err<TextureHandle>(ShaderError::Unsupported,
                                   "create_texture not implemented on OpenGL");
}
Result<RenderTargetHandle> GLFrameContextImpl::create_render_target(const RenderTargetDesc&) {
    return gfx_err<RenderTargetHandle>(ShaderError::Unsupported,
                                        "create_render_target not implemented on OpenGL");
}
Result<DepthBufferHandle> GLFrameContextImpl::create_depth_buffer(const DepthBufferDesc&) {
    return gfx_err<DepthBufferHandle>(ShaderError::Unsupported,
                                       "create_depth_buffer not implemented on OpenGL");
}
Result<void> GLFrameContextImpl::bind_effect(const Effect&) {
    return gfx_err<void>(ShaderError::Unsupported, "bind_effect not implemented on OpenGL");
}
Result<void> GLFrameContextImpl::update_constant_buffer(ConstantBufferHandle, const void*, uint32_t) {
    return gfx_err<void>(ShaderError::Unsupported,
                         "update_constant_buffer not implemented on OpenGL");
}
void* GLFrameContextImpl::native_device()  const noexcept { return hglrc_; }
void* GLFrameContextImpl::native_context() const noexcept { return hglrc_; }

} // namespace vangfx::backend::gl

// ── Factory (called from src/core/context.cpp) ────────────────────────────────

namespace vangfx::backend {

using namespace vangfx::backend::gl;

Result<std::unique_ptr<Context>> make_gl_context(const ContextBuilder& b) {
    HGLRC hglrc = static_cast<HGLRC>(b.gl_hglrc());
    // hglrc == nullptr is OK: the context auto-adopts on first wglSwapBuffers.
    return std::make_unique<GLContextImpl>(b, hglrc);
}

} // namespace vangfx::backend
