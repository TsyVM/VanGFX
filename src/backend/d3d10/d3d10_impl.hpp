#pragma once

/**
 * src/backend/d3d10/d3d10_impl.hpp — the D3D10 backend's shared declarations.
 *
 * D3D10 differs from D3D11 in one key way: ID3D10Device combines device and
 * immediate context into a single object.  There are no deferred contexts, no
 * command lists, and no heal/settle complexity (D3D10 never rewrites its own
 * vtable).  State is therefore per-device rather than per-context.
 */

#include "../../core/context_impl.hpp"
#include <compat/hook_site.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d10_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace vangfx::backend::d3d10 {

class D3D10ContextImpl;

// ── Hub ──────────────────────────────────────────────────────────────────────

void hub_attach(D3D10ContextImpl* ctx) noexcept;
void hub_detach(D3D10ContextImpl* ctx) noexcept;
[[nodiscard]] bool     hub_idle()       noexcept;
[[nodiscard]] uint32_t hub_generation() noexcept;
[[nodiscard]] bool     hub_attached(const D3D10ContextImpl* ctx) noexcept;

class HubReader {
public:
    HubReader() noexcept;
    ~HubReader();
    HubReader(const HubReader&) = delete;
    HubReader& operator=(const HubReader&) = delete;
};

struct ContextSet {
    static constexpr uint32_t kMax = 4;
    D3D10ContextImpl* items[kMax]{};
    uint32_t          count = 0;
    D3D10ContextImpl* const* begin() const noexcept { return items; }
    D3D10ContextImpl* const* end()   const noexcept { return items + count; }
    [[nodiscard]] explicit operator bool() const noexcept { return count != 0; }
};

[[nodiscard]] ContextSet contexts_for(const ID3D10Device* device) noexcept;

class InternalScope {
public:
    InternalScope() noexcept;
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
[[nodiscard]] bool internal() noexcept;

// ── D3D10 device bindings ─────────────────────────────────────────────────────
//
// The shaders currently set on a device (D3D10 has no separate context).

struct DevState {
    ID3D10VertexShader*  vs = nullptr;
    ID3D10PixelShader*   ps = nullptr;
    uint64_t             vs_hash = 0, ps_hash = 0;
    uint32_t             gen = UINT32_MAX;
    ContextSet           contexts;

    bool vs_swapped = false;
    bool ps_swapped = false;

    void clear() noexcept { *this = DevState{}; }
};

[[nodiscard]] DevState* dev_state(ID3D10Device* dev) noexcept;

// Shader hash registry.
void     shader_register(ID3D10DeviceChild* shader, uint64_t hash) noexcept;
[[nodiscard]] uint64_t shader_hash(const void* shader) noexcept;

// ── Frame context ─────────────────────────────────────────────────────────────

class D3D10FrameContextImpl final : public vangfx::FrameContext {
public:
    D3D10FrameContextImpl(D3D10ContextImpl& ctx, uint32_t frame_index,
                          ID3D10Device* dev = nullptr,
                          DevState* state   = nullptr) noexcept
        : ctx_(ctx), frame_index_(frame_index), dev_(dev), state_(state) {}

    BackendKind backend()     const noexcept override { return BackendKind::D3D10; }
    uint32_t    frame_index() const noexcept override { return frame_index_; }

    FrameSurfaces surfaces()  const noexcept override { return {}; }
    PipelineState pipeline()  const noexcept override { return {}; }

    SurfaceFormat format_of(RenderTargetHandle) const noexcept override { return {}; }
    SurfaceFormat format_of(DepthBufferHandle)  const noexcept override { return {}; }
    SurfaceFormat format_of(TextureHandle)      const noexcept override { return {}; }

    Result<TextureHandle>      create_texture(const TextureDesc&)      override;
    Result<RenderTargetHandle> create_render_target(const RenderTargetDesc&) override;
    Result<DepthBufferHandle>  create_depth_buffer(const DepthBufferDesc&)   override;
    void destroy(TextureHandle)      noexcept override {}
    void destroy(RenderTargetHandle) noexcept override {}
    void destroy(DepthBufferHandle)  noexcept override {}

    Result<void> bind_effect(const Effect& e) override;
    void         unbind_effect() noexcept override;

    Result<void> update_constant_buffer(ConstantBufferHandle, const void*, uint32_t) override;

