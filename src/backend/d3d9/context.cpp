/**
 * src/backend/d3d9/context.cpp — D3D9ContextImpl and make_d3d9_context.
 *
 * Building a context: check the device, install the hooks on its own table and
 * on the shared class tables (through objects made for the purpose), then
 * attach. The old context patched nine device slots through a pointer to the
 * device and restored them through the same pointer, kept one global "current
 * context" (a second context replaced the first for every hook), and never
 * installed its resource, state or query hooks at all.
 *
 * Tearing down: effect and capture teardown while the context still works,
 * detach (the hub waits for dispatches in flight), release the post-processing
 * resources; the hooks come out when the last D3D9 context goes.
 */

#include "d3d9_impl.hpp"

#include <string>

namespace vangfx::backend::d3d9 {

namespace {
std::mutex g_install_mutex;
}

D3D9ContextImpl::D3D9ContextImpl(const ContextBuilder& b, IDirect3DDevice9* device, bool is_ex)
    : ContextImpl(b), device_(device), is_ex_(is_ex)
{
    store_.attach(device_);
    snapshots_ = make_snapshots(*this);
    {
        std::lock_guard lk(g_install_mutex);
        if (!install_device_hooks(device_, is_ex_))
            log(LogLevel::Warn, "D3D9: some device hooks could not be installed (the table's page could not be made writable).");
    }
    hub_attach(this);
    log(LogLevel::Info, is_ex_ ? "D3D9Ex context attached" : "D3D9 context attached");
}

D3D9ContextImpl::~D3D9ContextImpl() {
    run_teardown();
    hub_detach(this);
    stop_capture();   // no hook runs for this context now; its copies go before the snapshots do
    destroy_snapshots(snapshots_);
    snapshots_ = nullptr;
    destroy_post_processor(post_);
    post_ = nullptr;
    store_.release_all();
    std::lock_guard lk(g_install_mutex);
    if (hub_idle()) uninstall_all_hooks();
}

D3D9ContextImpl::Timing D3D9ContextImpl::tick() noexcept {
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

void D3D9ContextImpl::post_process(IDirect3DSwapChain9* swap_chain, const Timing& t) {
    run_post_passes(*this, post_, swap_chain, t);
}

void D3D9ContextImpl::release_for_reset() noexcept {
    release_post_targets(post_);
    drop_snapshots(snapshots_);
    if (!is_ex_) store_.release_default_pool();
}

// ── Shaders and effects ──────────────────────────────────────────────────────

Result<Shader> D3D9ContextImpl::compile_shader(const ShaderBuilder& b) {
    return store_.register_shader(alloc_id(), b);
}

Result<Effect> D3D9ContextImpl::link_effect(Shader vs, Shader ps, Shader gs, Shader hs, Shader ds) {
    // Direct3D 9 has no geometry, hull or domain stage. Silently dropping one
    // would produce an Effect that does less than it was asked for.
    if (is_valid(gs.handle) || is_valid(hs.handle) || is_valid(ds.handle)) {
        return gfx_err<Effect>(ShaderError::Unsupported,
            "D3D9: there is no geometry, hull or domain stage on this API. Link only a vertex and a pixel shader.");
    }
    if (!is_valid(vs.handle) && !is_valid(ps.handle))
        return gfx_err<Effect>(ShaderError::InvalidFormat, "D3D9: link_effect needs at least a vertex or a pixel shader.");
    if (is_valid(vs.handle) && !store_.vertex_shader(vs.handle)) {
        return gfx_err<Effect>(ShaderError::InvalidHandle,
            "D3D9: the vertex shader is not one this context issued, or has already been destroyed.");
    }
    if (is_valid(ps.handle) && !store_.pixel_shader(ps.handle)) {
        return gfx_err<Effect>(ShaderError::InvalidHandle,
            "D3D9: the pixel shader is not one this context issued, or has already been destroyed.");
    }
    Effect e;
    e.handle = EffectHandle{ alloc_id() };
    e.vs = std::move(vs);
    e.ps = std::move(ps);
    std::unique_lock lk(effects_mutex_);
    effects_.emplace(e.handle.id, e);
    return e;
}

Result<Effect> D3D9ContextImpl::link_compute_effect(Shader) {
    return gfx_err<Effect>(ShaderError::Unsupported, "D3D9: there is no compute stage on this API.");
}

void D3D9ContextImpl::destroy(ShaderHandle h) noexcept { store_.destroy_shader(h); }

void D3D9ContextImpl::destroy(EffectHandle h) noexcept {
    std::unique_lock lk(effects_mutex_);
    effects_.erase(h.id);
}

}  // namespace vangfx::backend::d3d9

// ─────────────────────────────────────────────────────────────────────────────
//  Factory — called by ContextBuilder::build()
// ─────────────────────────────────────────────────────────────────────────────

namespace vangfx::backend {

Result<std::unique_ptr<Context>> make_d3d9_context(const ContextBuilder& b) {
    auto* raw = static_cast<IUnknown*>(b.d3d9_device());
    if (!raw) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D9: no device supplied; call ContextBuilder::d3d9_device(ptr), or use vangfx::AutoAttach to be "
            "handed one as the game creates it.");
    }
    IDirect3DDevice9* device = nullptr;
    if (FAILED(raw->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(&device))) || !device) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D9: the pointer given to d3d9_device() is not an IDirect3DDevice9.");
    }
    device->Release();   // the caller's reference keeps it alive

    // An Ex device's table has the Ex entries; a plain device's does not.
    bool is_ex = false;
    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex))) && ex) {
        is_ex = true;
        ex->Release();
    }
    return std::unique_ptr<Context>(std::make_unique<d3d9::D3D9ContextImpl>(b, device, is_ex));
}

}  // namespace vangfx::backend
