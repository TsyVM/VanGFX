#pragma once

/**
 * src/backend/d3d12/hub.hpp — what the D3D12 hooks share: the contexts that are
 * attached, the state of every command list, and the registries that turn raw
 * D3D12 pointers into VanGFX ids.
 *
 * HOOKS ARE PER CLASS, CONTEXTS ARE PER DEVICE
 *
 * Every D3D12 device, queue, command list and resource in a process shares its
 * class's vtable (in D3D12Core.dll, the same for a hardware device and WARP), so
 * a hook installed for one context's device also runs for every other device in
 * the process — an overlay's, a tool's WARP device. A thunk therefore starts by
 * asking which contexts, if any, are attached to the device behind the object
 * it was called on, and passes straight through when there are none.
 *
 * LIFETIMES
 *
 * A context cannot go away while a thunk is using it: thunks hold a HubReader
 * (a shared lock) while they dispatch, and hub_detach() takes the lock
 * exclusively. Objects are tracked through compat/lifetime.hpp — the runtime
 * tells us when each one is destroyed — never by hooking Release.
 *
 * INTERNAL WORK
 *
 * VanGFX records command lists and creates resources of its own (readback,
 * post-processing). Those calls run the same hooked methods; InternalScope marks
 * the thread so the thunks pass them through without reporting them.
 */

#include <vangfx/events/draw.hpp>
#include <vangfx/events/resource.hpp>
#include <vangfx/pipeline/state.hpp>
#include <vangfx/resources/surface.hpp>
#include <vangfx/resources/buffer.hpp>
#include <vangfx/d3d12/heap.hpp>
#include <vangfx/d3d12/pso.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

namespace vangfx::backend::d3d12 {

class D3D12ContextImpl;

// ── Contexts ─────────────────────────────────────────────────────────────────

struct ContextSet {
    static constexpr uint32_t kMax = 4;
    D3D12ContextImpl* items[kMax]{};
    uint32_t          count = 0;
    D3D12ContextImpl* const* begin() const noexcept { return items; }
    D3D12ContextImpl* const* end()   const noexcept { return items + count; }
    [[nodiscard]] explicit operator bool() const noexcept { return count != 0; }
};

void hub_attach(D3D12ContextImpl* ctx) noexcept;
void hub_detach(D3D12ContextImpl* ctx) noexcept;   // waits for every dispatch in flight
[[nodiscard]] bool hub_attached(const D3D12ContextImpl* ctx) noexcept;   // call with a HubReader held
[[nodiscard]] bool hub_idle() noexcept;            // no context attached anywhere
[[nodiscard]] uint32_t hub_generation() noexcept;  // moves on every attach/detach

// Held for the length of a dispatch so no context is destroyed under it.
class HubReader {
public:
    HubReader() noexcept;
    ~HubReader();
    HubReader(const HubReader&) = delete;
    HubReader& operator=(const HubReader&) = delete;
};

// The contexts attached to `device`. Call with a HubReader held.
[[nodiscard]] ContextSet contexts_for(const ID3D12Device* device) noexcept;

// The device an object belongs to (weak: the object holds it).
[[nodiscard]] ID3D12Device* device_of(ID3D12DeviceChild* child) noexcept;

// ── Internal work ────────────────────────────────────────────────────────────

class InternalScope {
public:
    InternalScope() noexcept;
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
[[nodiscard]] bool internal() noexcept;

// ── Resource states ──────────────────────────────────────────────────────────
//
// Copying a resource the application owns means transitioning it from the
// state it is in, and D3D12 will not say what that is. VanGFX follows it: the
// initial state at creation, then each barrier. A barrier recorded into a list
// changes nothing until the list runs, so a list keeps the states its barriers
// leave (ListState::states) and ExecuteCommandLists moves them into the table
// below, which is therefore the state as far as the queue has got. Only
// subresource 0 is followed; it is what a capture copies. A resource moved by
// enhanced barriers (ID3D12GraphicsCommandList7::Barrier, or created with a
// layout) has a layout instead.

struct TrackedState {
    uint32_t value  = 0;       // D3D12_RESOURCE_STATES, or D3D12_BARRIER_LAYOUT when `layout`
    bool     layout = false;
    bool     known  = false;
};
void state_set(ID3D12Resource* res, TrackedState s) noexcept;
[[nodiscard]] TrackedState state_of(const ID3D12Resource* res) noexcept;

// ── Command lists ────────────────────────────────────────────────────────────

// A render target or depth buffer a list has bound, and the format its view gave.
struct BoundTarget {
    ID3D12Resource* res    = nullptr;
    DXGI_FORMAT     format = DXGI_FORMAT_UNKNOWN;
};

struct ListState {
    uint64_t                 id     = 0;
    ID3D12Device*            device = nullptr;
    D3D12_COMMAND_LIST_TYPE  type   = D3D12_COMMAND_LIST_TYPE_DIRECT;

