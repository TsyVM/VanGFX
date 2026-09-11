#pragma once

/**
 * vangfx/pipeline/viewport.hpp — Viewport and ScissorRect.
 */

#include <cstdint>

namespace vangfx {

struct Viewport {
    float x         = 0.0f;
    float y         = 0.0f;
    float width     = 0.0f;
    float height    = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

struct ScissorRect {
    int32_t left   = 0;
    int32_t top    = 0;
    int32_t right  = 0;
    int32_t bottom = 0;
};

} // namespace vangfx
