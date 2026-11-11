/**
 * src/backend/d3d11/hub.cpp — see d3d11_impl.hpp.
 */

#include "d3d11_impl.hpp"
#include <compat/lifetime.hpp>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d11 {

namespace {

SRWLOCK                         g_hub = SRWLOCK_INIT;
std::vector<D3D11ContextImpl*>  g_contexts;
std::atomic<int>                g_context_count{ 0 };
std::atomic<uint32_t>           g_generation{ 1 };
thread_local int                t_reader_depth = 0;
thread_local int                t_internal     = 0;

// One lifetime key per registry (see the D3D12 hub for why).
enum : uint64_t { kDcTag = 1, kResourceTag, kIdTag, kShaderTag, kStateTag, kTagCount };
char g_keys[kTagCount];

// DcState::saved: written by the immediate context's thread at a swap, and by
// whichever thread releases a device context state object for the last time.
std::mutex g_saved_mutex;

// ── Device contexts ──────────────────────────────────────────────────────────

std::shared_mutex                                         g_dc_mutex;
std::unordered_map<const void*, std::unique_ptr<DcState>> g_dcs;
std::atomic<uint32_t>                                     g_dc_epoch{ 1 };
struct DcCache { const void* dc = nullptr; DcState* state = nullptr; uint32_t epoch = 0; };
thread_local DcCache t_dc_cache;

// ── Resources, plain ids, shaders ────────────────────────────────────────────

struct ResourceRecord { uint64_t id = 0; ResourceKind kind = ResourceKind::Texture; ID3D11Device* device = nullptr; };
std::shared_mutex                                   g_res_mutex;
std::unordered_map<const void*, ResourceRecord>     g_resources;
std::unordered_map<uint64_t, ID3D11Resource*>       g_resource_ids;

std::shared_mutex                                   g_ids_mutex;
std::unordered_map<const void*, uint64_t>           g_ids;

std::shared_mutex                                   g_shader_mutex;
std::unordered_map<const void*, uint64_t>           g_shaders;

void report_destroyed(const ResourceRecord& rec) noexcept {
    if (hub_idle()) return;
    HubReader r;
    for (D3D11ContextImpl* ctx : contexts_for(rec.device)) {
        ctx->store().forget(rec.id);   // before any callback can ask to read it
        if (!ctx->wants_resource()) continue;
        ResourceEvent ev{};
        ev.kind   = rec.kind;
        ev.action = ResourceAction::Destroy;
        switch (rec.kind) {
        case ResourceKind::Texture:          ev.texture         = { rec.id }; break;
        case ResourceKind::RenderTarget:     ev.render_target   = { rec.id }; break;
        case ResourceKind::DepthBuffer:      ev.depth_buffer    = { rec.id }; break;
        case ResourceKind::VertexBuffer:     ev.vertex_buffer   = { rec.id }; break;
        case ResourceKind::IndexBuffer:      ev.index_buffer    = { rec.id }; break;
        case ResourceKind::ConstantBuffer:   ev.constant_buffer = { rec.id }; break;
        case ResourceKind::StructuredBuffer: ev.structured_buf  = { rec.id }; break;
        case ResourceKind::UAV:              ev.uav             = { rec.id }; break;
        default: break;
        }
        D3D11FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_resource(fc, ev);
    }
}

void on_destroyed(void*, const void* object, uint64_t tag) noexcept {
    switch (tag) {
    case kDcTag: {
        std::unique_lock lk(g_dc_mutex);
        g_dcs.erase(object);
        g_dc_epoch.fetch_add(1, std::memory_order_acq_rel);
        break;
    }
    case kResourceTag: {
        ResourceRecord rec{};
        {
            std::unique_lock lk(g_res_mutex);
            auto it = g_resources.find(object);
            if (it == g_resources.end()) break;
            rec = it->second;
            g_resource_ids.erase(rec.id);
            g_resources.erase(it);
        }
        report_destroyed(rec);
        break;
    }
    case kIdTag: {
        std::unique_lock lk(g_ids_mutex);
        g_ids.erase(object);
        break;
    }
    case kShaderTag: {
        std::unique_lock lk(g_shader_mutex);
        g_shaders.erase(object);
        break;
    }
    case kStateTag: {
        // A state object gone: what it held can never be swapped in again, and
        // a new one at its address must not inherit it.
        std::shared_lock lk(g_dc_mutex);
        std::lock_guard sl(g_saved_mutex);
        for (auto& [dc, st] : g_dcs) {
            auto& saved = st->saved;
            for (size_t i = 0; i < saved.size();)
                if (saved[i].first == object) saved.erase(saved.begin() + static_cast<ptrdiff_t>(i)); else ++i;
            if (st->context_state == object) st->context_state = nullptr;
        }
        break;
    }
    default: break;
    }
}

bool watch(IUnknown* object, uint64_t tag) noexcept {
    return lifetime::watch(object, &on_destroyed, &g_keys[tag], tag);
}

void reset_registries() noexcept {
    for (char& key : g_keys) lifetime::forget(&key);
    { std::unique_lock lk(g_dc_mutex); g_dcs.clear(); g_dc_epoch.fetch_add(1, std::memory_order_acq_rel); }
    { std::unique_lock lk(g_res_mutex); g_resources.clear(); g_resource_ids.clear(); }
    { std::unique_lock lk(g_ids_mutex); g_ids.clear(); }
    { std::unique_lock lk(g_shader_mutex); g_shaders.clear(); }
}

// The shaders bound on a device context when it is first seen (set before
// VanGFX attached, so no hook reported them), or in a state swapped in that
// VanGFX has not seen before.
void read_bound_shaders(ID3D11DeviceContext* dc, DcBindings& st) noexcept {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader*  ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    UINT nv = DcBindings::kMaxInstances, np = DcBindings::kMaxInstances, nc = 0;
    dc->VSGetShader(&vs, st.vs_inst, &nv);
    dc->PSGetShader(&ps, st.ps_inst, &np);
    dc->CSGetShader(&cs, nullptr, &nc);
    st.vs = vs; st.ps = ps; st.cs = cs;
    st.vs_inst_n = nv; st.ps_inst_n = np;
    // Get* AddRefs; the state holds weak pointers, as the binding does not keep
    // a reference VanGFX would own.
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (cs) cs->Release();
    for (UINT i = 0; i < nv; ++i) if (st.vs_inst[i]) st.vs_inst[i]->Release();
    for (UINT i = 0; i < np; ++i) if (st.ps_inst[i]) st.ps_inst[i]->Release();
    st.vs_hash = shader_hash(vs);
    st.ps_hash = shader_hash(ps);
    st.cs_hash = shader_hash(cs);
    // And the targets, weakly like the shaders.
    ID3D11RenderTargetView* rtvs[8]{};
    ID3D11DepthStencilView* dsv = nullptr;
    dc->OMGetRenderTargets(8, rtvs, &dsv);
    st.rtv_count = 0;
    for (UINT i = 0; i < 8; ++i) {
        st.rtv[i] = rtvs[i];
        if (rtvs[i]) { st.rtv_count = i + 1; rtvs[i]->Release(); }
    }
    st.dsv = dsv;
    if (dsv) dsv->Release();
}

} // namespace

