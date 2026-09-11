#pragma once

/**
 * vangfx/core/context.hpp — Context, ContextBuilder, FrameContext, BackendKind.
 *
 * Context is the root object for the VanGFX SDK.  Create it once via
 * ContextBuilder::build(), then install it as the active interceptor.  The
 * Context hooks into D3D9 or D3D11 via vtable patching; no DLL proxy needed.
 *
 * FrameContext is handed to every event callback and exposes per-frame
 * queries, resource access, readback, and shader/effect binding.
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
    D3D11,
    D3D12,
    Auto,   // detect at build() time (default)
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

    // Grab the most recently resolved GBuffer capture (if capture is active).
    [[nodiscard]] virtual Result<GBufferCapture> latest_capture() = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Callback signatures
// ─────────────────────────────────────────────────────────────────────────────

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

    virtual void on_draw         (DrawCallback         cb) = 0;
    virtual void on_render_target(RenderTargetCallback cb) = 0;
    virtual void on_depth        (DepthCallback        cb) = 0;
    virtual void on_present      (PresentCallback      cb) = 0;
    virtual void on_state_change (StateCallback        cb) = 0;
    virtual void on_resource     (ResourceCallback     cb) = 0;
    virtual void on_query        (QueryCallback        cb) = 0;
    virtual void on_device_lost  (DeviceLostCallback   cb) = 0;
    virtual void on_device_reset (DeviceResetCallback  cb) = 0;

    // ── D3D12-only registration (no-op on D3D9/D3D11) ─────────────────────────
    virtual void on_command_list    (CommandListCallback      cb) = 0;
    virtual void on_pso_create      (PSOCreateCallback        cb) = 0;
    virtual void on_resource_barrier(ResourceBarrierCallback  cb) = 0;

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

    // ── Uniforms ──────────────────────────────────────────────────────────────

    // The context's own constant buffer: the one carrying vangfx_float4[8],
    // vangfx_time, vangfx_delta and vangfx_resolution into every injected
    // shader. Created on first call and owned by the context.
    //
    // This exists because Uniforms cannot create it. FrameContext's
    // update_constant_buffer takes a handle and returns Result<void>, so there
    // was no way to be handed one back -- Uniforms guarded its only update call
    // behind is_valid(cbuffer_) on a handle nothing ever filled in, and so
    // never uploaded anything at all. Ask the context for the buffer instead.
    //
    // Returns an invalid handle if the backend could not create it.
    [[nodiscard]] virtual ConstantBufferHandle uniform_buffer() noexcept = 0;

    // ── Capture control ───────────────────────────────────────────────────────

    virtual Result<void> start_capture()  = 0;
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
    // Minimum shader model to require on D3D12 (e.g. 60 = SM 6.0).
    // build() returns ShaderError::Unsupported if the device cannot meet it.
    ContextBuilder& d3d12_min_shader_model(uint32_t sm) {
        d3d12_min_shader_model_ = sm;
        return *this;
    }
    // Enable automatic capture on the first Present call.
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
    void*        d3d11_device_         = nullptr;
    void*        d3d11_swap_chain_     = nullptr;
    void*        d3d12_device_         = nullptr;
    void*        d3d12_command_queue_  = nullptr;
    void*        d3d12_swap_chain_     = nullptr;
    uint32_t     d3d12_min_shader_model_ = 0;
    bool         auto_capture_         = false;
};

} // namespace vangfx
