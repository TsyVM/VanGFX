#pragma once

/**
 * src/backend/d3d12/d3d12_impl.hpp — D3D12ContextImpl and D3D12FrameContextImpl.
 *
 * NOT part of the public SDK.
 */

#include "../../core/context_impl.hpp"
#include "hub.hpp"
#include "store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d12 {

class D3D12ContextImpl;
class PostProcessor;   // post.cpp
class Snapshots;       // capture.cpp

// ─────────────────────────────────────────────────────────────────────────────
//  D3D12FrameContextImpl — the FrameContext handed to every callback
//
//  Built on the stack in each thunk. `list` and `state` are set for events that
//  come from a command list (draws, binds, barriers, queries) and null for the
//  rest (queue, present, creation); surfaces() and pipeline() report the list's
//  own state when there is one — D3D12 records lists on many threads at once,
//  so "the current render target" only means something per list.
// ─────────────────────────────────────────────────────────────────────────────

class D3D12FrameContextImpl final : public vangfx::FrameContext {
public:
    D3D12FrameContextImpl(D3D12ContextImpl& ctx, uint32_t frame_index,
                          IDXGISwapChain* swap_chain = nullptr,
                          ID3D12GraphicsCommandList* list = nullptr,
                          ListState* state = nullptr) noexcept
        : ctx_(ctx), frame_index_(frame_index), swap_chain_(swap_chain), list_(list), state_(state) {}

    BackendKind backend()     const noexcept override { return BackendKind::D3D12; }
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

    Result<BufferData> readback(VertexBufferHandle h,     uint64_t, uint64_t) override;
    Result<BufferData> readback(IndexBufferHandle h,      uint64_t, uint64_t) override;
    Result<BufferData> readback(ConstantBufferHandle h,   uint64_t, uint64_t) override;
    Result<BufferData> readback(StructuredBufferHandle h, uint64_t, uint64_t) override;
    Result<BufferData> readback(RawBufferHandle h,        uint64_t, uint64_t) override;
    Result<BufferData> readback(UAVHandle h,              uint64_t, uint64_t) override;

    BufferRef          buffer_at(uint64_t gpu_virtual_address) const noexcept override;
    Result<BufferData> readback_at(uint64_t gpu_virtual_address, uint64_t size) override;

    bool                   capture_active() const noexcept override;
    Result<GBufferCapture> latest_capture() override;

    void* native_device()  const noexcept override;
    void* native_context() const noexcept override { return list_; }
    void* native_resource(RenderTargetHandle h) const noexcept override;
    void* native_resource(DepthBufferHandle h)  const noexcept override;
    void* native_resource(TextureHandle h)      const noexcept override;
    void* native_buffer(uint64_t id)            const noexcept override;

    [[nodiscard]] ID3D12GraphicsCommandList* list() const noexcept { return list_; }
    [[nodiscard]] ListState*                 list_state() const noexcept { return state_; }
    [[nodiscard]] IDXGISwapChain*            swap_chain() const noexcept { return swap_chain_; }

private:
    D3D12ContextImpl&          ctx_;
    uint32_t                   frame_index_;
    IDXGISwapChain*            swap_chain_;
    ID3D12GraphicsCommandList* list_;
    ListState*                 state_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  D3D12ContextImpl
// ─────────────────────────────────────────────────────────────────────────────

class D3D12ContextImpl final : public vangfx::ContextImpl {
public:
    // `swap_chain` may be null: Present is still hooked (through a swap chain
    // VanGFX makes for the purpose, since every swap chain shares one vtable),
    // and the application's swap chain is recognised by the queue it presents on.
    D3D12ContextImpl(const vangfx::ContextBuilder& b, ID3D12Device* device,
                     ID3D12CommandQueue* queue, IDXGISwapChain* swap_chain);
    ~D3D12ContextImpl() override;

    // ── Context ───────────────────────────────────────────────────────────────
    Result<Shader> compile_shader(const ShaderBuilder& b) override;
    Result<Effect> link_effect(Shader vs, Shader ps, Shader gs, Shader hs, Shader ds) override;
    Result<Effect> link_compute_effect(Shader cs) override;
    void destroy(ShaderHandle h) noexcept override;
    void destroy(EffectHandle h) noexcept override;

