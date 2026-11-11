/**
 * src/backend/d3d12/hub.cpp — see hub.hpp.
 */

#include "d3d12_impl.hpp"
#include "effect_bridge.hpp"
#include <compat/lifetime.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d12 {

namespace {

// ── Contexts ─────────────────────────────────────────────────────────────────

SRWLOCK                         g_hub = SRWLOCK_INIT;
std::vector<D3D12ContextImpl*>  g_contexts;        // written under g_hub exclusive
std::atomic<int>                g_context_count{ 0 };
std::atomic<uint32_t>           g_generation{ 1 };
thread_local int                t_reader_depth = 0;
thread_local int                t_internal     = 0;

// Every registry below has a listener tag; the lifetime service calls back with
// it. Each registry also listens under its own key: the service keeps one
// registration per (object, key), and a descriptor heap is in two registries
// (its id and its CPU range), so one shared key would let the second replace
// the first and leave the first stale when the heap goes.
enum : uint64_t { kListTag = 1, kIdTag, kPsoTag, kResourceTag, kHeapTag, kSwapTag, kTagCount };
char g_listener_keys[kTagCount];

// ── Command lists ────────────────────────────────────────────────────────────

std::shared_mutex                                                   g_lists_mutex;
std::unordered_map<const void*, std::unique_ptr<ListState>>         g_lists;
std::unordered_map<uint64_t, ID3D12GraphicsCommandList*>            g_list_ids;
std::atomic<uint32_t>                                               g_list_epoch{ 1 };

struct ListCache { const void* list = nullptr; ListState* state = nullptr; uint32_t epoch = 0; };
thread_local ListCache t_list_cache;

// ── Plain ids ────────────────────────────────────────────────────────────────

std::shared_mutex                              g_ids_mutex;
std::unordered_map<const void*, uint64_t>      g_ids;

// ── Pipeline states ──────────────────────────────────────────────────────────

std::shared_mutex                                       g_pso_mutex;
std::unordered_map<const void*, PsoRecord>              g_psos;
std::unordered_map<uint64_t, ID3D12PipelineState*>      g_pso_ids;

// ── Resources and views ──────────────────────────────────────────────────────

std::shared_mutex                                       g_res_mutex;
std::unordered_map<const void*, ResourceRecord>         g_resources;
std::unordered_map<uint64_t, ID3D12Resource*>           g_resource_ids;
std::unordered_map<uint64_t, BoundTarget>               g_views;       // cpu descriptor → resource, view format
std::unordered_map<const void*, std::vector<uint64_t>>  g_views_of;    // resource → its descriptors
std::unordered_map<const ID3D12Resource*, TrackedState> g_states;      // under g_res_mutex too

// ── Descriptor heaps ─────────────────────────────────────────────────────────

struct HeapSpan { uint64_t end = 0; uint64_t id = 0; uint32_t increment = 0; };
std::shared_mutex                                       g_heap_mutex;
std::unordered_map<const void*, DescriptorHeapRecord>   g_heaps;
std::map<uint64_t, HeapSpan>                            g_heap_spans;  // cpu_base → span

// ── Swap chains → the queue each presents on ─────────────────────────────────
// Kept across contexts coming and going: a swap chain made while one context
// was attached is presented to the next.

std::mutex                                             g_sc_mutex;
std::unordered_map<const void*, ID3D12CommandQueue*>   g_sc_queues;   // weak: the swap chain holds its queue

// ── Destruction ──────────────────────────────────────────────────────────────

void report_resource_destroyed(const ResourceRecord& rec) noexcept {
    if (hub_idle()) return;
    HubReader r;
    for (D3D12ContextImpl* ctx : contexts_for(rec.device)) {
        // Out of the store first: it holds a borrowed pointer to the resource,
        // and a callback that reads back this handle must be told it is gone
        // rather than be given freed memory.
        ctx->store().forget(rec.id);
        if (!ctx->wants_resource()) continue;
        ResourceEvent ev{};
        ev.kind   = rec.kind;
        ev.action = ResourceAction::Destroy;
        set_resource_handle(ev, rec.id);
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_resource(fc, ev);
    }
}

void on_destroyed(void*, const void* object, uint64_t tag) noexcept {
    switch (tag) {
    case kListTag: {
        std::unique_lock lk(g_lists_mutex);
        auto it = g_lists.find(object);
        if (it != g_lists.end()) {
            g_list_ids.erase(it->second->id);
            g_lists.erase(it);
            g_list_epoch.fetch_add(1, std::memory_order_acq_rel);
        }
        break;
    }
    case kIdTag: {
        std::unique_lock lk(g_ids_mutex);
        g_ids.erase(object);
        break;
    }
    case kPsoTag: {
        PsoRecord gone{};
        {
            std::unique_lock lk(g_pso_mutex);
            auto it = g_psos.find(object);
            if (it != g_psos.end()) {
                gone = std::move(it->second);
                g_pso_ids.erase(gone.id);
                g_psos.erase(it);
            }
        }
        if (gone.id) effect_bridge::pipeline_destroyed(gone.id);
        break;   // `gone` releases the blueprint (and its root signature) here, unlocked
    }
    case kResourceTag: {
        ResourceRecord rec{};
        bool found = false;
        {
            std::unique_lock lk(g_res_mutex);
            auto it = g_resources.find(object);
            if (it != g_resources.end()) {
                rec = it->second;
                found = true;
                g_resource_ids.erase(rec.id);
                g_resources.erase(it);
            }
            g_states.erase(static_cast<const ID3D12Resource*>(object));
            if (auto v = g_views_of.find(object); v != g_views_of.end()) {
                for (uint64_t cpu : v->second) {
                    auto w = g_views.find(cpu);
                    if (w != g_views.end() && w->second.res == object) g_views.erase(w);
                }
                g_views_of.erase(v);
            }
        }
        if (found) report_resource_destroyed(rec);
        break;
    }
    case kSwapTag: {
        std::lock_guard lk(g_sc_mutex);
        g_sc_queues.erase(object);
        break;
    }
    case kHeapTag: {
        std::unique_lock lk(g_heap_mutex);
        auto it = g_heaps.find(object);
        if (it != g_heaps.end()) {
            g_heap_spans.erase(it->second.cpu_base);
            g_heaps.erase(it);
        }
        break;
    }
    default: break;
    }
}

bool watch(ID3D12Object* object, uint64_t tag) noexcept {
    return lifetime::watch(object, &on_destroyed, &g_listener_keys[tag], tag);
}

// The last context has gone: stop tracking anything, so no sentinel of ours is
// left on an object that may outlive this module, and a context attached later
// starts from registries that hold no pointer to a freed object.
void reset_registries() noexcept {
    for (uint64_t tag = 0; tag < kTagCount; ++tag)
        if (tag != kSwapTag) lifetime::forget(&g_listener_keys[tag]);
    {
        std::unique_lock lk(g_lists_mutex);
        g_lists.clear();
        g_list_ids.clear();
        g_list_epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    { std::unique_lock lk(g_ids_mutex); g_ids.clear(); }
    {
        std::unordered_map<const void*, PsoRecord> psos;
        {
            std::unique_lock lk(g_pso_mutex);
            psos.swap(g_psos);
            g_pso_ids.clear();
        }
    }
    {
        std::unique_lock lk(g_res_mutex);
        g_resources.clear();
        g_resource_ids.clear();
        g_views.clear();
        g_views_of.clear();
        g_states.clear();
    }
    {
        std::unique_lock lk(g_heap_mutex);
        g_heaps.clear();
        g_heap_spans.clear();
    }
}

} // namespace

// ── Contexts ─────────────────────────────────────────────────────────────────

void hub_attach(D3D12ContextImpl* ctx) noexcept {
    AcquireSRWLockExclusive(&g_hub);
    g_contexts.push_back(ctx);
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_hub);
}

