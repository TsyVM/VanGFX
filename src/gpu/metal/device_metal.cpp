/**
 * src/gpu/metal/device_metal.cpp — Metal gpu::Device stub.
 *
 * Phase 7 stub. Full implementation requires macOS + Metal SDK.
 */

#include <vangfx/gpu/device.hpp>
#include <vangfx/core/error.hpp>

namespace vangfx::gpu::metal {

std::unique_ptr<Device> make_metal_device(void* /*mtl_device*/) { return nullptr; }

} // namespace vangfx::gpu::metal