    // The contexts listening, refreshed whenever the hub's generation moves.
    uint32_t   gen = UINT32_MAX;
    ContextSet contexts;

    // Pipeline.
    vangfx::d3d12::PSOHandle pso{};
    ID3D12PipelineState*     pso_raw = nullptr;   // as the application set it
    ID3D12PipelineState*     restore_pso = nullptr; // set by an effect for one draw; put back after it
    uint64_t                 vs_hash = 0;
    uint64_t                 ps_hash = 0;
    vangfx::d3d12::RootBinding root_bindings[kMaxRootBindings]{};
    uint32_t                 root_binding_count = 0;

    // Input assembler.
    D3D12VertexBufferBinding vb{};
    D3D12IndexBufferBinding  ib{};

    // Output merger and dynamic state.
    FrameSurfaces surfaces{};
    PipelineState pipeline{};
    BoundTarget   bound[8]{};
    uint32_t      bound_count = 0;
    BoundTarget   bound_depth{};
    bool          in_render_pass = false;
    bool          suspended      = false;   // the last render pass ended suspended: nothing may follow it
    uint32_t      render_pass_flags = 0;    // the open render pass's D3D12_RENDER_PASS_FLAGS

    // The state each resource this recording moved is left in (see TrackedState).
    std::unordered_map<ID3D12Resource*, TrackedState> states;

