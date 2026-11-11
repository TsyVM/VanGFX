/**
 * src/capture/capture.cpp — CaptureSession and the public capture functions.
 *
 * The running capture itself is the context's Recorder (recorder.hpp), made by
 * Context::start_capture(cfg) and fed by the backends' hooks.
 */

#include <vangfx/capture/capture.hpp>
#include <vangfx/core/context.hpp>
#include <vangfx/core/error.hpp>

#include "recorder.hpp"
#include "../core/context_impl.hpp"

namespace vangfx {

Result<CaptureSession> CaptureSession::begin(Context& ctx, CaptureConfig cfg) {
    if (!cfg.output_dir.empty()) {
        std::error_code ec;
        if (!cfg.output_dir.is_absolute()) cfg.output_dir = std::filesystem::absolute(cfg.output_dir, ec);
        if (!ec) std::filesystem::create_directories(cfg.output_dir, ec);
        if (ec) {
            return gfx_err<CaptureSession>(ShaderError::NotInitialized,
                "CaptureSession: cannot create output directory: " + cfg.output_dir.string() + " (" + ec.message() + ")");
        }
    }
    if (cfg.scope == CaptureScope::GBuffer && cfg.layer_mask == 0)
        return gfx_err<CaptureSession>(ShaderError::NotInitialized, "CaptureSession: layer_mask is 0; nothing to capture");

    auto result = ctx.start_capture(cfg);
    if (!result) return gfx_err<CaptureSession>(result.error().code, result.error().message);
    return Result<CaptureSession>(CaptureSession(&ctx));
}

void CaptureSession::stop() noexcept {
    if (!active_ || !ctx_) return;
    ctx_->stop_capture();
    capture::flush_files();
    active_ = false;
    ctx_    = nullptr;
}

Result<FrameCapture> latest_frame_capture(Context& ctx) {
    auto last = static_cast<ContextImpl&>(ctx).last_capture();
    if (!last) {
        return gfx_err<FrameCapture>(ShaderError::CaptureNotActive,
            "No frame has been captured yet. A capture takes the frame after it starts and is read back at "
            "that frame's Present -- start one with CaptureSession::begin().");
    }
    return *last;
}

void flush_capture_files() noexcept { capture::flush_files(); }

} // namespace vangfx
