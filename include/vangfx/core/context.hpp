#pragma once

/**
 * vangfx/core/context.hpp — Context, ContextBuilder, FrameContext, BackendKind.
 *
 * Context is the root object for the VanGFX SDK. Create it once per device via
 * ContextBuilder::build() (or let vangfx::AutoAttach create it when the game
 * creates its device). It hooks D3D9, D3D11 or D3D12 by patching the methods'
 * vtable slots once per class; no DLL proxy is needed.
 *
 * FrameContext is handed to every event callback and exposes per-frame
 * queries, resource access, readback, shader/effect binding, and the native
 * Direct3D objects behind the event.
 *
 * Callbacks run on whichever thread made the Direct3D call -- on D3D12 that is
 * every thread recording command lists. Do not destroy the Context from inside
 * one of its own callbacks.
 */

#include "error.hpp"
#include "shader.hpp"
#include <vangfx/resources/buffer.hpp>
#include <vangfx/resources/surface.hpp>
#include <vangfx/resources/texture.hpp>
#include <vangfx/pipeline/state.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Forward-declarations of event types to avoid circular includes.
namespace vangfx {
    struct DrawEvent;
    struct RenderTargetEvent;
    struct DepthEvent;
    struct PresentEvent;
    struct StateChangeEvent;
    struct ResourceEvent;
    struct QueryEvent;
    struct GBufferCapture;
    struct CaptureConfig;
}
namespace vangfx::d3d12 {
    struct CommandListEvent;   // d3d12/command.hpp
    struct PSOCreateEvent;     // d3d12/pso.hpp
    struct ResourceBarrier;    // d3d12/resource.hpp
}

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  BackendKind — which D3D runtime is being intercepted
// ─────────────────────────────────────────────────────────────────────────────

enum class BackendKind : uint8_t {
    D3D9,
    D3D10,
    D3D11,
    D3D12,
    OpenGL,  // OpenGL WGL front end (via VanHooks)
    D3D8,    // Direct3D 8 front end + d3d8_to_d3d9 translator
    Vulkan,  // Vulkan implicit layer
    GLES,    // GLES/EGL (Android NDK 29)
    Metal,   // Metal 3/4 (macOS/iOS — experimental, macOS build only)
    Android, // Injected .so auto-attach for GLES/Vulkan Android games (SHAR)
    Auto,    // detect at build() time (default)
};

// ─────────────────────────────────────────────────────────────────────────────
//  LogLevel
// ─────────────────────────────────────────────────────────────────────────────

enum class LogLevel : uint8_t { Off, Error, Warn, Info, Debug, Trace };

// ─────────────────────────────────────────────────────────────────────────────
//  FrameContext — per-frame state and operations, passed to every callback
// ─────────────────────────────────────────────────────────────────────────────

class Context; // forward

class FrameContext {
public:
    virtual ~FrameContext() = default;

    // ── Backend query ─────────────────────────────────────────────────────────

    [[nodiscard]] virtual BackendKind backend()      const noexcept = 0;
    [[nodiscard]] virtual uint32_t    frame_index()  const noexcept = 0;  // monotonic

    // ── Surface / resource queries ────────────────────────────────────────────

    [[nodiscard]] virtual FrameSurfaces surfaces()   const noexcept = 0;
    [[nodiscard]] virtual PipelineState pipeline()   const noexcept = 0;

    // Returns the format of an existing render-target handle, or Unknown.
    [[nodiscard]] virtual SurfaceFormat
        format_of(RenderTargetHandle rt)             const noexcept = 0;
    [[nodiscard]] virtual SurfaceFormat
        format_of(DepthBufferHandle db)              const noexcept = 0;
    [[nodiscard]] virtual SurfaceFormat
        format_of(TextureHandle tex)                 const noexcept = 0;

    // ── Texture / buffer creation ─────────────────────────────────────────────

    [[nodiscard]] virtual Result<TextureHandle>
        create_texture(const TextureDesc& desc)             = 0;

    [[nodiscard]] virtual Result<RenderTargetHandle>
        create_render_target(const RenderTargetDesc& desc)  = 0;

    [[nodiscard]] virtual Result<DepthBufferHandle>
        create_depth_buffer(const DepthBufferDesc& desc)    = 0;

