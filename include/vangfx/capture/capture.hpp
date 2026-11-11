#pragma once

/**
 * vangfx/capture/capture.hpp — capturing what a frame rendered.
 *
 * A capture reads back the surfaces a frame drew into and hands them over as a
 * FrameCapture (and, sorted into layers, a GBufferCapture), optionally writing
 * each to a file. What it takes is the CaptureScope:
 *
 *   Targets  every render target and depth buffer the frame drew into, as the
 *            frame left them, and the back buffer. The default.
 *   Passes   everything Targets takes, plus a copy of each target at the end
 *            of every pass that drew into it: a shadow map, a bloom chain, a
 *            G-buffer the lighting pass later overwrites -- what a single
 *            end-of-frame read cannot see. A pass ends when the frame binds
 *            other targets (on D3D12, when a command list closes too).
 *   GBuffer  only the targets the G-buffer view recognises, plus depth and
 *            the back buffer.
 *
 * Every backend: D3D9 (depth-stencil surfaces excepted -- D3D9 cannot read one
 * back; render depth into an INTZ texture to see it), D3D11 (passes drawn on
 * deferred contexts are taken at the end of the frame only), and D3D12, where
 * VanGFX follows every resource's state through the barriers the game records
 * so each copy starts from the state the resource is actually in.
 *
 *     vangfx::CaptureConfig cfg;
 *     cfg.scope      = vangfx::CaptureScope::Passes;
 *     cfg.output_dir = "captures";
 *     auto session   = vangfx::CaptureSession::begin(*ctx, cfg);   // the next frame
 *
 * Files go to <output_dir>/frame_<index>/, one per surface, named after the
 * pass, the resource id, its role and G-buffer layer (p003_rt1234_Color_Albedo.png,
 * backbuffer.png), with a capture.json describing each. They are written on a
 * background thread; CaptureSession::stop() and flush_capture_files() wait for
 * them.
 */

#include "gbuffer.hpp"
#include <vangfx/core/error.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vangfx {

class Context; // forward

// ─────────────────────────────────────────────────────────────────────────────
//  CaptureFormat — output file format
// ─────────────────────────────────────────────────────────────────────────────

enum class CaptureFormat : uint8_t {
    PNG,    // what a viewer shows: 8-bit sources exact, deeper ones 16-bit (float colour in sRGB,
            // depth and data channels stretched over their range)
    EXR,    // OpenEXR, the values themselves: HALF or FLOAT, ZIP-compressed
    DDS,    // the bytes, under a header naming the exact DXGI format
    Raw,    // the bytes, rows packed, nothing else
};

enum class CaptureScope : uint8_t {
    Targets,   // every target and depth buffer at the end of the frame, and the back buffer
    Passes,    // Targets, plus each target at the end of every pass that drew into it
    GBuffer,   // the recognised G-buffer layers, depth and the back buffer
};

// What a captured surface was to the frame.
enum class SurfaceRole : uint8_t {
    Color,        // a render target
    Depth,        // a depth-stencil buffer
    Backbuffer,   // the swap chain's image
};

// ─────────────────────────────────────────────────────────────────────────────
//  CapturedSurface / FrameCapture — the result
// ─────────────────────────────────────────────────────────────────────────────

struct CapturedSurface {
    uint64_t         resource_id = 0;   // as resource and render-target events name it (0: unknown)
    SurfaceRole      role        = SurfaceRole::Color;
    GBufferLayerKind kind        = GBufferLayerKind::Unknown;   // the G-buffer view's reading of it
    SurfaceFormat    format      = SurfaceFormat::Unknown;      // of `pixels` (a depth-stencil's depth plane)
    uint32_t         width       = 0;
    uint32_t         height      = 0;
    uint32_t         pass        = 0;   // the pass this copy ends (1 = first); 0 = the end of the frame
    uint32_t         draws       = 0;   // draws into it: in that pass, or over the whole frame
    bool             available   = false;
    std::span<const uint8_t> pixels;    // rows `row_pitch` bytes apart
    uint32_t         row_pitch   = 0;
    std::string      file;              // where it is being written, when output_dir is set
    std::string      note;              // why it is not available, or how its file converts it
};

