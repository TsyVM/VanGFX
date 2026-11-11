/**
 * src/backend/d3d12/context.cpp — D3D12ContextImpl and make_d3d12_context.
 *
 * Building a context:
 *   1. Check the device and queue the builder was given.
 *   2. Install the hooks. They go on the classes' vtables, shared by every
 *      device, queue, list and swap chain in the process, so each install is a
 *      no-op after the first: device and queue hooks through the objects given;
 *      command-list hooks through a direct list and a bundle made for the
 *      purpose (their classes differ), which also covers lists the application
 *      already has; Present through the application's swap chain, or a small one
 *      made on the queue when none was given.
 *   3. Attach to the hub. From then on the hooks report calls on this device.
 *
 * Tearing down: effect and capture teardown first, while the context still
 * works; then detach (the hub waits for every dispatch in flight); then wait for
 * the queue to finish what VanGFX submitted or built and release it. The hooks
 * come out when the last D3D12 context goes.
 */

#include "d3d12_impl.hpp"
#include "effect_bridge.hpp"
#include <core/shader_compile.hpp>

#include <cstring>
#include <string>

namespace vangfx::backend::d3d12 {

namespace {

// Highest shader model the device supports, as the public API writes it: 60
// for SM 6.0, 66 for SM 6.6. The query lowers the model it is asked about to
// what the device has, and fails outright on a runtime older than the model
// asked about, so walk down until one is accepted.
uint32_t query_shader_model(ID3D12Device* device) noexcept {
    static constexpr D3D_SHADER_MODEL kCandidates[] = {
        D3D_SHADER_MODEL(0x69), D3D_SHADER_MODEL(0x68), D3D_SHADER_MODEL(0x67), D3D_SHADER_MODEL(0x66),
        D3D_SHADER_MODEL(0x65), D3D_SHADER_MODEL(0x64), D3D_SHADER_MODEL(0x63), D3D_SHADER_MODEL(0x62),
        D3D_SHADER_MODEL(0x61), D3D_SHADER_MODEL(0x60),
    };
    for (auto candidate : kCandidates) {
        D3D12_FEATURE_DATA_SHADER_MODEL data{ candidate };
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
            const uint32_t raw = static_cast<uint32_t>(data.HighestShaderModel);
            return ((raw >> 4) * 10) + (raw & 0xF);
        }
    }
    return 0;
}

uint32_t query_raytracing_tier(ID3D12Device* device) noexcept {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts, sizeof(opts)))) return 0;
    switch (opts.RaytracingTier) {
    case D3D12_RAYTRACING_TIER_1_0: return 1;
    case D3D12_RAYTRACING_TIER_1_1: return 11;
    default:                        return 0;
    }
}

uint32_t query_mesh_shader_tier(ID3D12Device* device) noexcept {
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts, sizeof(opts)))) return 0;
    return opts.MeshShaderTier == D3D12_MESH_SHADER_TIER_1 ? 1u : 0u;
}

ShaderStage stage_from_profile(const std::string& profile) noexcept {
    const std::string_view p{ profile };
    if (p.starts_with("ps")) return ShaderStage::Pixel;
    if (p.starts_with("gs")) return ShaderStage::Geometry;
    if (p.starts_with("hs")) return ShaderStage::Hull;
    if (p.starts_with("ds")) return ShaderStage::Domain;
    if (p.starts_with("cs")) return ShaderStage::Compute;
    return ShaderStage::Vertex;
}

// Hooks are shared by every D3D12 context; the last one to go takes them out.
std::mutex g_install_mutex;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Construction and teardown
// ─────────────────────────────────────────────────────────────────────────────

