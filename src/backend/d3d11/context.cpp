/**
 * src/backend/d3d11/context.cpp — D3D11ContextImpl and make_d3d11_context.
 *
 * Building a context: check the device, install the hooks -- on the device
 * class, on the immediate context (every device context has a vtable of its
 * own; deferred ones are hooked as they are made), and Present through the
 * application's swap chain or one made for the purpose -- then attach.
 *
 * Tearing down: effect and capture teardown while the context still works,
 * detach (the hub waits for dispatches in flight), release the post-processing
 * resources; the hooks come out when the last D3D11 context goes.
 */

#include "d3d11_impl.hpp"

#include <string>

namespace vangfx::backend::d3d11 {

namespace {
std::mutex g_install_mutex;
}

void uninstall_context_hooks() noexcept;   // draw.cpp
void uninstall_device_hooks() noexcept;    // resource.cpp
void uninstall_present_hooks() noexcept;   // present.cpp
void release_post_targets(PostProcessor* p) noexcept;   // post.cpp

void uninstall_all_hooks() noexcept {
    uninstall_present_hooks();
    uninstall_context_hooks();
    uninstall_device_hooks();
}

D3D11ContextImpl::D3D11ContextImpl(const ContextBuilder& b, ID3D11Device* device, ID3D11DeviceContext* immediate,
                                   IDXGISwapChain* swap_chain)
    : ContextImpl(b), device_(device), immediate_(immediate), swap_chain_(swap_chain)
{
    store_.attach(device_, immediate_);
    snapshots_ = make_snapshots(*this);
    feature_level_ = static_cast<uint32_t>(device_->GetFeatureLevel());

    {
        std::lock_guard lk(g_install_mutex);
        install_device_hooks(device_);
        // Each device context has a vtable of its own: the immediate one is
        // hooked here, deferred ones as CreateDeferredContext makes them. Ones
        // made before this context attached are not seen (attach at device
        // creation with vangfx::AutoAttach to see them all).
        install_context_hooks(immediate_);
        const bool present_ok = swap_chain ? install_present_hooks(swap_chain) : prime_present_hooks(device_);
        if (!present_ok) {
            log(LogLevel::Warn, "D3D11: Present could not be hooked (no swap chain given, and none could be "
                                "made on the device). on_present will not fire.");
        }
    }

    hub_attach(this);
    log(LogLevel::Info, "D3D11 context attached (feature level " + std::to_string(feature_level_ >> 12) + "." +
                        std::to_string((feature_level_ >> 8) & 0xF) + ")");
}

D3D11ContextImpl::~D3D11ContextImpl() {
    run_teardown();
    hub_detach(this);
    stop_capture();   // no hook runs for this context now; its copies go before the snapshots do
    destroy_snapshots(snapshots_);
    snapshots_ = nullptr;
    destroy_post_processor(post_);
    post_ = nullptr;
    std::lock_guard lk(g_install_mutex);
    if (hub_idle()) uninstall_all_hooks();
}

void D3D11ContextImpl::adopt_swap_chain(IDXGISwapChain* sc) noexcept {
    IDXGISwapChain* expected = nullptr;
    if (swap_chain_.compare_exchange_strong(expected, sc, std::memory_order_acq_rel))
        log(LogLevel::Info, "D3D11: adopted the swap chain presenting on this context's device.");
}

D3D11ContextImpl::Timing D3D11ContextImpl::tick() noexcept {
    std::lock_guard lk(timing_mutex_);
    const auto now = Clock::now();
    Timing t;
    t.time  = std::chrono::duration<float>(now - start_).count();
    t.delta = std::chrono::duration<float>(now - last_).count();
    t.first = first_frame_;
    last_ = now;
    first_frame_ = false;
    return t;
}

void D3D11ContextImpl::post_process(IDXGISwapChain* swap_chain, const Timing& t) {
    run_post_passes(*this, post_, swap_chain, t);
}

void D3D11ContextImpl::release_post() noexcept { release_post_targets(post_); }

// ── Shaders and effects ──────────────────────────────────────────────────────

Result<Shader> D3D11ContextImpl::compile_shader(const ShaderBuilder& b) {
    return store_.register_shader(alloc_id(), b);
}

Result<Effect> D3D11ContextImpl::link_effect(Shader vs, Shader ps, Shader gs, Shader hs, Shader ds) {
    if (!is_valid(vs.handle) && !is_valid(ps.handle))
        return gfx_err<Effect>(ShaderError::InvalidFormat, "D3D11: link_effect needs at least a vertex or a pixel shader.");
    for (const Shader* s : { &vs, &ps, &gs, &hs, &ds }) {
        if (is_valid(s->handle) && !store_.has_shader(s->handle)) {
            return gfx_err<Effect>(ShaderError::InvalidHandle,
                "D3D11: link_effect was given a shader this context did not issue, or one already destroyed.");
        }
    }
    Effect e;
    e.handle = EffectHandle{ alloc_id() };
    e.vs = std::move(vs); e.ps = std::move(ps); e.gs = std::move(gs); e.hs = std::move(hs); e.ds = std::move(ds);
    std::unique_lock lk(effects_mutex_);
    effects_.emplace(e.handle.id, e);
    return e;
}

Result<Effect> D3D11ContextImpl::link_compute_effect(Shader cs) {
    if (!is_valid(cs.handle) || !store_.has_shader(cs.handle))
        return gfx_err<Effect>(ShaderError::InvalidHandle, "D3D11: link_compute_effect needs a compute shader this context issued.");
    Effect e;
    e.handle = EffectHandle{ alloc_id() };
    e.cs = std::move(cs);
    std::unique_lock lk(effects_mutex_);
    effects_.emplace(e.handle.id, e);
    return e;
}

void D3D11ContextImpl::destroy(ShaderHandle h) noexcept { store_.destroy_shader(h); }

void D3D11ContextImpl::destroy(EffectHandle h) noexcept {
    std::unique_lock lk(effects_mutex_);
    effects_.erase(h.id);
}

}  // namespace vangfx::backend::d3d11

// ─────────────────────────────────────────────────────────────────────────────
//  Factory — called by ContextBuilder::build()
// ─────────────────────────────────────────────────────────────────────────────

namespace vangfx::backend {

Result<std::unique_ptr<Context>> make_d3d11_context(const ContextBuilder& b) {
    auto* raw = static_cast<IUnknown*>(b.d3d11_device());
    if (!raw) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D11: no device supplied; call ContextBuilder::d3d11_device(ptr), or use vangfx::AutoAttach to be "
            "handed one as the game creates it.");
    }
    ID3D11Device* device = nullptr;
    if (FAILED(raw->QueryInterface(IID_PPV_ARGS(&device))) || !device) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D11: the pointer given to d3d11_device() is not an ID3D11Device.");
    }
    device->Release();

    IDXGISwapChain* swap_chain = nullptr;
    if (auto* raw_sc = static_cast<IUnknown*>(b.d3d11_swap_chain())) {
        if (FAILED(raw_sc->QueryInterface(IID_PPV_ARGS(&swap_chain))) || !swap_chain) {
            return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
                "D3D11: the pointer given to d3d11_swap_chain() is not an IDXGISwapChain.");
        }
        swap_chain->Release();
    }

    ID3D11DeviceContext* immediate = nullptr;
    device->GetImmediateContext(&immediate);
    if (!immediate) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized, "D3D11: the device has no immediate context.");
    }
    immediate->Release();   // the device keeps it alive for as long as the device lives

    return std::unique_ptr<Context>(std::make_unique<d3d11::D3D11ContextImpl>(b, device, immediate, swap_chain));
}

}  // namespace vangfx::backend
