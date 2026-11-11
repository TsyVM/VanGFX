/**
 * src/core/context_impl.hpp — ContextImpl, the base every backend's context
 * derives from: callback registration and dispatch, logging, ids, teardown.
 *
 * NOT part of the public SDK. The backend-specific classes live beside their
 * backends (backend/d3d9/d3d9_impl.hpp, d3d11/d3d11_impl.hpp, d3d12/d3d12_impl.hpp).
 *
 * CALLBACK LISTS
 *
 * Each event type keeps its callbacks in an immutable vector behind a
 * shared_ptr. Registering or removing builds a new vector and swaps it in;
 * dispatching takes a reference to the current one and calls through it with
 * no lock held. So a callback may register or remove callbacks (the old design
 * held a shared lock across the call and deadlocked on exactly that), and
 * remove() can wait until no dispatch still holds a vector with the removed
 * callback in it — which is what makes it safe to unload the code afterwards.
 */

#pragma once

#include <vangfx/core/context.hpp>
#include <vangfx/core/error.hpp>
#include <vangfx/core/shader.hpp>
#include <vangfx/capture/capture.hpp>
#include <vangfx/capture/gbuffer.hpp>
#include <vangfx/events/draw.hpp>
#include <vangfx/events/surface.hpp>
#include <vangfx/events/resource.hpp>
#include <vangfx/events/state.hpp>
#include <vangfx/events/query.hpp>
#include <vangfx/d3d12/command.hpp>
#include <vangfx/d3d12/pso.hpp>
#include <vangfx/d3d12/resource.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vangfx::capture {
class Recorder;       // capture/recorder.hpp
class SnapshotHost;
}

namespace vangfx {

// How deep the current thread is inside callback dispatch. remove() does not
// wait for dispatches to drain when called from inside one: the dispatch it
// would be waiting for is the one it is running in.
inline thread_local int t_dispatch_depth = 0;

template <typename Cb>
class CallbackList {
public:
    void add(uint64_t id, Cb cb) {
        std::unique_lock lk(mutex_);
        auto next = std::make_shared<Vec>(list_ ? *list_ : Vec{});
        next->emplace_back(id, std::move(cb));
        list_ = std::move(next);
        any_.store(true, std::memory_order_release);
    }

    // Returns true if `id` was in this list.
    bool remove(uint64_t id) noexcept {
        std::shared_ptr<const Vec> old;
        {
            std::unique_lock lk(mutex_);
            if (!list_) return false;
            bool found = false;
            for (const auto& e : *list_) if (e.first == id) { found = true; break; }
            if (!found) return false;
            auto next = std::make_shared<Vec>();
            next->reserve(list_->size());
            for (const auto& e : *list_) if (e.first != id) next->push_back(e);
            any_.store(!next->empty(), std::memory_order_release);
            old   = std::move(list_);
            list_ = std::move(next);
        }
        // Wait for dispatches still iterating the old vector (each holds one
        // reference to it; ours is the last once they are done).
        if (t_dispatch_depth == 0)
            while (old.use_count() > 1) std::this_thread::yield();
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return !any_.load(std::memory_order_acquire); }