    virtual void destroy(TextureHandle h)       noexcept = 0;
    virtual void destroy(RenderTargetHandle h)  noexcept = 0;
    virtual void destroy(DepthBufferHandle h)   noexcept = 0;

    // ── Shader / Effect binding ───────────────────────────────────────────────

    // Install a pre-built Effect and return true on success.
    // Restores prior shaders on the next call to unbind_effect().
    virtual Result<void> bind_effect(const Effect& effect)  = 0;
    virtual void         unbind_effect()              noexcept = 0;

    // Update a constant buffer with raw data.
    virtual Result<void> update_constant_buffer(
        ConstantBufferHandle handle,
        const void*          data,
        uint32_t             size)                          = 0;

    // ── Readback ──────────────────────────────────────────────────────────────

    // Reads back a GPU texture to CPU-visible memory.
    // The returned ReadbackData is valid until the next call on this
    // FrameContext or until the frame ends.
    [[nodiscard]] virtual Result<ReadbackData>
        readback(TextureHandle tex)                         = 0;
    [[nodiscard]] virtual Result<ReadbackData>
        readback(RenderTargetHandle rt)                     = 0;
    [[nodiscard]] virtual Result<ReadbackData>
        readback(DepthBufferHandle db)                      = 0;

    // ── Buffer readback ───────────────────────────────────────────────────────
    //
    // The same operation for the resources that are not images. ResourceEvent
    // has always handed out a handle for every buffer the application creates
    // -- vertex, index, constant, structured, raw, UAV -- and until these
    // existed there was nothing that accepted one, so the handle was a name for
    // something unreachable.
    //
    // Reading the index buffer behind a draw is how you work out what that draw
    // is, which is the question anyone modifying a game's rendering has to
    // answer first: a frame is thousands of draws and no label says which is the
    // car. Reading a constant buffer is how you get the matrices.
    //
    // `size` of 0 means "to the end of the buffer" and `offset` is in bytes;
    // both are clamped to the buffer, so asking for more than exists returns
    // what exists rather than failing. A window is worth asking for: these
    // stall the GPU, and a 4 MB vertex buffer copied to read 64 bytes of it is
    // the same stall as copying the 64 bytes.
    [[nodiscard]] virtual Result<BufferData>
        readback(VertexBufferHandle vb,
                 uint64_t offset = 0, uint64_t size = 0)     = 0;
    [[nodiscard]] virtual Result<BufferData>
        readback(IndexBufferHandle ib,
                 uint64_t offset = 0, uint64_t size = 0)     = 0;
    [[nodiscard]] virtual Result<BufferData>
        readback(ConstantBufferHandle cb,
                 uint64_t offset = 0, uint64_t size = 0)     = 0;
    [[nodiscard]] virtual Result<BufferData>
        readback(StructuredBufferHandle sb,
                 uint64_t offset = 0, uint64_t size = 0)     = 0;
    [[nodiscard]] virtual Result<BufferData>
        readback(RawBufferHandle rb,
                 uint64_t offset = 0, uint64_t size = 0)     = 0;
    [[nodiscard]] virtual Result<BufferData>
        readback(UAVHandle uav,
                 uint64_t offset = 0, uint64_t size = 0)     = 0;

    // Which registered buffer contains a GPU virtual address, and where in it.
    // The bridge from a DrawEvent to the memory behind that draw:
    //
    //     auto ref = fc.buffer_at(ev.d3d12_ib.view.gpu_virtual_address);
    //     if (ref) auto r = fc.readback(ref.as_index_buffer(), ref.offset, 96);
    //
    // D3D12 only -- D3D9 and D3D11 have no GPU virtual addresses to resolve, and
    // return an empty BufferRef. Never fails, because "no buffer covers this
    // address" is an ordinary answer: the address may belong to memory the
    // application had before VanGFX attached.
    [[nodiscard]] virtual BufferRef
        buffer_at(uint64_t gpu_virtual_address) const noexcept = 0;

    // buffer_at + readback in one call, for the common case of wanting the bytes
    // at an address rather than the identity of the buffer holding them.
    [[nodiscard]] virtual Result<BufferData>
        readback_at(uint64_t gpu_virtual_address, uint64_t size) = 0;

    // ── Capture ───────────────────────────────────────────────────────────────

    [[nodiscard]] virtual bool capture_active() const noexcept = 0;