    Result<ReadbackData> readback(TextureHandle)      override { return not_impl(); }
    Result<ReadbackData> readback(RenderTargetHandle) override { return not_impl(); }
    Result<ReadbackData> readback(DepthBufferHandle)  override { return not_impl(); }
    Result<BufferData>   readback(VertexBufferHandle,     uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(IndexBufferHandle,      uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(ConstantBufferHandle,   uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(StructuredBufferHandle, uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(RawBufferHandle,        uint64_t, uint64_t) override { return not_buf(); }
    Result<BufferData>   readback(UAVHandle,              uint64_t, uint64_t) override { return not_buf(); }
    BufferRef            buffer_at(uint64_t) const noexcept override { return {}; }
    Result<BufferData>   readback_at(uint64_t, uint64_t) override { return not_buf(); }

    bool                   capture_active() const noexcept override { return false; }
    Result<GBufferCapture> latest_capture()       override {
        return gfx_err<GBufferCapture>(ShaderError::Unsupported,
                                       "capture not implemented on D3D10");
    }

    void* native_device()  const noexcept override;
    void* native_context() const noexcept override;
    void* native_resource(RenderTargetHandle) const noexcept override { return nullptr; }
    void* native_resource(DepthBufferHandle)  const noexcept override { return nullptr; }
    void* native_resource(TextureHandle)      const noexcept override { return nullptr; }
    void* native_buffer(uint64_t)             const noexcept override { return nullptr; }

private:
    static Result<ReadbackData> not_impl() {
        return gfx_err<ReadbackData>(ShaderError::Unsupported,
                                     "readback not implemented on D3D10");
    }
    static Result<BufferData> not_buf() {
        return gfx_err<BufferData>(ShaderError::Unsupported,
                                   "buffer readback not implemented on D3D10");
    }

    D3D10ContextImpl& ctx_;
    uint32_t          frame_index_;
    ID3D10Device*     dev_;
    DevState*         state_;
};

// ── Context ───────────────────────────────────────────────────────────────────

class D3D10ContextImpl final : public vangfx::ContextImpl {
public:
    D3D10ContextImpl(const ContextBuilder& b, ID3D10Device* device,
                     IDXGISwapChain* swap_chain);
    ~D3D10ContextImpl() override;

    BackendKind backend()           const noexcept override { return BackendKind::D3D10; }
    uint32_t    d3d9_ver()          const noexcept override { return 0; }
    uint32_t    d3d11_feature_level() const noexcept override { return 0; }
    uint32_t    d3d12_shader_model()  const noexcept override { return 0; }
    uint32_t    d3d12_raytracing_tier() const noexcept override { return 0; }
    uint32_t    d3d12_mesh_shader_tier() const noexcept override { return 0; }
    void*       native_device()     const noexcept override { return device_; }

    Result<Shader> compile_shader(const ShaderBuilder& b) override;
    Result<Effect> link_effect(Shader vs, Shader ps, Shader gs,
                                Shader hs, Shader ds) override;
    Result<Effect> link_compute_effect(Shader cs) override;
    void destroy(ShaderHandle h) noexcept override;
    void destroy(EffectHandle h) noexcept override;

    [[nodiscard]] ID3D10Device*    device()     const noexcept { return device_; }
    [[nodiscard]] IDXGISwapChain*  swap_chain() const noexcept {
        return swap_chain_.load(std::memory_order_acquire);
    }
    void adopt_swap_chain(IDXGISwapChain* sc) noexcept;

    [[nodiscard]] uint32_t current_frame_index() const noexcept {
        return frame_index_.load(std::memory_order_relaxed);
    }
    uint32_t next_frame() noexcept {
        return frame_index_.fetch_add(1, std::memory_order_relaxed);
    }

    struct Timing { float time = 0.0f, delta = 0.0f; bool first = true; };
    Timing tick() noexcept;

    std::string post_pass_profile() const override { return "ps_4_0"; }

private:
    ID3D10Device*                device_      = nullptr;
    std::atomic<IDXGISwapChain*> swap_chain_{ nullptr };
    std::atomic<uint32_t>        frame_index_{ 0 };

    using Clock = std::chrono::steady_clock;
    std::mutex        timing_mutex_;
    Clock::time_point start_ = Clock::now();
    Clock::time_point last_  = Clock::now();
    bool              first_frame_ = true;
};

// ── Hook installation ─────────────────────────────────────────────────────────

bool install_device_draw_hooks(ID3D10Device* device) noexcept;  // draw.cpp
bool install_present_hooks(IDXGISwapChain* swap_chain) noexcept; // present.cpp
void uninstall_all_hooks() noexcept;

// VS/PS set without firing a hook (for per-draw shader swaps).
void set_vs_unreported(ID3D10Device* dev, ID3D10VertexShader* vs) noexcept;
void set_ps_unreported(ID3D10Device* dev, ID3D10PixelShader*  ps) noexcept;
void restore_shaders(ID3D10Device* dev, DevState& st) noexcept;

} // namespace vangfx::backend::d3d10
