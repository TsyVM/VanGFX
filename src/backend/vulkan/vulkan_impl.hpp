#pragma once

/**
 * src/backend/vulkan/vulkan_impl.hpp — Vulkan backend declarations.
 *
 * Phase 8 stub: the Vulkan backend is structurally present but all methods
 * return ShaderError::Unsupported.  A full implementation requires the
 * Vulkan SDK headers and a VK_LAYER_VANGFX implicit layer manifest.
 *
 * The backend becomes usable when VANGFX_VULKAN=ON and the Vulkan SDK is
 * available; until then it compiles as an absent-backend stub via the
 * VANGFX_ABSENT_BACKEND macro in context.cpp.
 */

#include "../../core/context_impl.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>

namespace vangfx::backend::vulkan {

class VulkanContextImpl;

class VulkanFrameContextImpl final : public StubFrameContext {
public:
    explicit VulkanFrameContextImpl(VulkanContextImpl& /*ctx*/, uint32_t frame_index)
        : frame_index_(frame_index) {}

    BackendKind backend()   const noexcept override { return BackendKind::Vulkan; }
    uint32_t frame_index()  const noexcept override { return frame_index_; }

    void* native_device()  const noexcept override { return nullptr; }
    void* native_context() const noexcept override { return nullptr; }

private:
    uint32_t frame_index_;
};

class VulkanContextImpl final : public ContextImpl {
public:
    VulkanContextImpl(const ContextBuilder& b, void* vk_instance);
    ~VulkanContextImpl() override;

    BackendKind backend() const noexcept override { return BackendKind::Vulkan; }
    std::string post_pass_profile() const override { return "spirv"; }

    Result<Shader>  compile_shader(const ShaderBuilder&) override;
    Result<Effect>  link_effect(Shader, Shader, Shader, Shader, Shader) override;
    Result<Effect>  link_compute_effect(Shader) override;
    void destroy(ShaderHandle) noexcept override;
    void destroy(EffectHandle) noexcept override;

private:
    void* vk_instance_;
};

} // namespace vangfx::backend::vulkan