    // The G-buffer view of the most recent captured frame; the whole frame is
    // vangfx::latest_frame_capture(ctx).
    [[nodiscard]] virtual Result<GBufferCapture> latest_capture() = 0;

    // ── Native objects ────────────────────────────────────────────────────────
    //
    // The Direct3D objects behind this callback, for work VanGFX does not wrap.
    // Borrowed: valid for the duration of the callback; AddRef one to keep it.
    //
    //   native_device()   IDirect3DDevice9* / ID3D11Device* / ID3D12Device*
    //   native_context()  D3D9: the device. D3D11: the ID3D11DeviceContext the
    //                     call arrived on (immediate or deferred). D3D12: the
    //                     ID3D12GraphicsCommandList recording the call, or null
    //                     for queue, present and creation events.
    //   native_resource() the resource a handle names -- IDirect3DSurface9* for
    //                     D3D9 targets, IDirect3DBaseTexture9* for D3D9 textures,
    //                     ID3D11Resource*, ID3D12Resource* -- or null when the
    //                     handle is unknown or its resource has been destroyed.
    //   native_buffer()   the same for the id a buffer handle or BufferRef carries.
    [[nodiscard]] virtual void* native_device()  const noexcept = 0;
    [[nodiscard]] virtual void* native_context() const noexcept = 0;
    [[nodiscard]] virtual void* native_resource(RenderTargetHandle h) const noexcept = 0;
    [[nodiscard]] virtual void* native_resource(DepthBufferHandle h)  const noexcept = 0;
    [[nodiscard]] virtual void* native_resource(TextureHandle h)      const noexcept = 0;
    [[nodiscard]] virtual void* native_buffer(uint64_t buffer_id)     const noexcept = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Callback signatures
// ─────────────────────────────────────────────────────────────────────────────

// Names one registered callback, for Context::remove_callback(). Zero is none.
struct CallbackId {
    uint64_t id = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

using DrawCallback         = std::function<void(FrameContext&, DrawEvent&)>;
using RenderTargetCallback = std::function<void(FrameContext&, RenderTargetEvent&)>;
using DepthCallback        = std::function<void(FrameContext&, DepthEvent&)>;
using PresentCallback      = std::function<void(FrameContext&, PresentEvent&)>;
using StateCallback        = std::function<void(FrameContext&, StateChangeEvent&)>;
using ResourceCallback     = std::function<void(FrameContext&, ResourceEvent&)>;
using QueryCallback        = std::function<void(FrameContext&, QueryEvent&)>;
using DeviceLostCallback   = std::function<void()>;
using DeviceResetCallback  = std::function<void(FrameContext&)>;

// Defined in vangfx/events/surface.hpp. Only named here, never dereferenced,
// so a declaration is enough and this header keeps its include set small.
struct ResourceBarrierBatch;

// D3D12-only callbacks
using CommandListCallback  = std::function<void(FrameContext&, d3d12::CommandListEvent&)>;
using PSOCreateCallback    = std::function<void(FrameContext&, d3d12::PSOCreateEvent&)>;
// Takes the batch, not a bare array: the batch is what carries the originating
// command list and the intercept flag, and events/surface.hpp already documents
// it as what this callback receives. The loose (pointer, count) form could not
// express either, so a callback had no way to suppress a barrier.
using ResourceBarrierCallback = std::function<void(FrameContext&, ResourceBarrierBatch&)>;

// ─────────────────────────────────────────────────────────────────────────────
//  Context — root SDK object; one instance per intercepted D3D device
// ─────────────────────────────────────────────────────────────────────────────

class Context {
public:
    virtual ~Context() = default;

    // ── Registration ──────────────────────────────────────────────────────────
    //
    // Each returns an id that remove_callback() takes. A plugin that can be
    // unloaded must remove its callbacks first: a callback left registered is
    // code the context will call after the plugin is gone.

    virtual CallbackId on_draw         (DrawCallback         cb) = 0;
    virtual CallbackId on_render_target(RenderTargetCallback cb) = 0;
    virtual CallbackId on_depth        (DepthCallback        cb) = 0;
    virtual CallbackId on_present      (PresentCallback      cb) = 0;
    virtual CallbackId on_state_change (StateCallback        cb) = 0;
    virtual CallbackId on_resource     (ResourceCallback     cb) = 0;
    virtual CallbackId on_query        (QueryCallback        cb) = 0;
    virtual CallbackId on_device_lost  (DeviceLostCallback   cb) = 0;
    virtual CallbackId on_device_reset (DeviceResetCallback  cb) = 0;

