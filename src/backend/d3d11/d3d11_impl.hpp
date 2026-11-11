#pragma once

/**
 * src/backend/d3d11/d3d11_impl.hpp — the D3D11 backend's shared declarations:
 * the hub (contexts, per-device-context state, registries), the frame context,
 * the context, and the hook installers.
 *
 * HOOKS ARE PER CLASS
 *
 * Every device, device context and swap chain of a class shares one vtable, so
 * each hook is installed once (hooks::Site) and serves every object; a thunk
 * asks the hub which contexts are attached to the device behind the object it
 * was called on and passes straight through when there are none.
 *
 * THE IMMEDIATE CONTEXT REWRITES ITS OWN TABLE
 *
 * D3D11 swaps entries of the immediate context's vtable between its own code
 * paths: Flush (so Present) puts a different Draw in the slot, and that Draw
 * puts the first back. A patch made once is gone after the first frame, which is
 * why on_draw used to fire for one frame only. Every device-context hook is a
 * Heal::SameModule site, and heal_context() repairs all of them after each draw,
 * Flush, ClearState, ExecuteCommandList and Present. Direct3D 11.1 and later add
 * more that rewrite it -- SwapDeviceContextState and ID3D11Multithread::
 * SetMultithreadProtected (both finishing lazily, at the next call into the
 * context), Flush1, Signal and Wait -- and context1.cpp settles the table after
 * each.
 *
 * STATE IS PER DEVICE CONTEXT
 *
 * The immediate context and every deferred context has its own bindings, so
 * surfaces, pipeline state and the shaders a draw runs are kept per
 * ID3D11DeviceContext (DcState), written only by the thread using it.
 */

#include "../../core/context_impl.hpp"
#include "store.hpp"
#include <compat/hook_site.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d11 {

class D3D11ContextImpl;
class PostProcessor;   // post.cpp
class Snapshots;       // capture.cpp

// ── Hub ──────────────────────────────────────────────────────────────────────

struct ContextSet {
    static constexpr uint32_t kMax = 4;
    D3D11ContextImpl* items[kMax]{};
    uint32_t          count = 0;
    D3D11ContextImpl* const* begin() const noexcept { return items; }
    D3D11ContextImpl* const* end()   const noexcept { return items + count; }
    [[nodiscard]] explicit operator bool() const noexcept { return count != 0; }
};

void hub_attach(D3D11ContextImpl* ctx) noexcept;
void hub_detach(D3D11ContextImpl* ctx) noexcept;
[[nodiscard]] bool     hub_idle() noexcept;
[[nodiscard]] uint32_t hub_generation() noexcept;
[[nodiscard]] bool     hub_attached(const D3D11ContextImpl* ctx) noexcept;   // with a HubReader held

class HubReader {
public:
    HubReader() noexcept;
    ~HubReader();
    HubReader(const HubReader&) = delete;
    HubReader& operator=(const HubReader&) = delete;
};

[[nodiscard]] ContextSet contexts_for(const ID3D11Device* device) noexcept;   // with a HubReader held
[[nodiscard]] ID3D11Device* device_of(ID3D11DeviceChild* child) noexcept;     // weak

// VanGFX's own work on the device: the hooks pass it through unreported.
class InternalScope {
public:
    InternalScope() noexcept;
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
[[nodiscard]] bool internal() noexcept;

// ── Device context state ─────────────────────────────────────────────────────

// The bindings VanGFX follows: everything a device context state object
// (ID3DDeviceContextState) holds of what is reported.
struct DcBindings {
    // Shaders as the game set them (weak), and what they hash to.
    ID3D11VertexShader*  vs = nullptr;
    ID3D11PixelShader*   ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    uint64_t             vs_hash = 0, ps_hash = 0, cs_hash = 0;
    static constexpr UINT kMaxInstances = 8;
    ID3D11ClassInstance* vs_inst[kMaxInstances]{};
    ID3D11ClassInstance* ps_inst[kMaxInstances]{};
    UINT                 vs_inst_n = 0, ps_inst_n = 0;

    FrameSurfaces surfaces{};
    PipelineState pipeline{};

    // The views bound (weak: the context holds them while they are bound, and
    // a device context state object while it is swapped out).
    ID3D11RenderTargetView* rtv[8]{};
    UINT                    rtv_count = 0;
    ID3D11DepthStencilView* dsv = nullptr;
};

struct DcState : DcBindings {
    ID3D11Device* device   = nullptr;
    bool          deferred = false;
    uint32_t      gen      = UINT32_MAX;
    ContextSet    contexts;

    // An effect swapped these for the draw being reported; put back after it.
    bool vs_swapped = false;
    bool ps_swapped = false;

