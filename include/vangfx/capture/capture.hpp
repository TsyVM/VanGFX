#pragma once

/**
 * vangfx/capture/capture.hpp — CaptureConfig and CaptureSession.
 *
 * CaptureConfig is passed to Context::start_capture() (via ContextBuilder or
 * directly) to control which layers are resolved, whether CPU readback is
 * performed, and where captured frames are written.
 *
 * CaptureSession is a RAII wrapper that starts capture on construction and
 * stops it on destruction.
 */

#include "gbuffer.hpp"
#include <vangfx/core/error.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace vangfx {

class Context; // forward

// ─────────────────────────────────────────────────────────────────────────────
//  CaptureFormat — output file format for captured layers
// ─────────────────────────────────────────────────────────────────────────────

enum class CaptureFormat : uint8_t {
    PNG,    // lossless, 8-bit or 16-bit depending on SurfaceFormat
    EXR,    // OpenEXR half/float — preserves HDR data
    DDS,    // raw DDS (preserves compressed/HDR formats exactly)
    Raw,    // raw byte dump, no container
};

// ─────────────────────────────────────────────────────────────────────────────
//  CaptureConfig — parameters for a capture session
// ─────────────────────────────────────────────────────────────────────────────

struct CaptureConfig {
    // ── Layer selection ───────────────────────────────────────────────────────

    // Bitmask of GBufferLayerKind values to resolve.  Default = all.
    uint32_t layer_mask = 0xFFFFFFFF;

    // ── CPU readback ──────────────────────────────────────────────────────────

    // When true, FrameContext::latest_capture() populates GBufferLayer::pixels.
    // Adds a GPU→CPU stall; disable for GPU-only post-processing.
    bool cpu_readback = true;

    // ── Output ────────────────────────────────────────────────────────────────

    // If non-empty, each captured layer is written to this directory.
    // Files are named: <output_dir>/<frame>_<layer_name>.<ext>
    std::filesystem::path output_dir;
    CaptureFormat         output_format = CaptureFormat::PNG;

    // Maximum number of frames to capture before stopping automatically.
    // 0 = run until Context::stop_capture() is called.
    uint32_t max_frames = 1;

    // ── Callback ──────────────────────────────────────────────────────────────

    // Called on the render thread after each frame is fully resolved.
    // Do NOT call any D3D API from inside this callback.
    std::function<void(const GBufferCapture&)> on_frame;

    // ── Helpers ───────────────────────────────────────────────────────────────

    CaptureConfig& capture_layer(GBufferLayerKind kind, bool enable = true) {
        uint32_t bit = 1u << static_cast<uint32_t>(kind);
        if (enable) layer_mask |=  bit;
        else        layer_mask &= ~bit;
        return *this;
    }
    [[nodiscard]] bool wants_layer(GBufferLayerKind kind) const noexcept {
        return (layer_mask >> static_cast<uint32_t>(kind)) & 1u;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  CaptureSession — RAII wrapper for a bounded capture run
//
//  Usage:
//      {
//          CaptureConfig cfg;
//          cfg.max_frames = 5;
//          cfg.output_dir = "captures/";
//
//          auto session = CaptureSession::begin(ctx, cfg);
//          if (!session) { /* handle error */ }
//          // ... game loop runs here ...
//      } // stop_capture() called automatically
// ─────────────────────────────────────────────────────────────────────────────

class CaptureSession {
public:
    CaptureSession()                                 = default;
    CaptureSession(const CaptureSession&)            = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    CaptureSession(CaptureSession&& other) noexcept
        : ctx_(other.ctx_), active_(other.active_) {
        other.ctx_    = nullptr;
        other.active_ = false;
    }
    CaptureSession& operator=(CaptureSession&& other) noexcept {
        stop();
        ctx_    = other.ctx_;
        active_ = other.active_;
        other.ctx_    = nullptr;
        other.active_ = false;
        return *this;
    }

    ~CaptureSession() { stop(); }

    // Factory — starts a capture session on ctx using the given config.
    [[nodiscard]] static Result<CaptureSession>
        begin(Context& ctx, CaptureConfig cfg = {});

    // Explicitly stop the session before destruction.
    void stop() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    explicit CaptureSession(Context* ctx) : ctx_(ctx), active_(true) {}

    Context* ctx_    = nullptr;
    bool     active_ = false;
};

} // namespace vangfx