    // ── D3D12-only registration (never called on D3D9/D3D11) ─────────────────
    virtual CallbackId on_command_list    (CommandListCallback      cb) = 0;
    virtual CallbackId on_pso_create      (PSOCreateCallback        cb) = 0;
    virtual CallbackId on_resource_barrier(ResourceBarrierCallback  cb) = 0;

    // Unregisters a callback. Once this returns the callback is not running on
    // any other thread and will not be called again -- except when called from
    // inside a callback, where waiting for that callback would never finish.
    virtual void remove_callback(CallbackId id) noexcept = 0;

    // The device this context is attached to: IDirect3DDevice9* /
    // ID3D11Device* / ID3D12Device*. Borrowed.
    [[nodiscard]] virtual void* native_device() const noexcept = 0;

    // ── Shader compilation ────────────────────────────────────────────────────

    // Called internally by ShaderBuilder::build().  Prefer ShaderBuilder.
    [[nodiscard]] virtual Result<Shader>
        compile_shader(const ShaderBuilder& builder)       = 0;

    // Link a set of compiled Shader stages into an Effect.
    [[nodiscard]] virtual Result<Effect>
        link_effect(Shader vs, Shader ps,
                    Shader gs = {}, Shader hs = {}, Shader ds = {}) = 0;
    [[nodiscard]] virtual Result<Effect>
        link_compute_effect(Shader cs)                     = 0;

    virtual void destroy(ShaderHandle h)  noexcept = 0;
    virtual void destroy(EffectHandle h)  noexcept = 0;

    // ── Capture control (vangfx/capture/capture.hpp) ──────────────────────────
    //
    // A capture takes the frame after it starts; CaptureSession wraps these.
    // Without a config, it is a CaptureConfig{}: every target at the end of
    // one frame, read back, no files.

    virtual Result<void> start_capture()  = 0;
    virtual Result<void> start_capture(const CaptureConfig& cfg) = 0;
    virtual void         stop_capture()   noexcept = 0;
    [[nodiscard]] virtual bool capture_active() const noexcept = 0;

    // ── Logging ───────────────────────────────────────────────────────────────

    virtual void set_log_level(LogLevel level) noexcept = 0;
    virtual void set_log_callback(
        std::function<void(LogLevel, std::string_view)> cb) = 0;

    // ── Backend information ───────────────────────────────────────────────────

    [[nodiscard]] virtual BackendKind backend()  const noexcept = 0;
    [[nodiscard]] virtual uint32_t    d3d9_ver() const noexcept = 0; // minor ver or 0
    [[nodiscard]] virtual uint32_t    d3d11_feature_level() const noexcept = 0;

    // D3D12 — shader model supported by the device (e.g. 60, 61, 66).
    // Returns 0 on D3D9/D3D11 backends.
    [[nodiscard]] virtual uint32_t    d3d12_shader_model()   const noexcept = 0;
    // D3D12 — hardware raytracing tier: 0 = none, 1 = tier1, 11 = tier1_1.
    [[nodiscard]] virtual uint32_t    d3d12_raytracing_tier() const noexcept = 0;
    // D3D12 — mesh shader tier: 0 = none, 1 = tier1.
    [[nodiscard]] virtual uint32_t    d3d12_mesh_shader_tier() const noexcept = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CallbackGuard — removes a callback when it goes out of scope
//
//      vangfx::CallbackGuard g(*ctx, ctx->on_draw([](auto&, auto&) { ... }));
//
//  Keep one per callback in whatever owns the callback's captures, so the
//  callback cannot outlive what it points at.
// ─────────────────────────────────────────────────────────────────────────────

class CallbackGuard {
public:
    CallbackGuard() = default;
    CallbackGuard(Context& ctx, CallbackId id) noexcept : ctx_(&ctx), id_(id) {}
    ~CallbackGuard() { reset(); }

