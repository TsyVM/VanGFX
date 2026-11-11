#pragma once

/**
 * src/backend/d3d9/d3d9_impl.hpp — the D3D9 backend's shared declarations:
 * the hub (contexts, per-device state, registries), the frame context, the
 * context, and the hook installers.
 *
 * Direct3D 9 here means the IDirect3DDevice9 API of DirectX 9.0c (shader
 * models 1 to 3, and fixed-function) and the IDirect3DDevice9Ex of Windows
 * Vista and later, in 32- and 64-bit processes alike.
 *
 * EVERY DEVICE HAS A TABLE OF ITS OWN
 *
 * Direct3D 9 builds each device's vtable on the heap when the device is
 * created; two devices never share one, even with identical creation flags. So
 * the device hooks are installed per device (hooks::Site keeps an original per
 * table), and when a device's last reference goes its table is forgotten by
 * every site -- the memory is freed, and a later device may be given the same
 * address. An Ex device's table is longer (134 entries to 119), so the Ex-only
 * sites are kept apart and only ever touched on an Ex device's table. The other
 * classes (swap chain, query, state block, buffers, textures) share one table
 * in d3d9.dll -- an Ex device's objects included -- and are hooked once.
 *
 * THE RUNTIME REWRITES THE DEVICE'S TABLE
 *
 * BeginStateBlock rewrites the whole table from the runtime's own template,
 * with recording versions of seventeen setters (SetRenderState, SetTexture,
 * SetVertexShader, ...) and every other entry as it was -- so every hook in it
 * is dropped, the draws and Release included. EndStateBlock and Reset (even a
 * failed one; not an Ex device's) write the seventeen normal setters back.
 * Nothing else writes the table: measured with marker hooks, on hardware,
 * software, mixed, pure, multithreaded and Ex devices, 32- and 64-bit. Every
 * device site is Heal::SameModule and every device thunk repairs the table on
 * its way out (Healer), and the three calls that write it are device thunks,
 * so the table is ours again before the next call through it.
 *
 * Between BeginStateBlock and EndStateBlock the setters record into the block
 * and change nothing, so they are not reported. A state block applied later
 * sets shaders, textures and the FVF through the table (so those hooks see it)
 * and everything else without a call; the Apply hook re-reads the rest.
 *
 * WHAT ELSE GOES THROUGH THE TABLE
 *
 * Measured on Windows 11, 32- and 64-bit alike: Reset releases the device
 * through the table eight times; on an Ex device Reset and ResetEx call
 * SetViewport and SetScissorRect through it. Present, PresentEx and a swap
 * chain's Present are three separate paths (none calls another), and the UP
 * draws do not call the plain ones.
 */

#include "../../core/context_impl.hpp"
#include "store.hpp"
#include <compat/hook_site.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace vangfx::backend::d3d9 {

class D3D9ContextImpl;
class PostProcessor;   // post.cpp
class Snapshots;       // capture.cpp

// ── Hub ──────────────────────────────────────────────────────────────────────

struct ContextSet {
    static constexpr uint32_t kMax = 4;
    D3D9ContextImpl* items[kMax]{};
    uint32_t         count = 0;
    D3D9ContextImpl* const* begin() const noexcept { return items; }
    D3D9ContextImpl* const* end()   const noexcept { return items + count; }
    [[nodiscard]] explicit operator bool() const noexcept { return count != 0; }
};

void hub_attach(D3D9ContextImpl* ctx) noexcept;
void hub_detach(D3D9ContextImpl* ctx) noexcept;
[[nodiscard]] bool     hub_idle() noexcept;
[[nodiscard]] uint32_t hub_generation() noexcept;
[[nodiscard]] bool     hub_attached(const D3D9ContextImpl* ctx) noexcept;   // with a HubReader held

class HubReader {
public:
    HubReader() noexcept;
    ~HubReader();
    HubReader(const HubReader&) = delete;
    HubReader& operator=(const HubReader&) = delete;
};

[[nodiscard]] ContextSet contexts_for(const IDirect3DDevice9* device) noexcept;   // with a HubReader held

// VanGFX's own work on the device: the hooks pass it through unreported.
class InternalScope {
public:
    InternalScope() noexcept;
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
[[nodiscard]] bool internal() noexcept;

// ── Device state ─────────────────────────────────────────────────────────────

struct DevState {
    IDirect3DDevice9* device = nullptr;
    uint32_t          gen    = UINT32_MAX;
    ContextSet        contexts;

    // Between BeginStateBlock and EndStateBlock: setters record, they do not set.
    bool recording = false;