void hub_detach(D3D12ContextImpl* ctx) noexcept {
    // Destroying a context from inside one of its own callbacks would wait here
    // for the dispatch it is running in. That is documented as not allowed; the
    // context is still taken out of every dispatch from now on, just without the wait.
    const bool nested = t_reader_depth > 0;
    if (nested) ReleaseSRWLockShared(&g_hub);
    AcquireSRWLockExclusive(&g_hub);
    for (size_t i = 0; i < g_contexts.size(); ++i)
        if (g_contexts[i] == ctx) { g_contexts.erase(g_contexts.begin() + static_cast<ptrdiff_t>(i)); break; }
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    // With no reader inside a dispatch nothing holds a ListState, so the
    // registries can go. (Nested, the dispatch this runs in still does.)
    if (g_contexts.empty() && !nested) reset_registries();
    ReleaseSRWLockExclusive(&g_hub);
    if (nested) AcquireSRWLockShared(&g_hub);
}

bool hub_attached(const D3D12ContextImpl* ctx) noexcept {
    for (D3D12ContextImpl* c : g_contexts) if (c == ctx) return true;
    return false;
}

bool hub_idle() noexcept { return g_context_count.load(std::memory_order_acquire) == 0; }
uint32_t hub_generation() noexcept { return g_generation.load(std::memory_order_acquire); }