// ── Contexts ─────────────────────────────────────────────────────────────────

void hub_attach(D3D11ContextImpl* ctx) noexcept {
    AcquireSRWLockExclusive(&g_hub);
    g_contexts.push_back(ctx);
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_hub);
}

void hub_detach(D3D11ContextImpl* ctx) noexcept {
    const bool nested = t_reader_depth > 0;
    if (nested) ReleaseSRWLockShared(&g_hub);
    AcquireSRWLockExclusive(&g_hub);
    for (size_t i = 0; i < g_contexts.size(); ++i)
        if (g_contexts[i] == ctx) { g_contexts.erase(g_contexts.begin() + static_cast<ptrdiff_t>(i)); break; }
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    if (g_contexts.empty() && !nested) reset_registries();
    ReleaseSRWLockExclusive(&g_hub);
    if (nested) AcquireSRWLockShared(&g_hub);
}

bool     hub_idle() noexcept       { return g_context_count.load(std::memory_order_acquire) == 0; }
uint32_t hub_generation() noexcept { return g_generation.load(std::memory_order_acquire); }

bool hub_attached(const D3D11ContextImpl* ctx) noexcept {
    for (D3D11ContextImpl* c : g_contexts) if (c == ctx) return true;
    return false;
}

HubReader::HubReader() noexcept { if (t_reader_depth++ == 0) AcquireSRWLockShared(&g_hub); }
HubReader::~HubReader()         { if (--t_reader_depth == 0) ReleaseSRWLockShared(&g_hub); }

ContextSet contexts_for(const ID3D11Device* device) noexcept {
    ContextSet set;
    if (!device) return set;
    for (D3D11ContextImpl* c : g_contexts)
        if (c->device() == device && set.count < ContextSet::kMax) set.items[set.count++] = c;
    return set;
}

ID3D11Device* device_of(ID3D11DeviceChild* child) noexcept {
    if (!child) return nullptr;
    ID3D11Device* dev = nullptr;
    child->GetDevice(&dev);
    if (dev) dev->Release();   // the child keeps it alive
    return dev;
}

InternalScope::InternalScope() noexcept { ++t_internal; }
InternalScope::~InternalScope()         { --t_internal; }
bool internal() noexcept { return t_internal > 0; }

// ── Device contexts ──────────────────────────────────────────────────────────

