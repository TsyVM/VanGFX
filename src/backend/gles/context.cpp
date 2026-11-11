/**
 * src/backend/gles/context.cpp — GLESContextImpl stub + factory.
 *
 * Phase 8 stub: all operations return Unsupported.
 */

#include "gles_impl.hpp"
#include <vangfx/core/shader.hpp>
#include <vangfx/core/error.hpp>

namespace vangfx::backend::gles {

GLESContextImpl::GLESContextImpl(const ContextBuilder& b,
                                  void* egl_display, void* egl_context)
    : ContextImpl(b), egl_display_(egl_display), egl_context_(egl_context) {}

GLESContextImpl::~GLESContextImpl() = default;

Result<Shader> GLESContextImpl::compile_shader(const ShaderBuilder&) {
    return gfx_err<Shader>(ShaderError::Unsupported,
        "GLES backend: compile_shader not yet implemented");
}

Result<Effect> GLESContextImpl::link_effect(Shader, Shader, Shader, Shader, Shader) {
    return gfx_err<Effect>(ShaderError::Unsupported,
        "GLES backend: link_effect not yet implemented");
}

Result<Effect> GLESContextImpl::link_compute_effect(Shader) {
    return gfx_err<Effect>(ShaderError::Unsupported,
        "GLES backend: link_compute_effect not yet implemented");
}

void GLESContextImpl::destroy(ShaderHandle h) noexcept { ContextImpl::destroy(h); }
void GLESContextImpl::destroy(EffectHandle h) noexcept { ContextImpl::destroy(h); }


} // namespace vangfx::backend::gles

// ── Factory ───────────────────────────────────────────────────────────────────

namespace vangfx::backend {

using namespace vangfx::backend::gles;

Result<std::unique_ptr<Context>> make_gles_context(const ContextBuilder& b) {
    return std::make_unique<GLESContextImpl>(b, b.egl_display(), b.egl_context());
}

} // namespace vangfx::backend