HubReader::HubReader() noexcept  { if (t_reader_depth++ == 0) AcquireSRWLockShared(&g_hub); }
HubReader::~HubReader()          { if (--t_reader_depth == 0) ReleaseSRWLockShared(&g_hub); }

ContextSet contexts_for(const ID3D12Device* device) noexcept {
    ContextSet set;
    if (!device) return set;
    for (D3D12ContextImpl* c : g_contexts)
        if (c->device() == device && set.count < ContextSet::kMax) set.items[set.count++] = c;
    return set;
}

ID3D12Device* device_of(ID3D12DeviceChild* child) noexcept {
    if (!child) return nullptr;
    ID3D12Device* dev = nullptr;
    if (FAILED(child->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    dev->Release();   // the child keeps it alive
    return dev;
}

// ── Internal work ────────────────────────────────────────────────────────────

InternalScope::InternalScope() noexcept { ++t_internal; }
InternalScope::~InternalScope()         { --t_internal; }
bool internal() noexcept { return t_internal > 0; }

// ── Command lists ────────────────────────────────────────────────────────────

vangfx::d3d12::RootBinding& ListState::binding(uint32_t index, vangfx::d3d12::RootParameterType kind, bool compute) noexcept {
    for (uint32_t i = 0; i < root_binding_count; ++i) {
        auto& b = root_bindings[i];
        if (b.parameter_index == index && b.is_compute == compute) {
            b.type = kind;
            return b;
        }
    }
    if (root_binding_count < kMaxRootBindings) {
        auto& b = root_bindings[root_binding_count++];
        b = {};
        b.parameter_index = index;
        b.type            = kind;
        b.is_compute      = compute;
        return b;
    }
    // Out of room (a root signature has at most 64 parameters per pipeline, so
    // this needs both pipelines full): reuse the last entry.
    auto& b = root_bindings[kMaxRootBindings - 1];
    b = {};
    b.parameter_index = index; b.type = kind; b.is_compute = compute;
    return b;
}

void ListState::clear_bindings(bool compute) noexcept {
    uint32_t n = 0;
    for (uint32_t i = 0; i < root_binding_count; ++i)
        if (root_bindings[i].is_compute != compute) {
            if (n != i) root_bindings[n] = root_bindings[i];
            ++n;
        }
    root_binding_count = n;
}

void ListState::reset() noexcept {
    pso = {};
    pso_raw = nullptr;
    restore_pso = nullptr;
    vs_hash = ps_hash = 0;
    root_binding_count = 0;
    vb = {};
    ib = {};
    surfaces = {};
    pipeline = {};
    for (auto& b : bound) b = {};
    bound_count = 0;
    bound_depth = {};
    in_render_pass = suspended = false;
    render_pass_flags = 0;
    states.clear();
}

ListState* list_state(ID3D12GraphicsCommandList* list) noexcept {
    if (!list || t_internal > 0) return nullptr;
    const uint32_t epoch = g_list_epoch.load(std::memory_order_acquire);

    ListState* st = nullptr;
    if (t_list_cache.list == list && t_list_cache.epoch == epoch) {
        st = t_list_cache.state;
    } else {
        {
            std::shared_lock lk(g_lists_mutex);
            auto it = g_lists.find(list);
            if (it != g_lists.end()) st = it->second.get();
        }
        if (!st) {
            auto fresh = std::make_unique<ListState>();
            fresh->device = device_of(list);
            fresh->type   = list->GetType();
            fresh->id     = ContextImpl::alloc_id();
            std::unique_lock lk(g_lists_mutex);
            auto [it, inserted] = g_lists.try_emplace(list, std::move(fresh));
            st = it->second.get();
            if (inserted) {
                g_list_ids[st->id] = list;
                watch(list, kListTag);
            }
        }
        t_list_cache = { list, st, epoch };
    }

    const uint32_t gen = hub_generation();
    if (st->gen != gen) {
        st->contexts = contexts_for(st->device);
        st->gen      = gen;
    }
    return st->contexts ? st : nullptr;
}

ID3D12GraphicsCommandList* list_from_id(uint64_t id) noexcept {
    std::shared_lock lk(g_lists_mutex);
    auto it = g_list_ids.find(id);
    return it != g_list_ids.end() ? it->second : nullptr;
}

// ── Plain ids ────────────────────────────────────────────────────────────────

uint64_t id_of(ID3D12Object* object) noexcept {
    if (!object) return 0;
    {
        std::shared_lock lk(g_ids_mutex);
        auto it = g_ids.find(object);
        if (it != g_ids.end()) return it->second;
    }
    const uint64_t id = ContextImpl::alloc_id();
    std::unique_lock lk(g_ids_mutex);
    auto [it, inserted] = g_ids.try_emplace(object, id);
    if (inserted) watch(object, kIdTag);
    return it->second;
}

uint64_t find_id(const void* object) noexcept {
    std::shared_lock lk(g_ids_mutex);
    auto it = g_ids.find(object);
    return it != g_ids.end() ? it->second : 0;
}

// ── Pipeline states ──────────────────────────────────────────────────────────

void pso_register(ID3D12PipelineState* pso, PsoRecord rec) noexcept {
    if (!pso) return;
    std::unique_lock lk(g_pso_mutex);
    g_pso_ids[rec.id] = pso;
    auto [it, inserted] = g_psos.insert_or_assign(pso, std::move(rec));
    if (inserted) watch(pso, kPsoTag);
}

PsoRecord pso_find(ID3D12PipelineState* pso) noexcept {
    if (!pso) return {};
    std::shared_lock lk(g_pso_mutex);
    auto it = g_psos.find(pso);
    return it != g_psos.end() ? it->second : PsoRecord{};
}

ID3D12PipelineState* pso_from_id(uint64_t id) noexcept {
    std::shared_lock lk(g_pso_mutex);
    auto it = g_pso_ids.find(id);
    return it != g_pso_ids.end() ? it->second : nullptr;
}

// ── Resources ────────────────────────────────────────────────────────────────

uint64_t resource_register(ID3D12Resource* res, ResourceKind kind, ID3D12Device* device) noexcept {
    if (!res) return 0;
    {
        std::shared_lock lk(g_res_mutex);
        auto it = g_resources.find(res);
        if (it != g_resources.end()) return it->second.id;
    }
    ResourceRecord rec{ ContextImpl::alloc_id(), kind, device };
    std::unique_lock lk(g_res_mutex);
    auto [it, inserted] = g_resources.try_emplace(res, rec);
    if (inserted) {
        g_resource_ids[rec.id] = res;
        watch(res, kResourceTag);
    }
    return it->second.id;
}

uint64_t resource_id(const ID3D12Resource* res) noexcept {
    if (!res) return 0;
    std::shared_lock lk(g_res_mutex);
    auto it = g_resources.find(res);
    return it != g_resources.end() ? it->second.id : 0;
}

ID3D12Resource* resource_from_id(uint64_t id) noexcept {
    std::shared_lock lk(g_res_mutex);
    auto it = g_resource_ids.find(id);
    return it != g_resource_ids.end() ? it->second : nullptr;
}

// ── Views ────────────────────────────────────────────────────────────────────

void view_bind(uint64_t cpu, ID3D12Resource* res, DXGI_FORMAT format) noexcept {
    if (!cpu) return;
    std::unique_lock lk(g_res_mutex);
    if (auto old = g_views.find(cpu); old != g_views.end() && old->second.res != res) {
        auto& list = g_views_of[old->second.res];
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == cpu) { list[i] = list.back(); list.pop_back(); break; }
    }
    if (!res) { g_views.erase(cpu); return; }
    auto [it, inserted] = g_views.insert_or_assign(cpu, BoundTarget{ res, format });
    if (inserted) g_views_of[res].push_back(cpu);
    else {
        auto& list = g_views_of[res];
        if (std::find(list.begin(), list.end(), cpu) == list.end()) list.push_back(cpu);
    }
}

ID3D12Resource* view_resource(uint64_t cpu) noexcept {
    if (!cpu) return nullptr;
    std::shared_lock lk(g_res_mutex);
    auto it = g_views.find(cpu);
    return it != g_views.end() ? it->second.res : nullptr;
}

BoundTarget view_target(uint64_t cpu) noexcept {
    if (!cpu) return {};
    std::shared_lock lk(g_res_mutex);
    auto it = g_views.find(cpu);
    return it != g_views.end() ? it->second : BoundTarget{};
}

// ── Resource states ──────────────────────────────────────────────────────────

void state_set(ID3D12Resource* res, TrackedState s) noexcept {
    if (!res) return;
    std::unique_lock lk(g_res_mutex);
    g_states[res] = s;
}

TrackedState state_of(const ID3D12Resource* res) noexcept {
    if (!res) return {};
    std::shared_lock lk(g_res_mutex);
    auto it = g_states.find(res);
    return it != g_states.end() ? it->second : TrackedState{};
}

// ── Swap chains ──────────────────────────────────────────────────────────────

void swap_chain_created(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue) noexcept {
    if (!swap_chain || !queue) return;
    {
        std::lock_guard lk(g_sc_mutex);
        g_sc_queues[swap_chain] = queue;
    }
    lifetime::watch(swap_chain, &on_destroyed, &g_listener_keys[kSwapTag], kSwapTag);
}

ID3D12CommandQueue* swap_chain_queue(IDXGISwapChain* swap_chain) noexcept {
    std::lock_guard lk(g_sc_mutex);
    auto it = g_sc_queues.find(swap_chain);
    return it != g_sc_queues.end() ? it->second : nullptr;
}

// ── Descriptor heaps ─────────────────────────────────────────────────────────

void descriptor_heap_register(ID3D12DescriptorHeap* heap, const DescriptorHeapRecord& rec) noexcept {
    if (!heap) return;
    std::unique_lock lk(g_heap_mutex);
    auto [it, inserted] = g_heaps.insert_or_assign(heap, rec);
    if (rec.cpu_base && rec.increment)
        g_heap_spans[rec.cpu_base] = HeapSpan{ rec.cpu_base + uint64_t(rec.desc.num_descriptors) * rec.increment,
                                               rec.id, rec.increment };
    if (inserted) watch(heap, kHeapTag);
}

std::pair<uint64_t, uint32_t> resolve_descriptor(uint64_t cpu) noexcept {
    std::shared_lock lk(g_heap_mutex);
    auto it = g_heap_spans.upper_bound(cpu);
    if (it == g_heap_spans.begin()) return {};
    --it;
    const HeapSpan& s = it->second;
    if (cpu >= s.end || !s.increment || (cpu - it->first) % s.increment) return {};
    return { s.id, static_cast<uint32_t>((cpu - it->first) / s.increment) };
}

uint32_t descriptor_increment(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept {
    static std::atomic<ID3D12Device*> cached_device{ nullptr };
    static std::atomic<uint32_t>      cached[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES]{};
    if (!device || type < 0 || type >= D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES) return 0;
    if (cached_device.load(std::memory_order_acquire) == device) {
        if (const uint32_t v = cached[type].load(std::memory_order_relaxed)) return v;
    }
    const uint32_t v = device->GetDescriptorHandleIncrementSize(type);
    if (cached_device.exchange(device, std::memory_order_acq_rel) != device)
        for (auto& c : cached) c.store(0, std::memory_order_relaxed);
    cached[type].store(v, std::memory_order_relaxed);
    return v;
}

} // namespace vangfx::backend::d3d12