    // Shaders as the game set them (weak), what they hash to, and their shader
    // model's major version (0 for none: fixed-function).
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9*  ps = nullptr;
    uint64_t                vs_hash = 0, ps_hash = 0;
    uint8_t                 vs_major = 0, ps_major = 0;

    // An effect swapped these for the draw being reported; put back after it.
    bool vs_swapped = false;
    bool ps_swapped = false;

    FrameSurfaces surfaces{};
    PipelineState pipeline{};
};

// The state of a device, created (and read from the device) the first time it
// is seen. Null when the thread is doing internal work or no context is
// attached to the device. Call with a HubReader held.
[[nodiscard]] DevState* dev_state(IDirect3DDevice9* device) noexcept;

// The device's last reference went (draw.cpp's Release hook): its state goes,
// and its contexts are orphaned -- never matched again to a device that the
// runtime may later place at the same address.
void forget_device(const IDirect3DDevice9* device) noexcept;

// Re-read what a state block or a reset changed without a setter.
void read_shaders(IDirect3DDevice9* device, DevState& st) noexcept;              // hub.cpp
void read_pipeline(IDirect3DDevice9* device, PipelineState& out) noexcept;       // state.cpp
void read_surfaces(IDirect3DDevice9* device, DevState& st) noexcept;            // rendertarget.cpp
// Everything, after Reset put the device back to its defaults.
void reread_device(IDirect3DDevice9* device, DevState& st) noexcept;            // hub.cpp

// SetRenderTarget(0, ...) sets the viewport and scissor rectangle to the whole
// target without calling either setter: folded into the pipeline and reported
// as a state change (state.cpp).
void target_resets_viewport(DevState& st, uint32_t width, uint32_t height) noexcept;

// ── Registries ───────────────────────────────────────────────────────────────

// Resources: id, kind, device; destruction reported to the device's contexts.
// An alias record (a texture's top surface, reported under the texture's id)
// reports nothing when it goes: the texture reports for both.
uint64_t resource_register(IDirect3DResource9* res, ResourceKind kind, IDirect3DDevice9* device,
                           uint64_t alias_of = 0) noexcept;
[[nodiscard]] uint64_t            resource_id(const void* res) noexcept;
[[nodiscard]] bool                resource_lookup(const void* res, uint64_t& id, ResourceKind& kind) noexcept;
[[nodiscard]] IDirect3DResource9* resource_from_id(uint64_t id) noexcept;
// Registers and reports a resource first met somewhere other than its creation
// (made before attaching, or a back buffer). resource.cpp.
void announce_resource(IDirect3DDevice9* device, IDirect3DResource9* res) noexcept;
// The id a bound surface is reported by: the texture's for the top level of a
// 2D texture, the surface's own otherwise. Announces it when new.
[[nodiscard]] uint64_t surface_id(IDirect3DSurface9* surface, IDirect3DDevice9* device) noexcept;

// Queries offer no private data, so nothing says when they go; a new object at
// an old address is registered afresh when it is created.
struct QueryInfo {
    uint64_t          id     = 0;
    QueryType         type   = QueryType::Event;
    D3DQUERYTYPE      raw    = D3DQUERYTYPE_EVENT;
    IDirect3DDevice9* device = nullptr;   // weak: the query holds it
};
QueryInfo               query_register(IDirect3DQuery9* query) noexcept;   // at creation: a fresh id
[[nodiscard]] QueryInfo query_info(IDirect3DQuery9* query) noexcept;       // registers one made before attaching

// Shaders: the hash of their bytecode and their shader model. Shaders offer no
// private data either; creation replaces a stale record. One made before
// VanGFX attached is read back (GetFunction) the first time it is asked about,
// so it is named by its hash like any other.
struct ShaderInfo { uint64_t hash = 0; uint8_t major = 0; };
void shader_register(const void* shader, const void* code, uint32_t size) noexcept;
[[nodiscard]] ShaderInfo shader_info(IDirect3DVertexShader9* vs) noexcept;
[[nodiscard]] ShaderInfo shader_info(IDirect3DPixelShader9* ps) noexcept;
// The major version in a shader's first token (vs_2_0 -> 2), 0 for none.
[[nodiscard]] inline uint8_t shader_major(const void* code) noexcept {
    return code ? static_cast<uint8_t>((*static_cast<const DWORD*>(code) >> 8) & 0xFF) : 0;
}

// ── Frame context ────────────────────────────────────────────────────────────

class D3D9FrameContextImpl final : public vangfx::FrameContext {
public:
    D3D9FrameContextImpl(D3D9ContextImpl& ctx, uint32_t frame_index, DevState* state = nullptr,
                         IDirect3DSwapChain9* swap_chain = nullptr) noexcept
        : ctx_(ctx), frame_index_(frame_index), state_(state), swap_chain_(swap_chain) {}

