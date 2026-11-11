#pragma once

/**
 * src/backend/gles/gles_impl.hpp — GLES/EGL backend declarations.
 *
 * Phase 8 stub: the GLES backend is structurally present but all methods
 * return ShaderError::Unsupported.
 *
 * GLES is primarily an Android target.  On Windows it requires a GLES
 * emulation layer (e.g. ANGLE) which is not in scope for the initial release.
 *
 * The backend becomes usable when VANGFX_GLES=ON and an EGLDisplay /
 * EGLContext pair is supplied via ContextBuilder::egl_display() /
 * ContextBuilder::egl_context().
 */

#include "../../core/context_impl.hpp"

namespace vangfx::backend::gles {

class GLESContextImpl;

class GLESFrameContextImpl final : public StubFrameContext {
public:
    explicit GLESFrameContextImpl(GLESContextImpl& /*ctx*/, uint32_t frame_index)
        : frame_index_(frame_index) {}

    BackendKind backend()   const noexcept override { return BackendKind::GLES; }
    uint32_t frame_index()  const noexcept override { return frame_index_; }

    void* native_device()  const noexcept override { return nullptr; }
    void* native_context() const noexcept override { return nullptr; }

private:
    uint32_t frame_index_;
};

class GLESContextImpl final : public ContextImpl {
public:
    GLESContextImpl(const ContextBuilder& b, void* egl_display, void* egl_context);
    ~GLESContextImpl() override;

    BackendKind backend() const noexcept override { return BackendKind::GLES; }
    std::string post_pass_profile() const override { return "glsl_es_300"; }

    Result<Shader>  compile_shader(const ShaderBuilder&) override;
    Result<Effect>  link_effect(Shader, Shader, Shader, Shader, Shader) override;
    Result<Effect>  link_compute_effect(Shader) override;
    void destroy(ShaderHandle) noexcept override;
    void destroy(EffectHandle) noexcept override;

private:
    void* egl_display_;
    void* egl_context_;
};

} // namespace vangfx::backend::gles
