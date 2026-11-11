/**
 * src/gpu/vulkan/device_vulkan.cpp — Vulkan gpu::Device stub.
 *
 * Phase 8 stub: returns Unsupported for all operations.
 * A full implementation requires the Vulkan SDK and VK_KHR_dynamic_rendering.
 */

#include <vangfx/gpu/device.hpp>
#include <vangfx/core/error.hpp>

namespace vangfx::gpu::vulkan {

std::unique_ptr<Device> make_vulkan_device(void* /*vk_device*/) {
    return nullptr;
}

} // namespace vangfx::gpu::vulkan