D3D12ContextImpl::D3D12ContextImpl(const ContextBuilder& b, ID3D12Device* device,
                                   ID3D12CommandQueue* queue, IDXGISwapChain* swap_chain)
    : ContextImpl(b), device_(device), queue_(queue), swap_chain_(swap_chain)
{
    if (!store_.attach(device_, queue_)) {
        log(LogLevel::Warn,
            "D3D12: could not create the descriptor heaps or copy machinery. Resource "
            "creation, readback and uniforms will report an error; events still work.");
    }
    snapshots_ = make_snapshots(*this);

    shader_model_     = query_shader_model(device_);
    raytracing_tier_  = query_raytracing_tier(device_);
    mesh_shader_tier_ = query_mesh_shader_tier(device_);

    {
        std::lock_guard lk(g_install_mutex);
        install_device_hooks(device_);
        install_queue_hooks(queue_);
        if (!prime_list_hooks(device_)) {
            log(LogLevel::Error,
                "D3D12: could not make a command list to install the list hooks through. "
                "Draw, state, barrier and query events will not fire.");
        }
        if (!install_factory_hooks()) {
            log(LogLevel::Warn, "D3D12: the DXGI factory could not be hooked; post passes run on the queue this "
                                "context was given rather than the one each swap chain presents on.");
        }
        const bool present_ok = swap_chain ? install_present_hooks(swap_chain) : prime_present_hooks(queue_);
        if (!present_ok) {
            log(LogLevel::Warn,
                "D3D12: Present could not be hooked (no swap chain given, and none could be made "
                "on the queue). on_present will not fire.");
        }
    }

    hub_attach(this);
    log(LogLevel::Info,
        "D3D12 context attached (SM " + std::to_string(shader_model_ / 10) + "." +
        std::to_string(shader_model_ % 10) + ")");
}

D3D12ContextImpl::~D3D12ContextImpl()
{
    run_teardown();
    hub_detach(this);
    // No hook runs for this context any more: the capture can go, handing its
    // copies back to the snapshots, which wait for the GPU before freeing them.
    stop_capture();
    destroy_snapshots(snapshots_);
    snapshots_ = nullptr;

    // Shadow pipelines and store resources may be referenced by work still on
    // the queue. Bounded: a queue waiting on a fence only the application will
    // signal would otherwise hang the caller.
    if (!store_.wait_idle(2000)) {
        log(LogLevel::Warn, "D3D12: the queue did not drain within 2 s of the context being destroyed; "
                            "releasing its resources anyway.");
    }
    effect_bridge::context_destroyed(*this);
    destroy_post_processor(post_);
    post_ = nullptr;

    std::lock_guard lk(g_install_mutex);
    if (hub_idle()) uninstall_all_hooks();
}

void D3D12ContextImpl::adopt_swap_chain(IDXGISwapChain* sc) noexcept {
    IDXGISwapChain* expected = nullptr;
    if (swap_chain_.compare_exchange_strong(expected, sc, std::memory_order_acq_rel))
        log(LogLevel::Info, "D3D12: adopted the swap chain presenting on this context's queue.");
}

std::string D3D12ContextImpl::post_pass_profile() const { return detail::d3d12_default_profile("ps"); }

void D3D12ContextImpl::post_process(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue, const Timing& t) {
    run_post_passes(*this, post_, swap_chain, queue, t);
}

void D3D12ContextImpl::note_surfaces(const FrameSurfaces& s) noexcept {
    std::lock_guard lk(surfaces_mutex_);
    last_surfaces_ = s;
}

FrameSurfaces D3D12ContextImpl::last_surfaces() const noexcept {
    std::lock_guard lk(surfaces_mutex_);
    return last_surfaces_;
}