    vangfx::d3d12::RootBinding& binding(uint32_t index, vangfx::d3d12::RootParameterType kind, bool compute) noexcept;
    void clear_bindings(bool compute) noexcept;
    void reset() noexcept;   // on Reset: a list starts recording with nothing bound
    [[nodiscard]] bool binds(const ID3D12Resource* res) const noexcept {
        if (!res) return false;
        if (bound_depth.res == res) return true;
        for (uint32_t i = 0; i < bound_count; ++i) if (bound[i].res == res) return true;
        return false;
    }
};

// The state of a list, created the first time the list is seen. Null when the
// thread is doing internal work or no context is attached to the list's device.
// Call with a HubReader held; the pointer stays valid until the list is destroyed.
[[nodiscard]] ListState* list_state(ID3D12GraphicsCommandList* list) noexcept;
[[nodiscard]] ID3D12GraphicsCommandList* list_from_id(uint64_t id) noexcept;

// ── Plain ids (queues, fences, heaps, root signatures, query heaps) ──────────

// The id for an object, assigned the first time it is asked for and dropped
// when the object is destroyed.
[[nodiscard]] uint64_t id_of(ID3D12Object* object) noexcept;
[[nodiscard]] uint64_t find_id(const void* object) noexcept;

struct PsoBlueprint;   // effect_bridge.hpp

// ── Pipeline states ──────────────────────────────────────────────────────────

struct PsoRecord {
    uint64_t id      = 0;
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;          // or the compute shader's
    bool     compute = false;
    // The description it was created from, for effects that rebuild it with a
    // different shader (see effect_bridge). Null when not recorded.
    std::shared_ptr<const PsoBlueprint> blueprint;
};
void pso_register(ID3D12PipelineState* pso, PsoRecord rec) noexcept;
[[nodiscard]] PsoRecord pso_find(ID3D12PipelineState* pso) noexcept;
[[nodiscard]] ID3D12PipelineState* pso_from_id(uint64_t id) noexcept;

// ── Resources ────────────────────────────────────────────────────────────────

// Sets the one handle field of a ResourceEvent that its kind names.
inline void set_resource_handle(ResourceEvent& ev, uint64_t id) noexcept {
    switch (ev.kind) {
    case ResourceKind::Texture:          ev.texture         = { id }; break;
    case ResourceKind::RenderTarget:     ev.render_target   = { id }; break;
    case ResourceKind::DepthBuffer:      ev.depth_buffer    = { id }; break;
    case ResourceKind::VertexBuffer:     ev.vertex_buffer   = { id }; break;
    case ResourceKind::IndexBuffer:      ev.index_buffer    = { id }; break;
    case ResourceKind::ConstantBuffer:   ev.constant_buffer = { id }; break;
    case ResourceKind::StructuredBuffer: ev.structured_buf  = { id }; break;
    case ResourceKind::UAV:              ev.uav             = { id }; break;
    case ResourceKind::RawBuffer:        ev.raw_buffer      = { id }; break;
    case ResourceKind::AccelStructure:   ev.accel_structure = { id }; break;
    }
}

struct ResourceRecord {
    uint64_t      id     = 0;
    ResourceKind  kind   = ResourceKind::Texture;
    ID3D12Device* device = nullptr;
};
// Records a resource and reports its destruction to the contexts on `device`.
// Returns the id (the existing one if the resource was already known).
uint64_t resource_register(ID3D12Resource* res, ResourceKind kind, ID3D12Device* device) noexcept;
[[nodiscard]] uint64_t resource_id(const ID3D12Resource* res) noexcept;
[[nodiscard]] ID3D12Resource* resource_from_id(uint64_t id) noexcept;

// ── Views: descriptor → resource ─────────────────────────────────────────────

void view_bind(uint64_t cpu_descriptor, ID3D12Resource* res, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) noexcept;
[[nodiscard]] ID3D12Resource* view_resource(uint64_t cpu_descriptor) noexcept;
// The resource and the format the view reads it as (the resource's own format
// when the view named none).
[[nodiscard]] BoundTarget view_target(uint64_t cpu_descriptor) noexcept;

// ── Swap chains ──────────────────────────────────────────────────────────────

// The queue a swap chain was created on, recorded by the factory hooks
// (swap_chains.cpp). Null for one made before they were installed: a D3D12 swap
// chain will not say which queue it presents on (GetDevice gives the device).
void swap_chain_created(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue) noexcept;
[[nodiscard]] ID3D12CommandQueue* swap_chain_queue(IDXGISwapChain* swap_chain) noexcept;

// ── Descriptor heaps ─────────────────────────────────────────────────────────

struct DescriptorHeapRecord {
    uint64_t                          id        = 0;
    vangfx::d3d12::DescriptorHeapDesc desc{};
    uint64_t                          cpu_base  = 0;
    uint64_t                          gpu_base  = 0;
    uint32_t                          increment = 0;
};
void descriptor_heap_register(ID3D12DescriptorHeap* heap, const DescriptorHeapRecord& rec) noexcept;
// The heap and index a CPU descriptor handle points at ({0,0} if unknown).
[[nodiscard]] std::pair<uint64_t, uint32_t> resolve_descriptor(uint64_t cpu_ptr) noexcept;
[[nodiscard]] uint32_t descriptor_increment(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept;

} // namespace vangfx::backend::d3d12
