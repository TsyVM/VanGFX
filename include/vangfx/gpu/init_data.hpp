#pragma once

#include <cstdint>

namespace vangfx::gpu {

// Upload data for create_texture / create_buffer.
// row_pitch is the row stride in bytes for 2-D textures.
// slice_pitch is the total size of one depth slice for 3-D textures.
// Both may be zero when size is derived from width, height, and format.
struct InitData {
    const void* data        = nullptr;
    uint32_t    row_pitch   = 0;
    uint32_t    slice_pitch = 0;
};

} // namespace vangfx::gpu