    // SwapDeviceContextState (Direct3D 11.1): the state object bound now (null
    // until one is named), and the bindings each state swapped out held, put
    // back when it is swapped in again. `saved` is guarded by the hub.
    ID3DDeviceContextState*                                 context_state = nullptr;   // weak
    std::vector<std::pair<const void*, DcBindings>>         saved;

    void clear() noexcept;   // ClearState, or a command list that did not keep state
};

// The immediate context swapped `outgoing` for `incoming` (hub.cpp). Saves the
// bindings under the state they belong to and puts back what `incoming` held
// when it was last swapped out -- or, for a state never seen, starts from its
// defaults and reads back its shaders.
void context_state_swapped(ID3D11DeviceContext* dc, DcState& st, ID3DDeviceContextState* outgoing,
                           ID3DDeviceContextState* incoming) noexcept;

// The state of a device context, created the first time it is seen. Null when
// the thread is doing internal work or no context is attached to its device.
// Call with a HubReader held.
[[nodiscard]] DcState* dc_state(ID3D11DeviceContext* dc) noexcept;

// ── Registries ───────────────────────────────────────────────────────────────

// Resources: id, kind, device; destruction reported to the device's contexts.
uint64_t resource_register(ID3D11Resource* res, ResourceKind kind, ID3D11Device* device) noexcept;
[[nodiscard]] uint64_t        resource_id(const ID3D11Resource* res) noexcept;
[[nodiscard]] ID3D11Resource* resource_from_id(uint64_t id) noexcept;
// Registers and reports a resource first met somewhere other than its creation
// (made before attaching, or a swap chain buffer). resource.cpp.
void announce_resource(ID3D11Device* device, ID3D11Resource* res) noexcept;
// The id of the resource behind a view, announcing it when new.
[[nodiscard]] uint64_t view_resource_id(ID3D11View* view, ID3D11Device* device) noexcept;

// Any other device child (queries): an id for the object's lifetime.
[[nodiscard]] uint64_t id_of(ID3D11DeviceChild* object) noexcept;

// Shaders: the hash of the bytecode each was created from.
void shader_register(ID3D11DeviceChild* shader, uint64_t hash) noexcept;
[[nodiscard]] uint64_t shader_hash(const void* shader) noexcept;

// ── Frame context ────────────────────────────────────────────────────────────

class D3D11FrameContextImpl final : public vangfx::FrameContext {
public:
    D3D11FrameContextImpl(D3D11ContextImpl& ctx, uint32_t frame_index,
                          ID3D11DeviceContext* dc = nullptr, DcState* state = nullptr) noexcept
        : ctx_(ctx), frame_index_(frame_index), dc_(dc), state_(state) {}

    BackendKind backend()     const noexcept override { return BackendKind::D3D11; }
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
    BufferRef            buffer_at(uint64_t) const noexcept override { return {}; }
    Result<BufferData>   readback_at(uint64_t, uint64_t) override;

    bool                   capture_active() const noexcept override;
    Result<GBufferCapture> latest_capture() override;

    void* native_device()  const noexcept override;
    void* native_context() const noexcept override;
    void* native_resource(RenderTargetHandle h) const noexcept override;
    void* native_resource(DepthBufferHandle h)  const noexcept override;
    void* native_resource(TextureHandle h)      const noexcept override;
    void* native_buffer(uint64_t id)            const noexcept override;

private:
    // Readback and resource work go through the immediate context, which only
    // the thread driving it may use.
    [[nodiscard]] Result<void> on_immediate() const;

    D3D11ContextImpl&    ctx_;
    uint32_t             frame_index_;
    ID3D11DeviceContext* dc_;
    DcState*             state_;
};

// ── Context ──────────────────────────────────────────────────────────────────

class D3D11ContextImpl final : public vangfx::ContextImpl {
public:
    D3D11ContextImpl(const ContextBuilder& b, ID3D11Device* device, ID3D11DeviceContext* immediate,
                     IDXGISwapChain* swap_chain);
    ~D3D11ContextImpl() override;

    BackendKind backend()                const noexcept override { return BackendKind::D3D11; }
    uint32_t    d3d11_feature_level()    const noexcept override { return feature_level_; }
    void*       native_device()          const noexcept override { return device_; }

    Result<Shader> compile_shader(const ShaderBuilder& b) override;
    Result<Effect> link_effect(Shader vs, Shader ps, Shader gs, Shader hs, Shader ds) override;
    Result<Effect> link_compute_effect(Shader cs) override;
    void destroy(ShaderHandle h) noexcept override;
    void destroy(EffectHandle h) noexcept override;

    [[nodiscard]] ID3D11Device*        device()     const noexcept { return device_; }
    [[nodiscard]] ID3D11DeviceContext* immediate()  const noexcept { return immediate_; }
    [[nodiscard]] IDXGISwapChain*      swap_chain() const noexcept { return swap_chain_.load(std::memory_order_acquire); }
    void adopt_swap_chain(IDXGISwapChain* sc) noexcept;
    [[nodiscard]] D3D11ResourceStore& store() noexcept { return store_; }