    BackendKind backend()               const noexcept override { return BackendKind::D3D12; }
    uint32_t    d3d9_ver()              const noexcept override { return 0; }
    uint32_t    d3d11_feature_level()   const noexcept override { return 0; }
    uint32_t    d3d12_shader_model()    const noexcept override { return shader_model_; }
    uint32_t    d3d12_raytracing_tier() const noexcept override { return raytracing_tier_; }
    uint32_t    d3d12_mesh_shader_tier()const noexcept override { return mesh_shader_tier_; }
    void*       native_device()         const noexcept override { return device_; }

    // ── For the thunks ────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<uint8_t> shader_bytecode(ShaderHandle h) const;

    // The shader as DXIL (`dxil`) or DXBC. A pipeline cannot mix the two, so a
    // replacement has to match the game's own shaders; the other form is
    // compiled from the shader's source the first time it is asked for, and
    // kept. Empty with `why` set when that is not possible.
    [[nodiscard]] std::vector<uint8_t> shader_bytecode_as(ShaderHandle h, bool dxil, std::string& why);

    [[nodiscard]] D3D12ResourceStore&  store() noexcept { return store_; }
    [[nodiscard]] ID3D12Device*        device()     const noexcept { return device_; }
    [[nodiscard]] ID3D12CommandQueue*  queue()      const noexcept { return queue_; }
    [[nodiscard]] IDXGISwapChain*      swap_chain() const noexcept { return swap_chain_.load(std::memory_order_acquire); }

    // The application's swap chain, learned from the first Present on this
    // context's queue when the builder was not given one.
    void adopt_swap_chain(IDXGISwapChain* sc) noexcept;

    // Whether pipelines created now should be recorded for the effect bridge:
    // true once an Effect has been linked on this context.
    [[nodiscard]] bool records_pipelines() const noexcept { return record_pipelines_.load(std::memory_order_acquire); }

    [[nodiscard]] uint32_t current_frame_index() const noexcept { return frame_index_.load(std::memory_order_relaxed); }
    uint32_t next_frame() noexcept { return frame_index_.fetch_add(1, std::memory_order_relaxed); }

    // Surfaces as of the last render-target bind on any list: what a present or
    // queue event reports, having no list of its own.
    void          note_surfaces(const FrameSurfaces& s) noexcept;
    FrameSurfaces last_surfaces() const noexcept;

    // Frame timing for the uniforms (vangfx_time / vangfx_delta).
    struct Timing { float time = 0.0f, delta = 0.0f; bool first = true; };
    Timing tick() noexcept;

    // Post-processing passes, run from the present hook before Present.
    std::string post_pass_profile() const override;
    void post_process(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue, const Timing& t);

    // Capture copies (capture.cpp).
    capture::SnapshotHost* snapshot_host() noexcept override;
    [[nodiscard]] Snapshots* snapshots() noexcept { return snapshots_; }

private:
    ID3D12Device*                device_ = nullptr;
    ID3D12CommandQueue*          queue_  = nullptr;
    std::atomic<IDXGISwapChain*> swap_chain_{ nullptr };   // weak: compared, never called through
    std::atomic<bool>            record_pipelines_{ false };
    D3D12ResourceStore           store_;
    PostProcessor*               post_ = nullptr;   // made on the first frame with a pass
    Snapshots*                   snapshots_ = nullptr;
    uint32_t                     shader_model_     = 0;
    uint32_t                     raytracing_tier_  = 0;
    uint32_t                     mesh_shader_tier_ = 0;
    std::atomic<uint32_t>        frame_index_{ 0 };

    struct ShaderRecord {
        ShaderStage          stage{};
        ShaderLang           lang = ShaderLang::DXIL;
        std::string          entry;
        std::string          profile;
        std::vector<uint8_t> bytecode;
        ShaderBuilder        source;          // the builder it came from, for recompiling
        std::vector<uint8_t> other;           // the other container, once compiled
        bool                 other_failed = false;
    };
    mutable std::shared_mutex                  shaders_mutex_;
    std::unordered_map<uint64_t, ShaderRecord> shaders_;
    mutable std::shared_mutex                  effects_mutex_;
    std::unordered_map<uint64_t, Effect>       effects_;

    mutable std::mutex surfaces_mutex_;
    FrameSurfaces      last_surfaces_{};

