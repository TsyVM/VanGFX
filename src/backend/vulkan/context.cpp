/**
 * src/backend/vulkan/context.cpp — VulkanContextImpl stub + factory.
 *
 * Phase 8 stub: all operations return Unsupported.
 */

#include "vulkan_impl.hpp"
#include <vangfx/core/shader.hpp>
#include <vangfx/core/error.hpp>

namespace vangfx::backend::vulkan {

VulkanContextImpl::VulkanContextImpl(const ContextBuilder& b, void* vk_instance)
    : ContextImpl(b), vk_instance_(vk_instance) {}

VulkanContextImpl::~VulkanContextImpl() = default;

Result<Shader> VulkanContextImpl::compile_shader(const ShaderBuilder&) {
    return gfx_err<Shader>(ShaderError::Unsupported,
        "Vulkan backend: compile_shader not yet implemented");
}

Result<Effect> VulkanContextImpl::link_effect(Shader, Shader, Shader, Shader, Shader) {
    return gfx_err<Effect>(ShaderError::Unsupported,
        "Vulkan backend: link_effect not yet implemented");
}

Result<Effect> VulkanContextImpl::link_compute_effect(Shader) {
    return gfx_err<Effect>(ShaderError::Unsupported,
        "Vulkan backend: link_compute_effect not yet implemented");
}

void VulkanContextImpl::destroy(ShaderHandle h) noexcept { ContextImpl::destroy(h); }
void VulkanContextImpl::destroy(EffectHandle h) noexcept { ContextImpl::destroy(h); }


} // namespace vangfx::backend::vulkan

// ── Factory ───────────────────────────────────────────────────────────────────

namespace vangfx::backend {

using namespace vangfx::backend::vulkan;

Result<std::unique_ptr<Context>> make_vulkan_context(const ContextBuilder& b) {
    return std::make_unique<VulkanContextImpl>(b, b.vk_instance());
}

} // namespace vangfx::backend
