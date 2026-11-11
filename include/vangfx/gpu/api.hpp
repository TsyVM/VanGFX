#pragma once

#include <cstdint>

namespace vangfx::gpu {

enum class Api : uint8_t {
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    Vulkan,
    GL,
    GLES,
    Metal,
};

} // namespace vangfx::gpu
