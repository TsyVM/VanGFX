#include <vangfx/fx/depth.hpp>

namespace vangfx::fx {

void DepthService::record_draw(void* depth_ptr, uint32_t width, uint32_t height) noexcept {
    if (!depth_ptr) return;
    auto& e   = entries_[depth_ptr];
    e.width   = width;
    e.height  = height;
    ++e.draws;
}

void DepthService::end_frame(uint32_t back_w, uint32_t back_h) noexcept {
    best_ = nullptr;
    uint32_t best_draws = 0;
    for (auto& [ptr, e] : entries_) {
        if (e.width == back_w && e.height == back_h && e.draws > best_draws) {
            best_draws = e.draws;
            best_      = ptr;
        }
    }
    // Reset draw counts; keep entries so the same pointer doesn't reallocate
    for (auto& [ptr, e] : entries_) e.draws = 0;
}

void DepthService::begin_frame() noexcept {
    entries_.clear();
}

} // namespace vangfx::fx