    CallbackGuard(CallbackGuard&& o) noexcept : ctx_(o.ctx_), id_(o.id_) { o.ctx_ = nullptr; o.id_ = {}; }
    CallbackGuard& operator=(CallbackGuard&& o) noexcept {
        if (this != &o) { reset(); ctx_ = o.ctx_; id_ = o.id_; o.ctx_ = nullptr; o.id_ = {}; }
        return *this;
    }
    CallbackGuard(const CallbackGuard&)            = delete;
    CallbackGuard& operator=(const CallbackGuard&) = delete;

    void reset() noexcept {
        if (ctx_ && id_) ctx_->remove_callback(id_);
        ctx_ = nullptr;
        id_  = {};
    }
    [[nodiscard]] CallbackId id() const noexcept { return id_; }

private:
    Context*   ctx_ = nullptr;
    CallbackId id_{};
};

// ─────────────────────────────────────────────────────────────────────────────
//  ContextBuilder — fluent factory for Context
//
//  Usage:
//      auto ctx = ContextBuilder{}
//          .backend(BackendKind::Auto)
//          .log_level(LogLevel::Info)
//          .build();
//      if (!ctx) { /* handle error */ }
//      auto& c = *ctx;
//      c.on_draw([](auto& frame, auto& ev) { ... });
// ─────────────────────────────────────────────────────────────────────────────

class ContextBuilder {
public:
    ContextBuilder& backend(BackendKind kind) {
        backend_ = kind;
        return *this;
    }
    ContextBuilder& log_level(LogLevel level) {
        log_level_ = level;
        return *this;
    }
    ContextBuilder& log_callback(
            std::function<void(LogLevel, std::string_view)> cb) {
        log_cb_ = std::move(cb);
        return *this;
    }
    // Attempt to hook a specific IDirect3DDevice9* (D3D9 only).
    ContextBuilder& d3d9_device(void* device) {
        d3d9_device_ = device;
        return *this;
    }
    // Getter used internally by make_d3d9_context().
    [[nodiscard]] void* d3d9_device() const noexcept { return d3d9_device_; }
    // Attempt to hook a specific ID3D10Device* (D3D10 only).
    ContextBuilder& d3d10_device(void* device) {
        d3d10_device_ = device;
        return *this;
    }
    [[nodiscard]] void* d3d10_device() const noexcept { return d3d10_device_; }
    // The IDXGISwapChain* to hook Present on (D3D10 only).
    ContextBuilder& d3d10_swap_chain(void* swap_chain) {
        d3d10_swap_chain_ = swap_chain;
        return *this;
    }
    [[nodiscard]] void* d3d10_swap_chain() const noexcept { return d3d10_swap_chain_; }
    // Attempt to hook a specific ID3D11Device* (D3D11 only).
    ContextBuilder& d3d11_device(void* device) {
        d3d11_device_ = device;
        return *this;
    }
    // Getter used internally by make_d3d11_context().
    [[nodiscard]] void* d3d11_device() const noexcept { return d3d11_device_; }
    // The IDXGISwapChain* to hook Present on (D3D11 only). Required: a device
    // cannot be walked back down to the swap chains created alongside it.
    ContextBuilder& d3d11_swap_chain(void* swap_chain) {
        d3d11_swap_chain_ = swap_chain;
        return *this;
    }
    // Getter used internally by make_d3d11_context().
    [[nodiscard]] void* d3d11_swap_chain() const noexcept {
        return d3d11_swap_chain_;
    }
    // Attempt to hook a specific ID3D12Device* (D3D12 only).
    ContextBuilder& d3d12_device(void* device) {
        d3d12_device_ = device;
        return *this;
    }
    // Getter used internally by make_d3d12_context().
    [[nodiscard]] void* d3d12_device() const noexcept { return d3d12_device_; }
    // Attempt to hook a specific ID3D12CommandQueue* (D3D12 direct queue).
    // Required: D3D12 has no way to enumerate the queues a device has already
    // created, so a context cannot find one on its own.
    ContextBuilder& d3d12_command_queue(void* queue) {
        d3d12_command_queue_ = queue;
        return *this;
    }
    // Getter used internally by make_d3d12_context().
    [[nodiscard]] void* d3d12_command_queue() const noexcept {
        return d3d12_command_queue_;
    }
    // The IDXGISwapChain* to hook Present on (D3D12 only). Optional: without
    // it the context still reports draws, resources and PSO creation, but
    // on_present never fires, because there is nothing to hook it on.
    ContextBuilder& d3d12_swap_chain(void* swap_chain) {
        d3d12_swap_chain_ = swap_chain;
        return *this;
    }
    // Getter used internally by make_d3d12_context().
    [[nodiscard]] void* d3d12_swap_chain() const noexcept {
        return d3d12_swap_chain_;
    }
    // OpenGL: the HGLRC to hook (OpenGL only). Stored as void*.
    ContextBuilder& gl_hglrc(void* hglrc) {
        gl_hglrc_ = hglrc;
        return *this;
    }
    [[nodiscard]] void* gl_hglrc() const noexcept { return gl_hglrc_; }
    // Attempt to hook a specific IDirect3DDevice8* (D3D8 only).
    ContextBuilder& d3d8_device(void* device) {
        d3d8_device_ = device;
        return *this;
    }
    [[nodiscard]] void* d3d8_device() const noexcept { return d3d8_device_; }
    // Vulkan: the VkInstance to monitor (Vulkan layer only).
    ContextBuilder& vk_instance(void* instance) {
        vk_instance_ = instance;
        return *this;
    }
    [[nodiscard]] void* vk_instance() const noexcept { return vk_instance_; }
    // GLES/EGL: EGLDisplay (GLES only).
    ContextBuilder& egl_display(void* display) {
        egl_display_ = display;
        return *this;
    }
    [[nodiscard]] void* egl_display() const noexcept { return egl_display_; }
    // GLES/EGL: EGLContext (GLES only).
    ContextBuilder& egl_context(void* ctx) {
        egl_context_ = ctx;
        return *this;
    }
    [[nodiscard]] void* egl_context() const noexcept { return egl_context_; }
    // Metal: MTLDevice* bridged as void* (Metal/macOS only).
    ContextBuilder& metal_device(void* device) {
        metal_device_ = device;
        return *this;
    }
    [[nodiscard]] void* metal_device() const noexcept { return metal_device_; }
    // Android: JNIEnv* for the attaching thread (Android loader only).
    ContextBuilder& android_env(void* jni_env) {
        android_env_ = jni_env;
        return *this;
    }
    [[nodiscard]] void* android_env() const noexcept { return android_env_; }
    // Minimum shader model to require on D3D12 (e.g. 60 = SM 6.0).
    // build() returns ShaderError::Unsupported if the device cannot meet it.
    ContextBuilder& d3d12_min_shader_model(uint32_t sm) {
        d3d12_min_shader_model_ = sm;
        return *this;
    }
    // Start a capture (a CaptureConfig{}) at the first Present, so the first
    // full frame is captured.
    ContextBuilder& auto_capture(bool enable = true) {
        auto_capture_ = enable;
        return *this;
    }

