#pragma once

/**
 * src/backend/metal/metal_impl.hpp — Metal front-end backend declarations.
 *
 * Phase 7 stub: the Metal backend is structurally present but all methods
 * return ShaderError::Unsupported until this code is compiled on macOS with
 * the Metal SDK (Xcode 15+, Metal 3+/Metal 4+).
 *
 * The backend becomes usable when VANGFX_METAL=ON and a MTLDevice* (bridged
 * as void*) is supplied via ContextBuilder::metal_device().
 */

#include "../../core/context_impl.hpp"

namespace vangfx::backend::metal {

class MetalContextImpl;

class MetalFrameContextImpl final : public StubFrameContext {
public:
    explicit MetalFrameContextImpl(MetalContextImpl& /*ctx*/, uint32_t frame_index)
        : frame_index_(frame_index) {}

    BackendKind backend()   const noexcept override { return BackendKind::Metal; }
    uint32_t frame_index()  const noexcept override { return frame_index_; }

    void* native_device()  const noexcept override { return device_; }
    void* native_context() const noexcept override { return nullptr; }

private:
    uint32_t frame_index_;
    void*    device_ = nullptr;
};

class MetalContextImpl final : public ContextImpl {
public:
    MetalContextImpl(const ContextBuilder& b, void* metal_device);
    ~MetalContextImpl() override;

    BackendKind backend() const noexcept override { return BackendKind::Metal; }
    std::string post_pass_profile() const override { return "msl_macos"; }

    Result<Shader>  compile_shader(const ShaderBuilder&) override;
    Result<Effect>  link_effect(Shader, Shader, Shader, Shader, Shader) override;
    Result<Effect>  link_compute_effect(Shader) override;
    void destroy(ShaderHandle) noexcept override;
    void destroy(EffectHandle) noexcept override;

private:
    void* metal_device_;
};

} // namespace vangfx::backend::metal
