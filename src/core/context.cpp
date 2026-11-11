/**
 * src/core/context.cpp — ContextBuilder::build() and the shared ContextImpl.
 *
 * Build path:
 *   1. Honour or auto-detect BackendKind.
 *   2. Hand the builder to that backend's factory, which checks the device
 *      pointers it was given, installs its hooks and returns the context.
 *
 * Per-backend implementations live in src/backend/{d3d9,d3d11,d3d12}/context.cpp.
 */

#include "context_impl.hpp"
#include "../capture/recorder.hpp"
#include <vangfx/config.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vangfx::backend {

Result<std::unique_ptr<Context>> make_d3d9_context  (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_d3d10_context (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_d3d11_context (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_d3d12_context (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_gl_context     (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_d3d8_context   (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_vulkan_context (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_gles_context   (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_metal_context  (const ContextBuilder& b);
Result<std::unique_ptr<Context>> make_android_context(const ContextBuilder& b);

// A backend this library was built without (see CMakeLists.txt) still links.
#define VANGFX_ABSENT_BACKEND(fn, name)                                                   \
    Result<std::unique_ptr<Context>> fn(const ContextBuilder&) {                          \
        return gfx_err<std::unique_ptr<Context>>(ShaderError::Unsupported,                \
            "This VanGFX library was built without the " name " backend.");               \
    }
#if !VANGFX_D3D9
VANGFX_ABSENT_BACKEND(make_d3d9_context, "Direct3D 9")
#endif
#if !VANGFX_D3D10
VANGFX_ABSENT_BACKEND(make_d3d10_context, "Direct3D 10")
#endif
#if !VANGFX_D3D11
VANGFX_ABSENT_BACKEND(make_d3d11_context, "Direct3D 11")
#endif
#if !VANGFX_D3D12
VANGFX_ABSENT_BACKEND(make_d3d12_context, "Direct3D 12")
#endif
#if !VANGFX_GL
VANGFX_ABSENT_BACKEND(make_gl_context, "OpenGL")
#endif
#if !VANGFX_D3D8
VANGFX_ABSENT_BACKEND(make_d3d8_context, "Direct3D 8")
#endif
#if !VANGFX_VULKAN
VANGFX_ABSENT_BACKEND(make_vulkan_context, "Vulkan")
#endif
#if !VANGFX_GLES
VANGFX_ABSENT_BACKEND(make_gles_context, "GLES/EGL")
#endif
#if !VANGFX_METAL
VANGFX_ABSENT_BACKEND(make_metal_context, "Metal")
#endif
#if !VANGFX_ANDROID
VANGFX_ABSENT_BACKEND(make_android_context, "Android")
#endif
#undef VANGFX_ABSENT_BACKEND

} // namespace vangfx::backend

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  BackendKind auto-detection
//
//  With a device pointer supplied, the pointer says which API it belongs to.
//  Without one, the loaded DLLs are the only evidence: D3D12 wins over D3D11
//  wins over D3D9 — a game that loads both d3d11.dll and d3d12.dll is almost
//  certainly a D3D12 title that kept d3d11.dll in process for other reasons.
// ─────────────────────────────────────────────────────────────────────────────

static BackendKind detect_backend(const ContextBuilder& b) noexcept {
    if (b.d3d12_device())  return BackendKind::D3D12;
    if (b.d3d11_device())  return BackendKind::D3D11;
    if (b.d3d10_device())  return BackendKind::D3D10;
    if (b.d3d9_device())   return BackendKind::D3D9;
    if (b.gl_hglrc())      return BackendKind::OpenGL;
    if (b.d3d8_device())   return BackendKind::D3D8;
    if (b.vk_instance())   return BackendKind::Vulkan;
    if (b.egl_display())   return BackendKind::GLES;
    if (b.metal_device())  return BackendKind::Metal;
    if (b.android_env())   return BackendKind::Android;
    // Module-based auto-detection: D3D wins over GL/D3D8 when both are present.
    if (VANGFX_D3D12 && GetModuleHandleW(L"d3d12.dll"))     return BackendKind::D3D12;
    if (VANGFX_D3D11 && GetModuleHandleW(L"d3d11.dll"))     return BackendKind::D3D11;
    if (VANGFX_D3D10 && GetModuleHandleW(L"d3d10.dll"))     return BackendKind::D3D10;
    if (VANGFX_D3D9  && GetModuleHandleW(L"d3d9.dll"))      return BackendKind::D3D9;
    if (VANGFX_D3D8  && GetModuleHandleW(L"d3d8.dll"))      return BackendKind::D3D8;
    if (VANGFX_GL    && GetModuleHandleW(L"opengl32.dll"))   return BackendKind::OpenGL;
    return BackendKind::Auto;
}

Result<std::unique_ptr<Context>> ContextBuilder::build() const {
    const BackendKind resolved = (backend_ == BackendKind::Auto) ? detect_backend(*this) : backend_;
    switch (resolved) {
        case BackendKind::D3D9:   return backend::make_d3d9_context(*this);
        case BackendKind::D3D10:  return backend::make_d3d10_context(*this);
        case BackendKind::D3D11:  return backend::make_d3d11_context(*this);
        case BackendKind::D3D12:  return backend::make_d3d12_context(*this);
        case BackendKind::OpenGL: return backend::make_gl_context(*this);
        case BackendKind::D3D8:   return backend::make_d3d8_context(*this);
        case BackendKind::Vulkan:  return backend::make_vulkan_context(*this);
        case BackendKind::GLES:    return backend::make_gles_context(*this);
        case BackendKind::Metal:   return backend::make_metal_context(*this);
        case BackendKind::Android: return backend::make_android_context(*this);
        case BackendKind::Auto:    break;
    }
    return gfx_err<std::unique_ptr<Context>>(
        ShaderError::NotInitialized,
        "BackendKind::Auto was not resolved: no device was supplied and no D3D DLL "
        "is loaded in this process.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  ContextImpl
// ─────────────────────────────────────────────────────────────────────────────

ContextImpl::ContextImpl(const ContextBuilder& b)
    : log_level_(b.log_level_value())
    , auto_capture_(b.auto_capture_enabled())
{
    if (b.log_callback())
        log_cb_ = std::make_shared<const LogFn>(b.log_callback());
}

ContextImpl::~ContextImpl() { run_teardown(); }

void ContextImpl::add_teardown(std::function<void()> fn) {
    std::lock_guard lk(teardown_mutex_);
    if (!torn_down_) teardown_.push_back(std::move(fn));
}

void ContextImpl::run_teardown() noexcept {
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard lk(teardown_mutex_);
        if (torn_down_) return;
        torn_down_ = true;
        fns.swap(teardown_);
    }
    for (auto it = fns.rbegin(); it != fns.rend(); ++it) {
        try { (*it)(); } catch (...) {}
    }
}

CallbackId ContextImpl::on_draw         (DrawCallback         cb) { return add(draw_cbs_,    std::move(cb)); }
CallbackId ContextImpl::on_render_target(RenderTargetCallback cb) { return add(rt_cbs_,      std::move(cb)); }
CallbackId ContextImpl::on_depth        (DepthCallback        cb) { return add(depth_cbs_,   std::move(cb)); }
CallbackId ContextImpl::on_present      (PresentCallback      cb) { return add(present_cbs_, std::move(cb)); }
CallbackId ContextImpl::on_state_change (StateCallback        cb) { return add(state_cbs_,   std::move(cb)); }
CallbackId ContextImpl::on_resource     (ResourceCallback     cb) { return add(res_cbs_,     std::move(cb)); }
CallbackId ContextImpl::on_query        (QueryCallback        cb) { return add(query_cbs_,   std::move(cb)); }
CallbackId ContextImpl::on_device_lost  (DeviceLostCallback   cb) { return add(lost_cbs_,    std::move(cb)); }
CallbackId ContextImpl::on_device_reset (DeviceResetCallback  cb) { return add(reset_cbs_,   std::move(cb)); }
CallbackId ContextImpl::on_command_list    (CommandListCallback      cb) { return add(cmd_cbs_,     std::move(cb)); }
CallbackId ContextImpl::on_pso_create      (PSOCreateCallback        cb) { return add(pso_cbs_,     std::move(cb)); }
CallbackId ContextImpl::on_resource_barrier(ResourceBarrierCallback  cb) { return add(barrier_cbs_, std::move(cb)); }

void ContextImpl::remove_callback(CallbackId id) noexcept {
    if (!id) return;
    // Ids are unique across lists, so at most one of these finds it.
    draw_cbs_.remove(id.id)    || rt_cbs_.remove(id.id)     || depth_cbs_.remove(id.id) ||
    present_cbs_.remove(id.id) || state_cbs_.remove(id.id)  || res_cbs_.remove(id.id)   ||
    query_cbs_.remove(id.id)   || lost_cbs_.remove(id.id)   || reset_cbs_.remove(id.id) ||
    cmd_cbs_.remove(id.id)     || pso_cbs_.remove(id.id)    || barrier_cbs_.remove(id.id);
}

void ContextImpl::set_log_level(LogLevel level) noexcept {
    log_level_.store(level, std::memory_order_relaxed);
}

void ContextImpl::set_log_callback(std::function<void(LogLevel, std::string_view)> cb) {
    auto next = cb ? std::make_shared<const LogFn>(std::move(cb)) : nullptr;
    std::lock_guard lk(log_mutex_);
    log_cb_ = std::move(next);
}

void ContextImpl::log(LogLevel level, std::string_view msg) noexcept {
    if (!logs(level)) return;
    std::shared_ptr<const LogFn> fn;
    {
        std::lock_guard lk(log_mutex_);
        fn = log_cb_;
    }
    if (fn && *fn) {
        try { (*fn)(level, msg); } catch (...) {}
    }
}

Result<void> ContextImpl::start_capture() { return start_capture(CaptureConfig{}); }

Result<void> ContextImpl::start_capture(const CaptureConfig& cfg) {
    if (!snapshot_host() && cfg.cpu_readback) {
        return gfx_err<void>(ShaderError::Unsupported,
            "This context's backend cannot copy surfaces for a capture.");
    }
    // Nothing drawn since the last Present: this frame is whole, and is taken.
    const bool whole = frame_draws_.load(std::memory_order_relaxed) == 0;
    recorder_.store(std::make_shared<capture::Recorder>(*this, cfg, whole), std::memory_order_release);
    capture_active_.store(true, std::memory_order_release);
    log(LogLevel::Info, whole ? "VanGFX capture started; it takes this frame."
                              : "VanGFX capture started part-way through a frame; it takes the next one.");
    return {};
}

void ContextImpl::stop_capture() noexcept {
    if (!capture_active_.exchange(false, std::memory_order_acq_rel)) return;
    recorder_.store(nullptr, std::memory_order_release);
    log(LogLevel::Info, "VanGFX capture stopped.");
}

bool ContextImpl::capture_active() const noexcept {
    return capture_active_.load(std::memory_order_acquire);
}

void ContextImpl::capture_present(FrameContext& fc, bool first_frame) {
    if (auto rec = recorder()) {
        if (rec->present(fc)) stop_capture();
    }
    frame_draws_.store(0, std::memory_order_relaxed);
    if (first_frame && auto_capture_) (void)start_capture();
}

Result<GBufferCapture> ContextImpl::latest_gbuffer() const {
    auto last = last_capture();
    if (!last) {
        return gfx_err<GBufferCapture>(ShaderError::CaptureNotActive,
            "No frame has been captured yet. A capture takes the frame after it starts and is read back at "
            "that frame's Present -- start one with CaptureSession::begin().");
    }
    return last->gbuffer;
}

uint64_t ContextImpl::add_post_pass(std::vector<uint8_t> ps_bytecode) {
    const uint64_t id = alloc_id();
    std::lock_guard lk(post_mutex_);
    auto next = std::make_shared<PostPassList>(*post_passes_);
    next->push_back({ id, std::move(ps_bytecode) });
    post_passes_ = std::move(next);
    return id;
}

bool ContextImpl::remove_post_pass(uint64_t id) noexcept {
    std::lock_guard lk(post_mutex_);
    auto next = std::make_shared<PostPassList>();
    for (const auto& p : *post_passes_) if (p.id != id) next->push_back(p);
    const bool found = next->size() != post_passes_->size();
    post_passes_ = std::move(next);
    return found;
}

uint64_t ContextImpl::alloc_id() noexcept {
    static std::atomic<uint64_t> next{ 1 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

void ContextImpl::destroy(ShaderHandle) noexcept {}
void ContextImpl::destroy(EffectHandle) noexcept {}

Result<Shader> ContextImpl::compile_shader(const ShaderBuilder&) {
    return gfx_err<Shader>(ShaderError::NotInitialized,
        "compile_shader: backend context did not override this method.");
}
Result<Effect> ContextImpl::link_effect(Shader, Shader, Shader, Shader, Shader) {
    return gfx_err<Effect>(ShaderError::NotInitialized,
        "link_effect: backend context did not override this method.");
}
Result<Effect> ContextImpl::link_compute_effect(Shader) {
    return gfx_err<Effect>(ShaderError::NotInitialized,
        "link_compute_effect: backend context did not override this method.");
}

BackendKind ContextImpl::backend()               const noexcept { return BackendKind::Auto; }
uint32_t    ContextImpl::d3d9_ver()              const noexcept { return 0; }
uint32_t    ContextImpl::d3d11_feature_level()   const noexcept { return 0; }
uint32_t    ContextImpl::d3d12_shader_model()    const noexcept { return 0; }
uint32_t    ContextImpl::d3d12_raytracing_tier() const noexcept { return 0; }
uint32_t    ContextImpl::d3d12_mesh_shader_tier()const noexcept { return 0; }

} // namespace vangfx