    // Forward-declared; implemented in the vangfx runtime library.
    [[nodiscard]] Result<std::unique_ptr<Context>> build() const;

    // Read-only accessors used by internal context construction (context.cpp).
    [[nodiscard]] uint32_t d3d12_min_shader_model_value() const noexcept {
        return d3d12_min_shader_model_;
    }
    [[nodiscard]] LogLevel log_level_value() const noexcept { return log_level_; }
    [[nodiscard]] const std::function<void(LogLevel, std::string_view)>& log_callback() const noexcept { return log_cb_; }
    [[nodiscard]] bool auto_capture_enabled() const noexcept { return auto_capture_; }

private:
    BackendKind  backend_      = BackendKind::Auto;
    LogLevel     log_level_    = LogLevel::Warn;
    std::function<void(LogLevel, std::string_view)> log_cb_;
    void*        d3d9_device_          = nullptr;
    void*        d3d10_device_         = nullptr;
    void*        d3d10_swap_chain_     = nullptr;
    void*        d3d11_device_         = nullptr;
    void*        d3d11_swap_chain_     = nullptr;
    void*        d3d12_device_         = nullptr;
    void*        d3d12_command_queue_  = nullptr;
    void*        d3d12_swap_chain_     = nullptr;
    void*        gl_hglrc_             = nullptr;
    void*        d3d8_device_          = nullptr;
    void*        vk_instance_          = nullptr;
    void*        egl_display_          = nullptr;
    void*        egl_context_          = nullptr;
    void*        metal_device_         = nullptr;
    void*        android_env_          = nullptr;
    uint32_t     d3d12_min_shader_model_ = 0;
    bool         auto_capture_         = false;
};

} // namespace vangfx