    BackendKind backend()     const noexcept override { return BackendKind::D3D9; }
    uint32_t    frame_index() const noexcept override { return frame_index_; }

    FrameSurfaces surfaces()  const noexcept override;
    PipelineState pipeline()  const noexcept override;

    SurfaceFormat format_of(RenderTargetHandle h) const noexcept override;
    SurfaceFormat format_of(DepthBufferHandle h)  const noexcept override;
    SurfaceFormat format_of(TextureHandle h)      const noexcept override;

    Result<TextureHandle>      create_texture(const TextureDesc& d) override;
    Result<RenderTargetHandle> create_render_target(const RenderTargetDesc& d) override;
    Result<DepthBufferHandle>  create_depth_buffer(const DepthBufferDesc& d) override;
    void destroy(TextureHandle h)      noexcept override;
    void destroy(RenderTargetHandle h) noexcept override;
    void destroy(DepthBufferHandle h)  noexcept override;

    Result<void> bind_effect(const Effect& e) override;
    void         unbind_effect() noexcept override;

    Result<void> update_constant_buffer(ConstantBufferHandle h, const void* data, uint32_t size) override;

    Result<ReadbackData> readback(TextureHandle h)      override;
    Result<ReadbackData> readback(RenderTargetHandle h) override;
    Result<ReadbackData> readback(DepthBufferHandle h)  override;
    Result<BufferData>   readback(VertexBufferHandle h,     uint64_t, uint64_t) override;
    Result<BufferData>   readback(IndexBufferHandle h,      uint64_t, uint64_t) override;
    Result<BufferData>   readback(ConstantBufferHandle h,   uint64_t, uint64_t) override;
    Result<BufferData>   readback(StructuredBufferHandle h, uint64_t, uint64_t) override;
    Result<BufferData>   readback(RawBufferHandle h,        uint64_t, uint64_t) override;
    Result<BufferData>   readback(UAVHandle h,              uint64_t, uint64_t) override;
    // D3D9 binds buffers as interface pointers: there is no GPU address.
    BufferRef            buffer_at(uint64_t) const noexcept override { return {}; }
    Result<BufferData>   readback_at(uint64_t, uint64_t) override;

    bool                   capture_active() const noexcept override;
    Result<GBufferCapture> latest_capture() override;

    void* native_device()  const noexcept override;
    void* native_context() const noexcept override;   // the device: D3D9 has no separate context
    void* native_resource(RenderTargetHandle h) const noexcept override;
    void* native_resource(DepthBufferHandle h)  const noexcept override;
    void* native_resource(TextureHandle h)      const noexcept override;
    void* native_buffer(uint64_t id)            const noexcept override;

    // The chain presenting, or null for the device's own.
    [[nodiscard]] IDirect3DSwapChain9* swap_chain() const noexcept { return swap_chain_; }

private:
    D3D9ContextImpl&     ctx_;
    uint32_t             frame_index_;
    DevState*            state_;
    IDirect3DSwapChain9* swap_chain_;
};

// ── Context ──────────────────────────────────────────────────────────────────

class D3D9ContextImpl final : public vangfx::ContextImpl {
public:
    D3D9ContextImpl(const ContextBuilder& b, IDirect3DDevice9* device, bool is_ex);
    ~D3D9ContextImpl() override;

    BackendKind backend()       const noexcept override { return BackendKind::D3D9; }
    uint32_t    d3d9_ver()      const noexcept override { return is_ex_ ? 11u : 9u; }
    void*       native_device() const noexcept override { return device_; }

    Result<Shader> compile_shader(const ShaderBuilder& b) override;
    Result<Effect> link_effect(Shader vs, Shader ps, Shader gs, Shader hs, Shader ds) override;
    Result<Effect> link_compute_effect(Shader cs) override;
    void destroy(ShaderHandle h) noexcept override;
    void destroy(EffectHandle h) noexcept override;

    [[nodiscard]] IDirect3DDevice9*  device() const noexcept { return device_; }
    [[nodiscard]] bool               is_ex()  const noexcept { return is_ex_; }
    [[nodiscard]] D3D9ResourceStore& store()  noexcept { return store_; }

