#pragma once

/**
 * vangfx/fx/depth.hpp — DepthService: finds the depth buffer that the game
 * uses for the main scene.
 *
 * The heuristic: "the depth buffer at back-buffer size that saw the most draws
 * this frame".  Front-end backends call record_draw() on every draw that has a
 * depth surface bound; end_frame() selects the winner.
 *
 * Usage (front-end backend):
 *
 *   // During each draw call:
 *   depth_service_.record_draw(depth_surface_ptr, rt_width, rt_height);
 *
 *   // During Present (before run_present):
 *   depth_service_.end_frame(back_w, back_h);
 *   auto* best = depth_service_.best_depth();   // ID3D11Texture2D*, IDirect3DSurface9*, …
 *   gpu::Texture depth_wrapped = device.wrap_texture(best, ...);
 *
 *   // At the start of the next frame:
 *   depth_service_.begin_frame();
 */

#include <cstdint>
#include <unordered_map>

namespace vangfx::fx {

class DepthService {
public:
    DepthService() = default;

    // Record a draw that had `depth_ptr` bound as depth surface.
    // `width` / `height` are the depth surface's dimensions.
    void record_draw(void* depth_ptr, uint32_t width, uint32_t height) noexcept;

    // Select the best depth buffer and reset draw counts for the next frame.
    // `back_w` / `back_h` are the swap-chain back-buffer dimensions.
    void end_frame(uint32_t back_w, uint32_t back_h) noexcept;

    // The depth surface that won the last end_frame() call, or null.
    [[nodiscard]] void* best_depth() const noexcept { return best_; }

    // Clear everything for the beginning of a new frame.
    void begin_frame() noexcept;

private:
    struct Entry {
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t draws  = 0;
    };
    std::unordered_map<void*, Entry> entries_;
    void*    best_   = nullptr;
};

} // namespace vangfx::fx
