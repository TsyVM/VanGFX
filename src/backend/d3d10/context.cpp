/**
 * src/backend/d3d10/context.cpp — D3D10ContextImpl + make_d3d10_context().
 */

#include "d3d10_impl.hpp"
#include <vangfx/core/shader.hpp>
#include <vangfx/core/error.hpp>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace vangfx::backend::d3d10 {

// ── install_all_device_hooks (resource.cpp) forward declaration ──────────────

bool install_all_device_hooks(ID3D10Device* device) noexcept;

// ── D3D10ContextImpl ─────────────────────────────────────────────────────────

D3D10ContextImpl::D3D10ContextImpl(const ContextBuilder& b, ID3D10Device* device,
                                    IDXGISwapChain* swap_chain)
    : ContextImpl(b), device_(device)
{
    if (swap_chain) swap_chain_.store(swap_chain, std::memory_order_release);
    hub_attach(this);
    install_all_device_hooks(device);
    if (swap_chain) install_present_hooks(swap_chain);
}

D3D10ContextImpl::~D3D10ContextImpl() {
    hub_detach(this);
    if (hub_idle()) uninstall_all_hooks();
}

void D3D10ContextImpl::adopt_swap_chain(IDXGISwapChain* sc) noexcept {
    IDXGISwapChain* expected = nullptr;
    if (swap_chain_.compare_exchange_strong(expected, sc,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        install_present_hooks(sc);
    }
}

D3D10ContextImpl::Timing D3D10ContextImpl::tick() noexcept {
    std::lock_guard lk(timing_mutex_);
    const auto now   = Clock::now();
    const float time  = std::chrono::duration<float>(now - start_).count();
    const float delta = std::chrono::duration<float>(now - last_).count();
    last_             = now;
    const bool first  = first_frame_;
    first_frame_      = false;
    return { time, delta, first };
}

// ── Shader compilation (delegated to D3DCompile for SM 4.0) ─────────────────

Result<Shader> D3D10ContextImpl::compile_shader(const ShaderBuilder& b) {
    const std::string& src     = b.inline_source_text();
    const std::string& ep      = b.entry_point();
    const std::string& profile = b.shader_profile();
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(
        src.data(), src.size(),
        nullptr, nullptr, nullptr,
        ep.empty() ? "main" : ep.c_str(),
        profile.empty() ? "ps_4_0" : profile.c_str(),
        0, 0, &code, &err);
    if (FAILED(hr) || !code) {
        const char* msg = (err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return gfx_err<Shader>(ShaderError::CompileFailed, msg);
    }
    Shader s;
    s.handle.id = alloc_id();
    s.bytecode.assign(
        static_cast<const uint8_t*>(code->GetBufferPointer()),
        static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
    return s;
}

Result<Effect> D3D10ContextImpl::link_effect(Shader vs, Shader ps,
                                               Shader /*gs*/, Shader /*hs*/, Shader /*ds*/) {
    Effect e;
    e.handle.id = alloc_id();
    e.vs = std::move(vs);
    e.ps = std::move(ps);
    return e;
}

Result<Effect> D3D10ContextImpl::link_compute_effect(Shader /*cs*/) {
    return gfx_err<Effect>(ShaderError::Unsupported, "D3D10 has no compute shaders");
}

void D3D10ContextImpl::destroy(ShaderHandle h) noexcept { ContextImpl::destroy(h); }
void D3D10ContextImpl::destroy(EffectHandle h) noexcept { ContextImpl::destroy(h); }

// ── D3D10FrameContextImpl stubs ───────────────────────────────────────────────

Result<TextureHandle> D3D10FrameContextImpl::create_texture(const TextureDesc&) {
    return gfx_err<TextureHandle>(ShaderError::Unsupported,
                                   "create_texture not implemented on D3D10 backend");
}
Result<RenderTargetHandle> D3D10FrameContextImpl::create_render_target(const RenderTargetDesc&) {
    return gfx_err<RenderTargetHandle>(ShaderError::Unsupported,
                                        "create_render_target not implemented on D3D10");
}
Result<DepthBufferHandle> D3D10FrameContextImpl::create_depth_buffer(const DepthBufferDesc&) {
    return gfx_err<DepthBufferHandle>(ShaderError::Unsupported,
                                       "create_depth_buffer not implemented on D3D10");
}

Result<void> D3D10FrameContextImpl::bind_effect(const Effect& /*e*/) {
    return gfx_err<void>(ShaderError::Unsupported, "bind_effect not implemented on D3D10");
}

void D3D10FrameContextImpl::unbind_effect() noexcept {}

Result<void> D3D10FrameContextImpl::update_constant_buffer(ConstantBufferHandle, const void*, uint32_t) {
    return gfx_err<void>(ShaderError::Unsupported,
                         "update_constant_buffer not implemented on D3D10");
}

void* D3D10FrameContextImpl::native_device()  const noexcept { return dev_; }
void* D3D10FrameContextImpl::native_context() const noexcept { return dev_; }

} // namespace vangfx::backend::d3d10

// ── Factory (called from src/core/context.cpp) ────────────────────────────────

namespace vangfx::backend {

using namespace vangfx::backend::d3d10;

Result<std::unique_ptr<Context>> make_d3d10_context(const ContextBuilder& b) {
    auto* dev = static_cast<ID3D10Device*>(b.d3d10_device());
    if (!dev)
        return gfx_err<std::unique_ptr<Context>>(
            ShaderError::NotInitialized,
            "ContextBuilder: d3d10_device() is null — call .d3d10_device(ptr)");

    auto* sc = static_cast<IDXGISwapChain*>(b.d3d10_swap_chain());
    return std::make_unique<D3D10ContextImpl>(b, dev, sc);
}

} // namespace vangfx::backend