    [[nodiscard]] uint32_t current_frame_index() const noexcept { return frame_index_.load(std::memory_order_relaxed); }
    uint32_t next_frame() noexcept { return frame_index_.fetch_add(1, std::memory_order_relaxed); }

    struct Timing { float time = 0.0f, delta = 0.0f; bool first = true; };
    Timing tick() noexcept;

    std::string post_pass_profile() const override { return "ps_5_0"; }
    void post_process(IDXGISwapChain* swap_chain, const Timing& t);
    void release_post() noexcept;   // before the swap chain's buffers are resized

    capture::SnapshotHost* snapshot_host() noexcept override;   // capture.cpp

private:
    ID3D11Device*                device_    = nullptr;
    ID3D11DeviceContext*         immediate_ = nullptr;
    std::atomic<IDXGISwapChain*> swap_chain_{ nullptr };   // weak
    uint32_t                     feature_level_ = 0;
    std::atomic<uint32_t>        frame_index_{ 0 };
    D3D11ResourceStore           store_;
    PostProcessor*               post_ = nullptr;
    Snapshots*                   snapshots_ = nullptr;

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
// Each installer patches its classes' vtables through the object given; the
// patches are shared by every context and taken out when the last goes.

bool install_device_hooks(ID3D11Device* device) noexcept;         // resource.cpp
bool install_context_hooks(ID3D11DeviceContext* dc) noexcept;     // draw.cpp + state/rendertarget/query/resource
void install_state_hooks(ID3D11DeviceContext* dc) noexcept;       // state.cpp
void install_rendertarget_hooks(ID3D11DeviceContext* dc) noexcept;// rendertarget.cpp
void install_query_hooks(ID3D11DeviceContext* dc) noexcept;       // query.cpp
void install_map_hooks(ID3D11DeviceContext* dc) noexcept;         // resource.cpp
bool install_present_hooks(IDXGISwapChain* swap_chain) noexcept;  // present.cpp
bool prime_present_hooks(ID3D11Device* device) noexcept;          // a swap chain made for the purpose
void uninstall_all_hooks() noexcept;

// Every device-context site, so heal_context can repair them all. Sites in the
// ID3D11DeviceContext1..4 part of the table are kept apart: a context's table
// has those entries only if it answers for the interface, so each is healed
// only on tables it was installed on.
void register_context_site(hooks::Site& site) noexcept;
void register_ext_context_site(hooks::Site& site) noexcept;
void heal_context(ID3D11DeviceContext* dc) noexcept;
// After a call that rewrites the table: one harmless call into the context,
// which makes the runtime finish a rewrite it does lazily (on the first call
// into a swapped-in state), then heal_context.
void settle_context(ID3D11DeviceContext* dc) noexcept;

// Direct3D 11.1..11.4 on a device context (context1.cpp).
void install_context1_hooks(ID3D11DeviceContext* dc) noexcept;
void uninstall_context1_hooks() noexcept;

// Put first in a device-context thunk: repairs the table when the thunk
// returns, whatever path it returns by.
struct Healer {
    ID3D11DeviceContext* dc;
    ~Healer() { heal_context(dc); }
};

// Hooks whose only job is healing, on the calls a game makes before nearly
// every draw (heal.cpp).
void install_heal_hooks(ID3D11DeviceContext* dc) noexcept;

// VSSetShader / PSSetShader past the hooks, for per-draw swaps (draw.cpp).
void set_vs_unreported(ID3D11DeviceContext* dc, ID3D11VertexShader* vs, ID3D11ClassInstance* const* inst, UINT n) noexcept;
void set_ps_unreported(ID3D11DeviceContext* dc, ID3D11PixelShader* ps, ID3D11ClassInstance* const* inst, UINT n) noexcept;
// Put back what an effect swapped for a draw.
void restore_shaders(ID3D11DeviceContext* dc, DcState& st) noexcept;

// capture.cpp: copies for a capture, and the hooks' reports to it. Nothing
// happens unless a context on the device is capturing.
Snapshots* make_snapshots(D3D11ContextImpl& ctx) noexcept;
void destroy_snapshots(Snapshots* s) noexcept;
void capture_bind(ID3D11DeviceContext* dc, DcState& st);   // after the bindings changed
void capture_draw(ID3D11DeviceContext* dc, DcState& st);

// post.cpp
void destroy_post_processor(PostProcessor* p) noexcept;
void run_post_passes(D3D11ContextImpl& ctx, PostProcessor*& processor, IDXGISwapChain* swap_chain,
                     const D3D11ContextImpl::Timing& t);

}  // namespace vangfx::backend::d3d11