D3D12ContextImpl::Timing D3D12ContextImpl::tick() noexcept {
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

// ─────────────────────────────────────────────────────────────────────────────
//  Shaders and effects
//
//  D3D12 has no shader object: bytecode goes into a pipeline state and nowhere
//  else. A Shader here is the stored bytecode under an id, and an Effect a set
//  of those; the effect bridge turns one into a pipeline when a draw asks.
// ─────────────────────────────────────────────────────────────────────────────

Result<Shader> D3D12ContextImpl::compile_shader(const ShaderBuilder& b) {
    const auto& blob = b.precompiled_bytecode();
    if (blob.empty()) {
        return gfx_err<Shader>(ShaderError::CompileFailed,
            "D3D12: compile_shader received no bytecode. Build shaders through "
            "ShaderBuilder::build(ctx), which compiles the source first.");
    }
    Shader shader;
    shader.handle   = ShaderHandle{ alloc_id() };
    shader.lang     = ShaderLang::DXIL;
    shader.entry    = b.entry_point();
    shader.profile  = b.shader_profile();
    shader.stage    = stage_from_profile(shader.profile);
    shader.bytecode = blob;
    ShaderRecord rec{ shader.stage, shader.lang, shader.entry, shader.profile, blob, b, {}, false };
    rec.source.bytecode(nullptr, 0);   // the source is what is kept; the blob is above
    {
        std::unique_lock lk(shaders_mutex_);
        shaders_.emplace(shader.handle.id, std::move(rec));
    }
    return shader;
}

namespace {

// Whether a shader container holds DXIL (SM 6) or DXBC (SM 5 and older). Both
// use the "DXBC" container; the chunk that holds the program says which.
enum class Container { Unknown, Dxbc, Dxil };

Container container_of(const void* data, size_t size) noexcept {
    const auto* b = static_cast<const uint8_t*>(data);
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) return Container::Unknown;
    uint32_t count = 0;
    std::memcpy(&count, b + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4 * (i + 1) <= size; ++i) {
        uint32_t off = 0;
        std::memcpy(&off, b + 32 + 4 * i, 4);
        if (off + 4 > size) break;
        const char* fourcc = reinterpret_cast<const char*>(b + off);
        if (std::memcmp(fourcc, "DXIL", 4) == 0) return Container::Dxil;
        if (std::memcmp(fourcc, "SHEX", 4) == 0 || std::memcmp(fourcc, "SHDR", 4) == 0) return Container::Dxbc;
    }
    return Container::Unknown;
}

Container container_of(const std::vector<uint8_t>& b) noexcept { return container_of(b.data(), b.size()); }

// "ps_6_6" -> "ps_5_1", "vs_5_0" -> "vs_6_0".
std::string retarget(const std::string& profile, bool dxil) {
    const std::string stage = profile.size() >= 2 ? profile.substr(0, 2) : std::string("ps");
    return stage + (dxil ? "_6_0" : "_5_1");
}

} // namespace

bool bytecode_is_dxil(const void* data, size_t size) noexcept {
    return container_of(data, size) == Container::Dxil;
}

std::vector<uint8_t> D3D12ContextImpl::shader_bytecode_as(ShaderHandle h, bool dxil, std::string& why) {
    ShaderBuilder source;
    std::string   profile;
    {
        std::shared_lock lk(shaders_mutex_);
        const auto it = shaders_.find(h.id);
        if (it == shaders_.end()) { why = "the shader was destroyed"; return {}; }
        const ShaderRecord& r = it->second;
        const Container have = container_of(r.bytecode);
        if (have == Container::Unknown || (have == Container::Dxil) == dxil) return r.bytecode;
        if (!r.other.empty()) return r.other;
        if (r.other_failed) { why = "recompiling it for the game's shader format failed earlier"; return {}; }
        source  = r.source;
        profile = r.profile;
    }

    auto bytes = vangfx::detail::compile_hlsl(source, retarget(profile, dxil));
    std::unique_lock lk(shaders_mutex_);
    const auto it = shaders_.find(h.id);
    if (it == shaders_.end()) { why = "the shader was destroyed"; return {}; }
    if (!bytes) {
        it->second.other_failed = true;
        why = std::string("the game's pipelines use ") + (dxil ? "DXIL (SM 6)" : "DXBC (SM 5)") +
              " and recompiling the replacement for that failed: " + bytes.error().message;
        log(LogLevel::Error, "D3D12: " + why);
        return {};
    }
    it->second.other = std::move(*bytes);
    return it->second.other;
}

std::vector<uint8_t> D3D12ContextImpl::shader_bytecode(ShaderHandle h) const {
    std::shared_lock lk(shaders_mutex_);
    const auto it = shaders_.find(h.id);
    return it != shaders_.end() ? it->second.bytecode : std::vector<uint8_t>{};
}

void D3D12ContextImpl::destroy(ShaderHandle h) noexcept {
    std::unique_lock lk(shaders_mutex_);
    shaders_.erase(h.id);
}

Result<Effect> D3D12ContextImpl::link_effect(Shader vs, Shader ps, Shader gs, Shader hs, Shader ds) {
    if (!vs.valid() && !ps.valid()) {
        return gfx_err<Effect>(ShaderError::InvalidFormat, "D3D12: link_effect needs at least a vertex or a pixel shader.");
    }
    {
        std::shared_lock lk(shaders_mutex_);
        for (const auto* stage : { &vs, &ps, &gs, &hs, &ds }) {
            if (stage->valid() && !shaders_.contains(stage->handle.id)) {
                return gfx_err<Effect>(ShaderError::InvalidHandle,
                    "D3D12: link_effect was given a shader this context did not issue, or one already destroyed.");
            }
        }
    }
    Effect effect;
    effect.handle = EffectHandle{ alloc_id() };
    effect.vs = std::move(vs); effect.ps = std::move(ps);
    effect.gs = std::move(gs); effect.hs = std::move(hs); effect.ds = std::move(ds);
    {
        std::unique_lock lk(effects_mutex_);
        effects_.emplace(effect.handle.id, effect);
    }
    // Pipelines created from now on are recorded, so a draw using one can be
    // rebuilt with this Effect's shaders.
    if (!record_pipelines_.exchange(true, std::memory_order_acq_rel)) {
        log(LogLevel::Info, "D3D12: an Effect was linked; pipelines created from now on are recorded so "
                            "their draws can be swapped.");
    }
    return effect;
}