    template <typename... Args>
    void call(Args&... args) const {
        if (!any_.load(std::memory_order_acquire)) return;
        std::shared_ptr<const Vec> snap;
        {
            std::shared_lock lk(mutex_);
            snap = list_;
        }
        if (!snap) return;
        ++t_dispatch_depth;
        for (const auto& e : *snap) {
            // A callback throwing through a Direct3D call would unwind through
            // the runtime's frames. Catch here; the context logs nothing (it
            // may be the logger that threw) but the game keeps running.
            try { e.second(args...); } catch (...) {}
        }
        --t_dispatch_depth;
    }

private:
    using Vec = std::vector<std::pair<uint64_t, Cb>>;
    mutable std::shared_mutex    mutex_;
    std::shared_ptr<const Vec>   list_;
    std::atomic<bool>            any_{ false };
};

// ─────────────────────────────────────────────────────────────────────────────
//  StubFrameContext — FrameContext base that returns Unsupported for every
//  pure virtual.  New backends derive from this and override what they need.
// ─────────────────────────────────────────────────────────────────────────────

class StubFrameContext : public FrameContext {
protected:
    static Result<ReadbackData>   not_impl()  { return gfx_err<ReadbackData>(ShaderError::Unsupported, "not implemented"); }
    static Result<BufferData>     not_buf()   { return gfx_err<BufferData>(ShaderError::Unsupported, "not implemented"); }
public:
    FrameSurfaces surfaces()  const noexcept override { return {}; }
    PipelineState pipeline()  const noexcept override { return {}; }
    SurfaceFormat format_of(RenderTargetHandle) const noexcept override { return {}; }
    SurfaceFormat format_of(DepthBufferHandle)  const noexcept override { return {}; }
    SurfaceFormat format_of(TextureHandle)      const noexcept override { return {}; }
    Result<TextureHandle>     create_texture(const TextureDesc&)           override { return gfx_err<TextureHandle>(ShaderError::Unsupported, "stub"); }
    Result<RenderTargetHandle> create_render_target(const RenderTargetDesc&) override { return gfx_err<RenderTargetHandle>(ShaderError::Unsupported, "stub"); }
    Result<DepthBufferHandle>  create_depth_buffer(const DepthBufferDesc&)   override { return gfx_err<DepthBufferHandle>(ShaderError::Unsupported, "stub"); }
    void destroy(TextureHandle)      noexcept override {}
    void destroy(RenderTargetHandle) noexcept override {}
    void destroy(DepthBufferHandle)  noexcept override {}
    Result<void> bind_effect(const Effect&) override { return gfx_err<void>(ShaderError::Unsupported, "stub"); }
    void unbind_effect() noexcept override {}
    Result<void> update_constant_buffer(ConstantBufferHandle, const void*, uint32_t) override { return gfx_err<void>(ShaderError::Unsupported, "stub"); }
    Result<ReadbackData> readback(TextureHandle)       override { return not_impl(); }
    Result<ReadbackData> readback(RenderTargetHandle)  override { return not_impl(); }
    Result<ReadbackData> readback(DepthBufferHandle)   override { return not_impl(); }
    Result<BufferData>   readback(VertexBufferHandle,     uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(IndexBufferHandle,      uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(ConstantBufferHandle,   uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(StructuredBufferHandle, uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(RawBufferHandle,        uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(UAVHandle,              uint64_t, uint64_t) override { return not_buf(); }
    BufferRef            buffer_at(uint64_t) const noexcept override { return {}; }
    Result<BufferData>   readback_at(uint64_t, uint64_t) override { return not_buf(); }
    bool                   capture_active() const noexcept override { return false; }
    Result<GBufferCapture> latest_capture()       override { return gfx_err<GBufferCapture>(ShaderError::Unsupported, "stub"); }
    void* native_device()  const noexcept override { return nullptr; }
    void* native_context() const noexcept override { return nullptr; }
    void* native_resource(RenderTargetHandle) const noexcept override { return nullptr; }
    void* native_resource(DepthBufferHandle)  const noexcept override { return nullptr; }
    void* native_resource(TextureHandle)      const noexcept override { return nullptr; }
    void* native_buffer(uint64_t)             const noexcept override { return nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ContextImpl — shared base class for all three backends
// ─────────────────────────────────────────────────────────────────────────────

class ContextImpl : public Context {
public:
    explicit ContextImpl(const ContextBuilder& b);
    ~ContextImpl() override;

    // ── Callback registration ─────────────────────────────────────────────────
    CallbackId on_draw         (DrawCallback         cb) override;
    CallbackId on_render_target(RenderTargetCallback cb) override;
    CallbackId on_depth        (DepthCallback        cb) override;
    CallbackId on_present      (PresentCallback      cb) override;
    CallbackId on_state_change (StateCallback        cb) override;
    CallbackId on_resource     (ResourceCallback     cb) override;
    CallbackId on_query        (QueryCallback        cb) override;
    CallbackId on_device_lost  (DeviceLostCallback   cb) override;
    CallbackId on_device_reset (DeviceResetCallback  cb) override;
    CallbackId on_command_list    (CommandListCallback      cb) override;
    CallbackId on_pso_create      (PSOCreateCallback        cb) override;
    CallbackId on_resource_barrier(ResourceBarrierCallback  cb) override;
    void remove_callback(CallbackId id) noexcept override;

    // ── Logging ───────────────────────────────────────────────────────────────
    void set_log_level(LogLevel level) noexcept override;
    void set_log_callback(std::function<void(LogLevel, std::string_view)> cb) override;
    void log(LogLevel level, std::string_view msg) noexcept;
    [[nodiscard]] bool logs(LogLevel level) const noexcept {
        return level <= log_level_.load(std::memory_order_relaxed);
    }

    // ── Capture ───────────────────────────────────────────────────────────────
    Result<void> start_capture() override;
    Result<void> start_capture(const CaptureConfig& cfg) override;
    void         stop_capture() noexcept override;
    bool         capture_active() const noexcept override;
    bool         auto_capture() const noexcept { return auto_capture_; }

    // The running capture, which the backends' hooks feed; null when none.
    [[nodiscard]] std::shared_ptr<capture::Recorder> recorder() const noexcept {
        return capture_active_.load(std::memory_order_acquire) ? recorder_.load(std::memory_order_acquire) : nullptr;
    }
    // What copies surfaces for a capture; each backend's context supplies one.
    [[nodiscard]] virtual capture::SnapshotHost* snapshot_host() noexcept { return nullptr; }
    // A present hook's part: resolves the frame a capture was taking (and
    // stops it when it has all its frames), then starts an auto-capture on
    // the first frame.
    void capture_present(FrameContext& fc, bool first_frame);
    // Every draw into targets, whether or not a capture runs: a capture begun
    // before the frame's first draw takes that frame rather than the next.
    void note_draw() noexcept { frame_draws_.fetch_add(1, std::memory_order_relaxed); }

    [[nodiscard]] std::shared_ptr<const FrameCapture> last_capture() const {
        std::lock_guard lk(last_mutex_);
        return last_capture_;
    }
    void set_last_capture(std::shared_ptr<const FrameCapture> c) {
        std::lock_guard lk(last_mutex_);
        last_capture_ = std::move(c);
    }
    // FrameContext::latest_capture() on every backend.
    [[nodiscard]] Result<GBufferCapture> latest_gbuffer() const;

    // ── Ids ───────────────────────────────────────────────────────────────────
    // Handle ids come from one process-wide counter, so an id names one object
    // whichever context reports it.
    static uint64_t alloc_id() noexcept;

    // ── Teardown ──────────────────────────────────────────────────────────────
    // Work to run when this context is destroyed, newest first: the effect and
    // capture layers keep per-context state keyed on the context, and this is
    // how it goes away with it rather than lingering for the next context that
    // happens to be allocated at the same address.
    void add_teardown(std::function<void()> fn);

    // ── Post-processing passes ────────────────────────────────────────────────
    // Full-screen pixel shaders run over the back buffer at Present, in the
    // order added (effect/inject.cpp adds them; each backend's present hook
    // runs them). `ps` is compiled for the backend's own post-pass pipeline.
    struct PostPass {
        uint64_t             id = 0;
        std::vector<uint8_t> ps;
    };
    using PostPassList = std::vector<PostPass>;
    uint64_t add_post_pass(std::vector<uint8_t> ps_bytecode);
    bool     remove_post_pass(uint64_t id) noexcept;
    [[nodiscard]] std::shared_ptr<const PostPassList> post_passes() const noexcept {
        std::lock_guard lk(post_mutex_);
        return post_passes_;
    }
    // The eight vangfx_float4 slots post passes see (vangfx::Uniforms).
    using Float4Array = std::array<std::array<float, 4>, 8>;
    void set_uniform(uint32_t slot, const std::array<float, 4>& v) noexcept {
        if (slot >= 8) return;
        std::lock_guard lk(post_mutex_);
        uniforms_[slot] = v;
    }
    [[nodiscard]] Float4Array uniforms() const noexcept {
        std::lock_guard lk(post_mutex_);
        return uniforms_;
    }

    // The shader profile post-pass shaders are compiled for on this backend.
    [[nodiscard]] virtual std::string post_pass_profile() const { return "ps_5_0"; }

    // ── Stubs — overridden by backend subclasses ──────────────────────────────
    void destroy(ShaderHandle h)  noexcept override;
    void destroy(EffectHandle  h) noexcept override;
    Result<Shader> compile_shader(const ShaderBuilder&) override;
    Result<Effect> link_effect(Shader,Shader,Shader,Shader,Shader) override;
    Result<Effect> link_compute_effect(Shader) override;

    // Context-level native device pointer; backends override with their own.
    void* native_device() const noexcept override { return nullptr; }

    BackendKind backend()               const noexcept override;
    uint32_t    d3d9_ver()              const noexcept override;
    uint32_t    d3d11_feature_level()   const noexcept override;
    uint32_t    d3d12_shader_model()    const noexcept override;
    uint32_t    d3d12_raytracing_tier() const noexcept override;
    uint32_t    d3d12_mesh_shader_tier()const noexcept override;

    // ── Dispatch (used by the backends' hook thunks) ──────────────────────────
    void dispatch_draw            (FrameContext& fc, DrawEvent& ev)         { draw_cbs_.call(fc, ev); }
    void dispatch_render_target   (FrameContext& fc, RenderTargetEvent& ev) { rt_cbs_.call(fc, ev); }
    void dispatch_depth           (FrameContext& fc, DepthEvent& ev)        { depth_cbs_.call(fc, ev); }
    void dispatch_present         (FrameContext& fc, PresentEvent& ev)      { present_cbs_.call(fc, ev); }
    void dispatch_state           (FrameContext& fc, StateChangeEvent& ev)  { state_cbs_.call(fc, ev); }
    void dispatch_resource        (FrameContext& fc, ResourceEvent& ev)     { res_cbs_.call(fc, ev); }
    void dispatch_query           (FrameContext& fc, QueryEvent& ev)        { query_cbs_.call(fc, ev); }
    void dispatch_device_lost     ()                                        { lost_cbs_.call(); }
    void dispatch_device_reset    (FrameContext& fc)                        { reset_cbs_.call(fc); }
    void dispatch_command_list    (FrameContext& fc, d3d12::CommandListEvent& ev) { cmd_cbs_.call(fc, ev); }
    void dispatch_pso_create      (FrameContext& fc, d3d12::PSOCreateEvent& ev)   { pso_cbs_.call(fc, ev); }
    void dispatch_resource_barrier(FrameContext& fc, ResourceBarrierBatch& b)     { barrier_cbs_.call(fc, b); }

    // Whether anything listens, so a thunk can skip building an event nobody reads.
    [[nodiscard]] bool wants_draw()     const noexcept { return !draw_cbs_.empty(); }
    [[nodiscard]] bool wants_rt()       const noexcept { return !rt_cbs_.empty() || !depth_cbs_.empty() || capture_active(); }
    [[nodiscard]] bool wants_state()    const noexcept { return !state_cbs_.empty(); }
    [[nodiscard]] bool wants_resource() const noexcept { return !res_cbs_.empty(); }
    [[nodiscard]] bool wants_query()    const noexcept { return !query_cbs_.empty(); }
    [[nodiscard]] bool wants_command()  const noexcept { return !cmd_cbs_.empty(); }
    [[nodiscard]] bool wants_pso()      const noexcept { return !pso_cbs_.empty(); }
    [[nodiscard]] bool wants_present()  const noexcept { return !present_cbs_.empty(); }
    [[nodiscard]] bool wants_barrier()  const noexcept { return !barrier_cbs_.empty(); }

protected:
    // Backends call this first thing in their destructor, before any hook
    // state goes, so teardown work sees a context that still works.
    void run_teardown() noexcept;

    std::atomic<LogLevel> log_level_;
    bool                  auto_capture_ = false;
    std::atomic<bool>     capture_active_{ false };
    std::atomic<uint32_t> frame_draws_{ 0 };   // since the last Present
    std::atomic<std::shared_ptr<capture::Recorder>> recorder_;
    mutable std::mutex                              last_mutex_;
    std::shared_ptr<const FrameCapture>             last_capture_;

private:
    template <typename Cb>
    CallbackId add(CallbackList<Cb>& list, Cb cb) {
        const CallbackId id{ next_callback_.fetch_add(1, std::memory_order_relaxed) };
        list.add(id.id, std::move(cb));
        return id;
    }

    std::atomic<uint64_t> next_callback_{ 1 };

    CallbackList<DrawCallback>            draw_cbs_;
    CallbackList<RenderTargetCallback>    rt_cbs_;
    CallbackList<DepthCallback>           depth_cbs_;
    CallbackList<PresentCallback>         present_cbs_;
    CallbackList<StateCallback>           state_cbs_;
    CallbackList<ResourceCallback>        res_cbs_;
    CallbackList<QueryCallback>           query_cbs_;
    CallbackList<DeviceLostCallback>      lost_cbs_;
    CallbackList<DeviceResetCallback>     reset_cbs_;
    CallbackList<CommandListCallback>     cmd_cbs_;
    CallbackList<PSOCreateCallback>       pso_cbs_;
    CallbackList<ResourceBarrierCallback> barrier_cbs_;

    using LogFn = std::function<void(LogLevel, std::string_view)>;
    mutable std::mutex           log_mutex_;
    std::shared_ptr<const LogFn> log_cb_;

    std::mutex                         teardown_mutex_;
    std::vector<std::function<void()>> teardown_;
    bool                               torn_down_ = false;

    mutable std::mutex                  post_mutex_;
    std::shared_ptr<const PostPassList> post_passes_ = std::make_shared<const PostPassList>();
    Float4Array                         uniforms_{};
};

} // namespace vangfx