    // The device was destroyed before this context: it reports nothing more.
    void orphan() noexcept { orphaned_.store(true, std::memory_order_release); }
    [[nodiscard]] bool orphaned() const noexcept { return orphaned_.load(std::memory_order_acquire); }

    [[nodiscard]] uint32_t current_frame_index() const noexcept { return frame_index_.load(std::memory_order_relaxed); }
    uint32_t next_frame() noexcept { return frame_index_.fetch_add(1, std::memory_order_relaxed); }

    struct Timing { float time = 0.0f, delta = 0.0f; bool first = true; };
    Timing tick() noexcept;

    std::string post_pass_profile() const override { return "ps_3_0"; }
    // `swap_chain` is the chain presenting, or null for the device's own.
    void post_process(IDirect3DSwapChain9* swap_chain, const Timing& t);
    // Before Reset: everything in the default pool VanGFX holds (a plain
    // device's Reset fails while any of it is alive; an Ex device's does not).
    void release_for_reset() noexcept;

    capture::SnapshotHost* snapshot_host() noexcept override;   // capture.cpp

private:
    IDirect3DDevice9*     device_ = nullptr;
    bool                  is_ex_  = false;
    std::atomic<bool>     orphaned_{ false };
    std::atomic<uint32_t> frame_index_{ 0 };
    D3D9ResourceStore     store_;
    PostProcessor*        post_ = nullptr;
    Snapshots*            snapshots_ = nullptr;

    mutable std::shared_mutex            effects_mutex_;
    std::unordered_map<uint64_t, Effect> effects_;

    using Clock = std::chrono::steady_clock;
    std::mutex        timing_mutex_;
    Clock::time_point start_ = Clock::now();
    Clock::time_point last_  = Clock::now();
    bool              first_frame_ = true;
};

// ── Hook installation ────────────────────────────────────────────────────────
//
// Each installer patches through the device given: its own table for the
// device sites, the shared tables for the rest. `ex` says the device is an
// IDirect3DDevice9Ex, whose table has the Ex entries. Taken out when the last
// D3D9 context goes.

bool install_device_hooks(IDirect3DDevice9* device, bool ex) noexcept;   // draw.cpp: all of them
void install_state_hooks(IDirect3DDevice9* device) noexcept;             // state.cpp
void install_rendertarget_hooks(IDirect3DDevice9* device) noexcept;      // rendertarget.cpp
void install_resource_hooks(IDirect3DDevice9* device, bool ex) noexcept; // resource.cpp
void install_query_hooks(IDirect3DDevice9* device) noexcept;             // query.cpp
void install_present_hooks(IDirect3DDevice9* device, bool ex) noexcept;  // present.cpp
void uninstall_all_hooks() noexcept;

// Device sites are healed after every device thunk and forgotten with their
// table; Ex device sites the same, on Ex devices only; class sites (shared
// tables) only come out at uninstall.
void register_device_site(hooks::Site& site) noexcept;
void register_ex_device_site(hooks::Site& site) noexcept;
void register_class_site(hooks::Site& site) noexcept;
void heal_device(IDirect3DDevice9* device) noexcept;

// Put first in a device thunk: repairs the table when the thunk returns.
struct Healer {
    IDirect3DDevice9* device;
    ~Healer() { heal_device(device); }
};

// SetVertexShader / SetPixelShader past the hooks, for per-draw swaps (draw.cpp).
void set_vs_unreported(IDirect3DDevice9* device, IDirect3DVertexShader9* vs) noexcept;
void set_ps_unreported(IDirect3DDevice9* device, IDirect3DPixelShader9* ps) noexcept;
// Put back what an effect swapped for a draw.
void restore_shaders(IDirect3DDevice9* device, DevState& st) noexcept;

// post.cpp
void destroy_post_processor(PostProcessor* p) noexcept;
void release_post_targets(PostProcessor* p) noexcept;
void run_post_passes(D3D9ContextImpl& ctx, PostProcessor*& processor, IDirect3DSwapChain9* swap_chain,
                     const D3D9ContextImpl::Timing& t);

// capture.cpp: the copies a capture takes, and the hooks' reports to it.
Snapshots* make_snapshots(D3D9ContextImpl& ctx) noexcept;
void destroy_snapshots(Snapshots* s) noexcept;
void drop_snapshots(Snapshots* s) noexcept;                       // before Reset: copies waiting go
void capture_bind(IDirect3DDevice9* device, DevState& st);        // after the bound targets changed
void capture_draw(IDirect3DDevice9* device, DevState& st);

}  // namespace vangfx::backend::d3d9
