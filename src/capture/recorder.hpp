#pragma once

/**
 * src/capture/recorder.hpp — what a running capture follows, and what each
 * backend supplies for it.
 *
 * The backends report three things while a capture runs: each render-target
 * binding (bind), each draw (draw) and, on D3D12, each command list closing
 * and being executed (scope_end, submitted). A "scope" is where bindings live:
 * the D3D9 device, a D3D11 device context, a D3D12 command list. The Recorder
 * keeps, per scope, the targets bound and the draws made since; a binding that
 * drew and is then replaced is the end of a pass, and in Passes scope each of
 * its targets is copied there and then (SnapshotHost::snapshot, recorded into
 * the scope). At Present, every target the frame drew into and the back buffer
 * are copied too, the GPU is waited for, every copy is read, and the frame is
 * handed to the callbacks and to the file writer.
 */

#include <vangfx/capture/capture.hpp>
#include <vangfx/core/context.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vangfx {
class ContextImpl;
}

namespace vangfx::capture {

struct TargetInfo {
    uint64_t      id     = 0;
    SurfaceRole   role   = SurfaceRole::Color;
    SurfaceFormat format = SurfaceFormat::Unknown;
    uint32_t      width  = 0;
    uint32_t      height = 0;
};

struct TargetSet {
    TargetInfo colour[8]{};
    uint32_t   count = 0;
    TargetInfo depth{};   // id 0: none
};

struct Pixels {
    std::vector<uint8_t> bytes;
    SurfaceFormat        format    = SurfaceFormat::Unknown;
    uint32_t             width     = 0;
    uint32_t             height    = 0;
    uint32_t             row_pitch = 0;
};

// A copy of a surface, waiting to be read.
class Snapshot {
public:
    virtual ~Snapshot() = default;
    virtual Result<Pixels> read() = 0;          // after SnapshotHost::flush()
    // Recorded into a command list the GPU runs later (D3D12): only readable
    // once that list has been submitted.
    [[nodiscard]] virtual bool deferred() const noexcept { return false; }
};

// Implemented by each backend's context.
class SnapshotHost {
public:
    virtual ~SnapshotHost() = default;
    // Whether copies can be recorded into `scope` now, with `why` set when not
    // (a D3D11 deferred context: nothing says when its commands run).
    [[nodiscard]] virtual bool can_snapshot_in(const void* scope, std::string& why) noexcept = 0;
    // Whether `scope` is a command list recorded now and run later (D3D12): a
    // pass open in one at Present ends when the list closes, not there.
    [[nodiscard]] virtual bool records_later(const void*) noexcept { return false; }
    // A copy of `t` as it is now, recorded into `scope`, or -- scope null --
    // made on the device or queue at once. Null with `why` when it cannot be.
    virtual std::unique_ptr<Snapshot> snapshot(const void* scope, const TargetInfo& t, std::string& why) = 0;
    // The image the swap chain behind `fc` is about to present, with `info`
    // filled in; only `info` when not `copy` (a capture without readback).
    virtual std::unique_ptr<Snapshot> snapshot_backbuffer(FrameContext& fc, TargetInfo& info, std::string& why, bool copy) = 0;
    // Makes every copy so far readable: waits for the GPU where it must.
    virtual void flush() = 0;
    // Every copy comes back here once read or given up on. A host whose copies
    // run later (D3D12) keeps each until the GPU is done with it.
    virtual void retire(std::unique_ptr<Snapshot> s) { s.reset(); }
};

class Recorder {
public:
    // `whole_frame`: nothing has been drawn since the last Present, so the
    // frame under way is taken; otherwise the next one.
    Recorder(ContextImpl& ctx, CaptureConfig cfg, bool whole_frame);
    ~Recorder();

    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    [[nodiscard]] const CaptureConfig& config() const noexcept { return cfg_; }

    void bind(const void* scope, const TargetSet& set);
    void draw(const void* scope);
    // The targets bound in `scope` are finished with for now (a D3D12 barrier
    // moving one out of its render-target state, or EndRenderPass): the pass
    // ends here; the binding stays.
    void pass_break(const void* scope);
    void scope_end(const void* scope);
    void submitted(const void* scope);
    // Whether `scope` has a binding the recorder saw. A capture that starts
    // after a scope bound its targets has not: the backend binds what is
    // bound now before reporting the draw.
    [[nodiscard]] bool knows(const void* scope);

    // The frame is over: read it back and hand it on. Returns whether the
    // capture has now taken all the frames it was asked for.
    bool present(FrameContext& fc);

private:
    struct Binding {
        TargetSet set{};
        uint32_t  draws = 0;
        bool      open  = false;
    };
    struct Observed {
        TargetInfo info{};
        uint32_t   draws     = 0;
        uint32_t   last_pass = 0;
        uint8_t    slot      = 0;   // MRT slot where first bound (8 = depth)
        uint8_t    set_count = 0;   // targets in that binding
    };
    struct Pending {
        std::unique_ptr<Snapshot> snap;
        TargetInfo                info{};
        uint32_t                  pass  = 0;
        uint32_t                  draws = 0;
        const void*               scope = nullptr;
        bool                      submitted = false;
        std::string               why;
    };

    void observe(const TargetInfo& t, uint8_t slot, uint8_t set_count);
    void end_pass(const void* scope, Binding& b);
    bool within_budget(const TargetInfo& t, std::string& why);
    void reset_frame();

    ContextImpl&   ctx_;
    CaptureConfig  cfg_;
    SnapshotHost*  host_ = nullptr;

    std::mutex                                     mutex_;
    std::unordered_map<const void*, Binding>       scopes_;
    std::vector<Observed>                          observed_;
    std::unordered_map<uint64_t, size_t>           observed_index_;
    std::vector<Pending>                           pending_;
    uint32_t                                       passes_ = 0;
    uint32_t                                       draws_  = 0;
    uint64_t                                       bytes_  = 0;
    uint32_t                                       frames_done_ = 0;
    bool                                           started_ = false;   // taking a whole frame
};

// Guesses the G-buffer layer of each surface (see gbuffer.hpp).
void classify(std::vector<CapturedSurface>& surfaces, const std::vector<uint8_t>& slots,
              const std::vector<uint8_t>& set_counts, uint32_t bb_width, uint32_t bb_height, uint32_t layer_mask);

// "RGBA16F", "D24S8", ...
[[nodiscard]] const char* format_name(SurfaceFormat f) noexcept;

// Background file writing (file_queue.cpp).
void queue_frame_files(std::shared_ptr<const FrameCapture> frame, CaptureFormat format, bool manifest);
void flush_files() noexcept;

}  // namespace vangfx::capture
