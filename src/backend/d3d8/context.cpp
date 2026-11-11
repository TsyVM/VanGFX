/**
 * src/backend/d3d8/context.cpp — D3D8ContextImpl + make_d3d8_context().
 */

#include "d3d8_impl.hpp"

namespace vangfx::backend::d3d8 {

void uninstall_present_hooks(IDirect3DDevice8* dev) noexcept;

// ── D3D8ContextImpl ───────────────────────────────────────────────────────────

D3D8ContextImpl::D3D8ContextImpl(const ContextBuilder& b,
                                   IDirect3DDevice8* device)
    : ContextImpl(b), device_(device)
{
    hub_attach(this);
    if (device_) {
        install_draw_hooks(device_);
        install_present_hooks(device_);
    }
}

D3D8ContextImpl::~D3D8ContextImpl() {
    hub_detach(this);
    if (hub_idle() && device_) {
        uninstall_all_hooks(device_);
        uninstall_present_hooks(device_);
    }
}

D3D8ContextImpl::Timing D3D8ContextImpl::tick() noexcept {
    std::lock_guard lk(timing_mutex_);
    const auto now   = Clock::now();
    const float time  = std::chrono::duration<float>(now - start_).count();
    const float delta = std::chrono::duration<float>(now - last_).count();
    last_             = now;
    const bool first  = first_frame_;
    first_frame_      = false;
    return { time, delta, first };
}

void* D3D8FrameContextImpl::native_device()  const noexcept { return device_; }
void* D3D8FrameContextImpl::native_context() const noexcept { return device_; }

} // namespace vangfx::backend::d3d8

// ── Factory (called from src/core/context.cpp) ────────────────────────────────

namespace vangfx::backend {

using namespace vangfx::backend::d3d8;

Result<std::unique_ptr<Context>> make_d3d8_context(const ContextBuilder& b) {
    auto* device = static_cast<IDirect3DDevice8*>(b.d3d8_device());
    return std::make_unique<D3D8ContextImpl>(b, device);
}

} // namespace vangfx::backend