struct FrameCapture {
    uint32_t frame_index = 0;
    uint32_t width       = 0;   // back buffer size
    uint32_t height      = 0;
    uint32_t passes      = 0;   // passes the frame had (Passes scope; bindings that drew)
    uint32_t draws       = 0;   // draws the frame made
    std::filesystem::path folder;   // where the files went, when output_dir is set

    // End-of-frame copies first, in the order the frame first drew into each;
    // then, in Passes scope, the pass copies in pass order.
    std::vector<CapturedSurface> surfaces;
    GBufferCapture               gbuffer;

    // Keeps the pixel memory alive; copies of the capture share it.
    std::shared_ptr<const void> storage;

    // The end-of-frame copy of a resource (pass 0), or a given pass's copy.
    [[nodiscard]] const CapturedSurface* find(uint64_t resource_id, uint32_t pass = 0) const noexcept {
        for (const auto& s : surfaces)
            if (s.resource_id == resource_id && s.pass == pass) return &s;
        return nullptr;
    }
    [[nodiscard]] const CapturedSurface* backbuffer() const noexcept {
        for (const auto& s : surfaces)
            if (s.role == SurfaceRole::Backbuffer) return &s;
        return nullptr;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  CaptureConfig — parameters for a capture session
// ─────────────────────────────────────────────────────────────────────────────

struct CaptureConfig {
    CaptureScope scope = CaptureScope::Targets;

    // G-buffer layers to keep (bits of GBufferLayerKind). Filters the G-buffer
    // view, and in GBuffer scope which surfaces are read at all.
    uint32_t layer_mask = 0xFFFFFFFF;

    // Whether to read pixels back at all. Off, a capture still lists every
    // surface -- id, role, format, size, draws, layer -- without the stall.
    bool cpu_readback = true;

    // Files: none when empty. One folder per frame (see the top of this header).
    std::filesystem::path output_dir;
    CaptureFormat         output_format = CaptureFormat::PNG;
    bool                  write_manifest = true;   // capture.json beside the files

    // Frames to capture before stopping by itself (0 = until stop_capture()).
    uint32_t max_frames = 1;

    // Pixel memory one frame's capture may read back; surfaces past it are
    // listed with a note instead. 1 GiB.
    uint64_t max_bytes = 1ull << 30;

    // Called on the render thread once each frame is read back. Do not call
    // Direct3D from them.
    std::function<void(const FrameCapture&)>   on_capture;
    std::function<void(const GBufferCapture&)> on_frame;   // the G-buffer view of the same frame

    CaptureConfig& capture_layer(GBufferLayerKind kind, bool enable = true) {
        const uint32_t bit = 1u << static_cast<uint32_t>(kind);
        if (enable) layer_mask |= bit; else layer_mask &= ~bit;
        return *this;
    }
    [[nodiscard]] bool wants_layer(GBufferLayerKind kind) const noexcept {
        return (layer_mask >> static_cast<uint32_t>(kind)) & 1u;
    }
};

// The most recent capture on a context, or CaptureNotActive before the first.
[[nodiscard]] Result<FrameCapture> latest_frame_capture(Context& ctx);

// Waits until every file captures have queued is written.
void flush_capture_files() noexcept;

// ─────────────────────────────────────────────────────────────────────────────
//  CaptureSession — RAII wrapper for a bounded capture run
//
//      {
//          vangfx::CaptureConfig cfg;
//          cfg.max_frames = 5;
//          cfg.output_dir = "captures/";
//          auto session = vangfx::CaptureSession::begin(ctx, cfg);
//          if (!session) { /* handle error */ }
//          // ... frames render ...
//      } // stopped, and its files written, here
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

    // Starts capturing on `ctx` from its next frame.
    [[nodiscard]] static Result<CaptureSession> begin(Context& ctx, CaptureConfig cfg = {});

    // Stops (if the frame count has not already) and waits for the files.
    void stop() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    explicit CaptureSession(Context* ctx) : ctx_(ctx), active_(true) {}

    Context* ctx_    = nullptr;
    bool     active_ = false;
};

} // namespace vangfx