    using Clock = std::chrono::steady_clock;
    std::mutex        timing_mutex_;
    Clock::time_point start_ = Clock::now();
    Clock::time_point last_  = Clock::now();
    bool              first_frame_ = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Hook installation — one install/uninstall pair per module
//
//  Installing patches the class vtables through the context's objects; the
//  patches are shared by every context and are taken out when the last D3D12
//  context goes (uninstall_*), so these are reference-counted in context.cpp.
// ─────────────────────────────────────────────────────────────────────────────

bool install_device_hooks(ID3D12Device* device) noexcept;           // pso.cpp, resource.cpp, descriptor_heap.cpp, query.cpp, command.cpp
bool install_queue_hooks(ID3D12CommandQueue* queue) noexcept;       // command.cpp
bool install_list_hooks(ID3D12GraphicsCommandList* list) noexcept;  // command.cpp + draw/ia/barrier/rendertarget/state/query
bool install_present_hooks(IDXGISwapChain* swap_chain) noexcept;    // present.cpp
bool install_factory_hooks() noexcept;                             // swap_chains.cpp
void uninstall_all_hooks() noexcept;

// Per-module pieces the installers call.
void install_pso_hooks(ID3D12Device*) noexcept;           void uninstall_pso_hooks() noexcept;
void install_resource_hooks(ID3D12Device*) noexcept;      void uninstall_resource_hooks() noexcept;
void install_descriptor_hooks(ID3D12Device*) noexcept;    void uninstall_descriptor_hooks() noexcept;
void uninstall_query_hooks() noexcept;
void install_draw_hooks(ID3D12GraphicsCommandList*) noexcept;         void uninstall_draw_hooks() noexcept;
void install_ia_hooks(ID3D12GraphicsCommandList*) noexcept;           void uninstall_ia_hooks() noexcept;
void install_barrier_hooks(ID3D12GraphicsCommandList*) noexcept;      void uninstall_barrier_hooks() noexcept;
void install_rendertarget_hooks(ID3D12GraphicsCommandList*) noexcept; void uninstall_rendertarget_hooks() noexcept;
void install_state_hooks(ID3D12GraphicsCommandList*) noexcept;        void uninstall_state_hooks() noexcept;
void install_query_list_hooks(ID3D12GraphicsCommandList*) noexcept;
void uninstall_present_hooks() noexcept;
void uninstall_factory_hooks() noexcept;
void uninstall_command_hooks() noexcept;

// SetPipelineState without the hook seeing it (draw.cpp), for effects that swap
// a pipeline for one draw.
void set_pipeline_unreported(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) noexcept;

// Registers and reports (as a Create event) a resource VanGFX first meets
// somewhere other than its creation: through a view, when it was made before
// VanGFX attached or by DXGI (resource.cpp).
void announce_resource(ID3D12Device* device, ID3D12Resource* res) noexcept;

// Whether shader bytecode is DXIL (SM 6) rather than DXBC (context.cpp).
[[nodiscard]] bool bytecode_is_dxil(const void* data, size_t size) noexcept;

// capture.cpp -- the hooks' side of a capture, and the copies it makes. Each
// hook passes its list's state; nothing happens unless a context on the list's
// device is capturing.
Snapshots* make_snapshots(D3D12ContextImpl& ctx) noexcept;
void destroy_snapshots(Snapshots* s) noexcept;   // waits (bounded) for copies still on the GPU
void snapshots_present(Snapshots* s, ID3D12CommandQueue* present_queue) noexcept;   // every frame: frees finished copies
void capture_bind(ID3D12GraphicsCommandList* list, ListState& st);
void capture_draw(ID3D12GraphicsCommandList* list, ListState& st);
void capture_pass_break(ID3D12GraphicsCommandList* list, ListState& st);
void capture_close(ID3D12GraphicsCommandList* list, ListState& st);
void capture_reset(ID3D12GraphicsCommandList* list, ListState& st);
void capture_executed(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ListState& st);
[[nodiscard]] bool capturing(const ListState& st) noexcept;

// post.cpp
void destroy_post_processor(PostProcessor* p) noexcept;
void run_post_passes(D3D12ContextImpl& ctx, PostProcessor*& processor, IDXGISwapChain* swap_chain,
                     ID3D12CommandQueue* queue, const D3D12ContextImpl::Timing& timing);

// Makes a command list of each type VanGFX hooks (direct and bundle have
// different vtables) and installs the list hooks through it, so lists the
// application created before VanGFX attached are covered too.
bool prime_list_hooks(ID3D12Device* device) noexcept;
// Makes a throwaway swap chain on `queue` so Present is hooked for every swap
// chain in the process, the application's included.
bool prime_present_hooks(ID3D12CommandQueue* queue) noexcept;

Result<std::unique_ptr<vangfx::Context>> make_d3d12_context(const vangfx::ContextBuilder& b);

} // namespace vangfx::backend::d3d12
