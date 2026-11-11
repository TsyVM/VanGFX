#pragma once

/**
 * src/backend/gl/gl_impl.hpp — the OpenGL WGL backend's shared declarations.
 *
 * The GL backend differs from the D3D backends in one key way: hooks are
 * installed on opengl32.dll exports (via VanHooks api_hook) rather than on
 * vtable slots.  The "device" key is an HGLRC (OpenGL rendering context).
 *
 * DevState is looked up by HGLRC at hook time via wglGetCurrentContext().
 * A context that has no HGLRC yet auto-adopts the first HGLRC seen.
 */

#include "../../core/context_impl.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>  // GL types + WGL declarations (wingdi.h subset)

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace vangfx::backend::gl {

class GLContextImpl;

// ── Hub ───────────────────────────────────────────────────────────────────────

void hub_attach(GLContextImpl* ctx) noexcept;
void hub_detach(GLContextImpl* ctx) noexcept;
[[nodiscard]] bool     hub_idle()       noexcept;
[[nodiscard]] uint32_t hub_generation() noexcept;

class HubReader {
public:
    HubReader() noexcept;
    ~HubReader();
    HubReader(const HubReader&) = delete;
    HubReader& operator=(const HubReader&) = delete;
};

struct ContextSet {
    static constexpr uint32_t kMax = 4;
    GLContextImpl* items[kMax]{};
    uint32_t       count = 0;
    GLContextImpl* const* begin() const noexcept { return items; }
    GLContextImpl* const* end()   const noexcept { return items + count; }
    [[nodiscard]] explicit operator bool() const noexcept { return count != 0; }
};

[[nodiscard]] ContextSet contexts_for(HGLRC hglrc) noexcept;

class InternalScope {
public:
    InternalScope() noexcept;
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
[[nodiscard]] bool internal() noexcept;

// ── Per-context state ─────────────────────────────────────────────────────────

struct DevState {
    GLuint   current_program = 0;
    uint64_t program_hash    = 0;
    uint32_t gen = UINT32_MAX;
    ContextSet contexts;
    void clear() noexcept { *this = DevState{}; }
};

[[nodiscard]] DevState* dev_state(HGLRC hglrc) noexcept;

// Program hash: stable per (hglrc, program_id), never 0 for a live program.
[[nodiscard]] uint64_t make_program_hash(HGLRC hglrc, GLuint program) noexcept;

// ── Frame context ─────────────────────────────────────────────────────────────

class GLFrameContextImpl final : public vangfx::FrameContext {
public:
    GLFrameContextImpl(GLContextImpl& ctx, uint32_t frame_index,
                       HGLRC hglrc    = nullptr,
                       DevState* state = nullptr) noexcept
        : ctx_(ctx), frame_index_(frame_index), hglrc_(hglrc), state_(state) {}

    BackendKind backend()     const noexcept override { return BackendKind::OpenGL; }
    uint32_t    frame_index() const noexcept override { return frame_index_; }

    FrameSurfaces surfaces()  const noexcept override { return {}; }
    PipelineState pipeline()  const noexcept override { return {}; }

    SurfaceFormat format_of(RenderTargetHandle) const noexcept override { return {}; }
    SurfaceFormat format_of(DepthBufferHandle)  const noexcept override { return {}; }
    SurfaceFormat format_of(TextureHandle)      const noexcept override { return {}; }

    Result<TextureHandle>      create_texture(const TextureDesc&)           override;
    Result<RenderTargetHandle> create_render_target(const RenderTargetDesc&) override;
    Result<DepthBufferHandle>  create_depth_buffer(const DepthBufferDesc&)   override;
    void destroy(TextureHandle)      noexcept override {}
    void destroy(RenderTargetHandle) noexcept override {}
    void destroy(DepthBufferHandle)  noexcept override {}

    Result<void> bind_effect(const Effect&) override;
    void         unbind_effect() noexcept override {}

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
        return gfx_err<GBufferCapture>(ShaderError::Unsupported, "capture not implemented on OpenGL");
    }

    void* native_device()  const noexcept override;
    void* native_context() const noexcept override;
    void* native_resource(RenderTargetHandle) const noexcept override { return nullptr; }
    void* native_resource(DepthBufferHandle)  const noexcept override { return nullptr; }
    void* native_resource(TextureHandle)      const noexcept override { return nullptr; }
    void* native_buffer(uint64_t)             const noexcept override { return nullptr; }

private:
    static Result<ReadbackData> not_impl() {
        return gfx_err<ReadbackData>(ShaderError::Unsupported, "readback not implemented on OpenGL");
    }
    static Result<BufferData> not_buf() {
        return gfx_err<BufferData>(ShaderError::Unsupported, "buffer readback not implemented on OpenGL");
    }

    GLContextImpl& ctx_;
    uint32_t       frame_index_;
    HGLRC          hglrc_;
    DevState*      state_;
};

// ── Context ───────────────────────────────────────────────────────────────────

class GLContextImpl final : public vangfx::ContextImpl {
public:
    GLContextImpl(const ContextBuilder& b, HGLRC hglrc);
    ~GLContextImpl() override;

    BackendKind backend()              const noexcept override { return BackendKind::OpenGL; }
    uint32_t    d3d9_ver()             const noexcept override { return 0; }
    uint32_t    d3d11_feature_level()  const noexcept override { return 0; }
    uint32_t    d3d12_shader_model()   const noexcept override { return 0; }
    uint32_t    d3d12_raytracing_tier()const noexcept override { return 0; }
    uint32_t    d3d12_mesh_shader_tier()const noexcept override{ return 0; }
    void*       native_device()        const noexcept override { return hglrc_; }

    Result<Shader> compile_shader(const ShaderBuilder& b) override;
    Result<Effect> link_effect(Shader vs, Shader ps, Shader gs, Shader hs, Shader ds) override;
    Result<Effect> link_compute_effect(Shader cs) override;
    void destroy(ShaderHandle h) noexcept override;
    void destroy(EffectHandle h) noexcept override;

    [[nodiscard]] HGLRC hglrc() const noexcept { return hglrc_; }
    void adopt_hglrc(HGLRC h) noexcept;

    [[nodiscard]] uint32_t current_frame_index() const noexcept {
        return frame_index_.load(std::memory_order_relaxed);
    }
    uint32_t next_frame() noexcept {
        return frame_index_.fetch_add(1, std::memory_order_relaxed);
    }

    struct Timing { float time = 0.0f, delta = 0.0f; bool first = true; };
    Timing tick() noexcept;

    std::string post_pass_profile() const override { return "glsl_460"; }

private:
    HGLRC                   hglrc_;
    std::atomic<uint32_t>   frame_index_{ 0 };

    using Clock = std::chrono::steady_clock;
    std::mutex        timing_mutex_;
    Clock::time_point start_ = Clock::now();
    Clock::time_point last_  = Clock::now();
    bool              first_frame_ = true;
};

// ── Hook installation ─────────────────────────────────────────────────────────

bool install_draw_hooks()    noexcept;  // draw.cpp
bool install_present_hooks() noexcept;  // present.cpp
void uninstall_all_hooks()   noexcept;

} // namespace vangfx::backend::gl