void DcState::clear() noexcept {
    static_cast<DcBindings&>(*this) = DcBindings{};
    vs_swapped = ps_swapped = false;
}

void context_state_swapped(ID3D11DeviceContext* dc, DcState& st, ID3DDeviceContextState* outgoing,
                           ID3DDeviceContextState* incoming) noexcept {
    const DcBindings now = st;
    bool known = false;
    {
        std::lock_guard lk(g_saved_mutex);
        const void* key = outgoing ? outgoing : st.context_state;
        bool put = false;
        for (auto& [k, b] : st.saved) if (k == key) { b = now; put = true; break; }
        if (!put) st.saved.emplace_back(key, now);
        for (const auto& [k, b] : st.saved) {
            if (k != incoming) continue;
            static_cast<DcBindings&>(st) = b;
            known = true;
            break;
        }
        st.context_state = incoming;
    }
    st.vs_swapped = st.ps_swapped = false;
    if (!known) {
        // A state never seen swapped out: a new one holds the defaults, one
        // made before VanGFX attached holds what it holds -- its shaders at
        // least can be read back.
        static_cast<DcBindings&>(st) = DcBindings{};
        read_bound_shaders(dc, st);
    }
    if (outgoing) watch(outgoing, kStateTag);
    if (incoming) watch(incoming, kStateTag);
}

DcState* dc_state(ID3D11DeviceContext* dc) noexcept {
    if (!dc || t_internal > 0) return nullptr;
    const uint32_t epoch = g_dc_epoch.load(std::memory_order_acquire);
    DcState* st = nullptr;
    if (t_dc_cache.dc == dc && t_dc_cache.epoch == epoch) {
        st = t_dc_cache.state;
    } else {
        {
            std::shared_lock lk(g_dc_mutex);
            auto it = g_dcs.find(dc);
            if (it != g_dcs.end()) st = it->second.get();
        }
        if (!st) {
            auto fresh = std::make_unique<DcState>();
            fresh->device   = device_of(dc);
            fresh->deferred = dc->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
            read_bound_shaders(dc, *fresh);
            std::unique_lock lk(g_dc_mutex);
            auto [it, inserted] = g_dcs.try_emplace(dc, std::move(fresh));
            st = it->second.get();
            if (inserted) watch(dc, kDcTag);
        }
        t_dc_cache = { dc, st, epoch };
    }
    const uint32_t gen = hub_generation();
    if (st->gen != gen) {
        st->contexts = contexts_for(st->device);
        st->gen      = gen;
    }
    return st->contexts ? st : nullptr;
}

// ── Resources ────────────────────────────────────────────────────────────────

uint64_t resource_register(ID3D11Resource* res, ResourceKind kind, ID3D11Device* device) noexcept {
    if (!res) return 0;
    {
        std::shared_lock lk(g_res_mutex);
        auto it = g_resources.find(res);
        if (it != g_resources.end()) return it->second.id;
    }
    const ResourceRecord rec{ ContextImpl::alloc_id(), kind, device };
    std::unique_lock lk(g_res_mutex);
    auto [it, inserted] = g_resources.try_emplace(res, rec);
    if (inserted) {
        g_resource_ids[rec.id] = res;
        watch(res, kResourceTag);
    }
    return it->second.id;
}

uint64_t resource_id(const ID3D11Resource* res) noexcept {
    if (!res) return 0;
    std::shared_lock lk(g_res_mutex);
    auto it = g_resources.find(res);
    return it != g_resources.end() ? it->second.id : 0;
}

ID3D11Resource* resource_from_id(uint64_t id) noexcept {
    std::shared_lock lk(g_res_mutex);
    auto it = g_resource_ids.find(id);
    return it != g_resource_ids.end() ? it->second : nullptr;
}

uint64_t view_resource_id(ID3D11View* view, ID3D11Device* device) noexcept {
    if (!view) return 0;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (!res) return 0;
    uint64_t id = resource_id(res);
    if (!id) {
        announce_resource(device, res);
        id = resource_id(res);
    }
    res->Release();   // the view keeps it alive
    return id;
}

uint64_t id_of(ID3D11DeviceChild* object) noexcept {
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

void shader_register(ID3D11DeviceChild* shader, uint64_t hash) noexcept {
    if (!shader || !hash) return;
    std::unique_lock lk(g_shader_mutex);
    auto [it, inserted] = g_shaders.insert_or_assign(shader, hash);
    if (inserted) watch(shader, kShaderTag);
}

uint64_t shader_hash(const void* shader) noexcept {
    if (!shader) return 0;
    std::shared_lock lk(g_shader_mutex);
    auto it = g_shaders.find(shader);
    return it != g_shaders.end() ? it->second : 0;
}

}  // namespace vangfx::backend::d3d11