Result<Effect> D3D12ContextImpl::link_compute_effect(Shader cs) {
    if (!cs.valid()) return gfx_err<Effect>(ShaderError::InvalidFormat, "D3D12: link_compute_effect needs a compute shader.");
    {
        std::shared_lock lk(shaders_mutex_);
        if (!shaders_.contains(cs.handle.id)) {
            return gfx_err<Effect>(ShaderError::InvalidHandle,
                "D3D12: link_compute_effect was given a shader this context did not issue, or one already destroyed.");
        }
    }
    Effect effect;
    effect.handle = EffectHandle{ alloc_id() };
    effect.cs = std::move(cs);
    std::unique_lock lk(effects_mutex_);
    effects_.emplace(effect.handle.id, effect);
    return effect;
}

void D3D12ContextImpl::destroy(EffectHandle h) noexcept {
    {
        std::unique_lock lk(effects_mutex_);
        effects_.erase(h.id);
    }
    effect_bridge::effect_destroyed(*this, h.id);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Factory
// ─────────────────────────────────────────────────────────────────────────────

Result<std::unique_ptr<vangfx::Context>> make_d3d12_context(const vangfx::ContextBuilder& b) {
    auto* raw_device = static_cast<IUnknown*>(b.d3d12_device());
    auto* raw_queue  = static_cast<IUnknown*>(b.d3d12_command_queue());
    if (!raw_device) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D12: no device supplied; call ContextBuilder::d3d12_device(ptr), or use vangfx::AutoAttach "
            "to be handed one as the game creates it.");
    }
    if (!raw_queue) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D12: no command queue supplied; call ContextBuilder::d3d12_command_queue(ptr). A device "
            "cannot be asked which queues it has, so this cannot be found for you.");
    }

    // Check both are what they claim before any vtable is patched through them:
    // a wrong pointer would have the patch land in whatever it does point at.
    ID3D12Device* device = nullptr;
    if (FAILED(raw_device->QueryInterface(IID_PPV_ARGS(&device))) || !device) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D12: the pointer given to d3d12_device() is not an ID3D12Device.");
    }
    device->Release();
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(raw_queue->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D12: the pointer given to d3d12_command_queue() is not an ID3D12CommandQueue.");
    }
    queue->Release();
    if (device_of(queue) != device) {
        return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
            "D3D12: the command queue belongs to a different device.");
    }

    if (const uint32_t required = b.d3d12_min_shader_model_value(); required) {
        const uint32_t have = query_shader_model(device);
        if (have < required) {
            return gfx_err<std::unique_ptr<Context>>(ShaderError::Unsupported,
                "D3D12: device supports shader model " + std::to_string(have / 10) + "." +
                std::to_string(have % 10) + ", below the " + std::to_string(required / 10) + "." +
                std::to_string(required % 10) + " required by d3d12_min_shader_model().");
        }
    }

    IDXGISwapChain* swap_chain = nullptr;
    if (auto* raw_sc = static_cast<IUnknown*>(b.d3d12_swap_chain())) {
        if (FAILED(raw_sc->QueryInterface(IID_PPV_ARGS(&swap_chain))) || !swap_chain) {
            return gfx_err<std::unique_ptr<Context>>(ShaderError::NotInitialized,
                "D3D12: the pointer given to d3d12_swap_chain() is not an IDXGISwapChain.");
        }
        swap_chain->Release();
    }

    return std::unique_ptr<Context>(std::make_unique<D3D12ContextImpl>(b, device, queue, swap_chain));
}

}  // namespace vangfx::backend::d3d12

namespace vangfx::backend {
Result<std::unique_ptr<Context>> make_d3d12_context(const ContextBuilder& b) { return d3d12::make_d3d12_context(b); }
}  // namespace vangfx::backend
