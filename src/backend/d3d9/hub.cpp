/**
 * src/backend/d3d9/hub.cpp — see d3d9_impl.hpp.
 */

#include "d3d9_impl.hpp"
#include <compat/lifetime.hpp>
#include <hash.hpp>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d9 {

namespace {

SRWLOCK                        g_hub = SRWLOCK_INIT;
std::vector<D3D9ContextImpl*>  g_contexts;
std::atomic<int>               g_context_count{ 0 };
std::atomic<uint32_t>          g_generation{ 1 };
thread_local int               t_reader_depth = 0;
thread_local int               t_internal     = 0;

// One lifetime key for the resource registry (see the D3D12 hub for why one
// per registry).
char g_resource_key;

// ── Devices ──────────────────────────────────────────────────────────────────

std::shared_mutex                                         g_dev_mutex;
std::unordered_map<const void*, std::unique_ptr<DevState>> g_devs;
std::atomic<uint32_t>                                     g_dev_epoch{ 1 };
struct DevCache { const void* dev = nullptr; DevState* state = nullptr; uint32_t epoch = 0; };
thread_local DevCache t_dev_cache;

// ── Resources, queries, shaders ──────────────────────────────────────────────

struct ResourceRecord {
    uint64_t          id = 0;
    ResourceKind      kind = ResourceKind::Texture;
    IDirect3DDevice9* device = nullptr;
    bool              alias = false;
};
std::shared_mutex                                   g_res_mutex;
std::unordered_map<const void*, ResourceRecord>     g_resources;
std::unordered_map<uint64_t, IDirect3DResource9*>   g_resource_ids;

std::shared_mutex                                   g_query_mutex;
std::unordered_map<const void*, QueryInfo>          g_queries;

std::shared_mutex                                   g_shader_mutex;
std::unordered_map<const void*, ShaderInfo>         g_shaders;

void report_destroyed(const ResourceRecord& rec) noexcept {
    if (hub_idle() || rec.alias) return;
    HubReader r;
    for (D3D9ContextImpl* ctx : contexts_for(rec.device)) {
        ctx->store().forget(rec.id);   // before any callback can ask to read it
        if (!ctx->wants_resource()) continue;
        ResourceEvent ev{};
        ev.kind   = rec.kind;
        ev.action = ResourceAction::Destroy;
        switch (rec.kind) {
        case ResourceKind::Texture:      ev.texture       = { rec.id }; break;
        case ResourceKind::RenderTarget: ev.render_target = { rec.id }; break;
        case ResourceKind::DepthBuffer:  ev.depth_buffer  = { rec.id }; break;
        case ResourceKind::VertexBuffer: ev.vertex_buffer = { rec.id }; break;
        case ResourceKind::IndexBuffer:  ev.index_buffer  = { rec.id }; break;
        default: break;
        }
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_resource(fc, ev);
    }
}

void on_destroyed(void*, const void* object, uint64_t) noexcept {
    ResourceRecord rec{};
    {
        std::unique_lock lk(g_res_mutex);
        auto it = g_resources.find(object);
        if (it == g_resources.end()) return;
        rec = it->second;
        auto ids = g_resource_ids.find(rec.id);
        if (ids != g_resource_ids.end() && ids->second == object) g_resource_ids.erase(ids);
        g_resources.erase(it);
    }
    report_destroyed(rec);
}

void reset_registries() noexcept {
    lifetime::forget(&g_resource_key);
    { std::unique_lock lk(g_dev_mutex); g_devs.clear(); g_dev_epoch.fetch_add(1, std::memory_order_acq_rel); }
    { std::unique_lock lk(g_res_mutex); g_resources.clear(); g_resource_ids.clear(); }
    { std::unique_lock lk(g_query_mutex); g_queries.clear(); }
    { std::unique_lock lk(g_shader_mutex); g_shaders.clear(); }
}

QueryType translate_query_type(D3DQUERYTYPE t) noexcept {
    switch (t) {
    case D3DQUERYTYPE_EVENT:             return QueryType::Event;
    case D3DQUERYTYPE_OCCLUSION:         return QueryType::Occlusion;
    case D3DQUERYTYPE_TIMESTAMP:         return QueryType::Timestamp;
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    case D3DQUERYTYPE_TIMESTAMPFREQ:     return QueryType::TimestampDisjoint;
    case D3DQUERYTYPE_PIPELINETIMINGS:
    case D3DQUERYTYPE_VERTEXSTATS:       return QueryType::PipelineStatistics;
    default:                             return QueryType::Event;
    }
}

QueryInfo describe_query(IDirect3DQuery9* query) noexcept {
    QueryInfo info;
    info.id   = ContextImpl::alloc_id();
    info.raw  = query->GetType();
    info.type = translate_query_type(info.raw);
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(query->GetDevice(&dev)) && dev) {
        info.device = dev;
        dev->Release();   // the query holds it
    }
    return info;
}

// A shader not seen being created: its bytecode read back from the object.
template <typename S>
ShaderInfo lookup_shader(S* shader) noexcept {
    if (!shader) return {};
    {
        std::shared_lock lk(g_shader_mutex);
        auto it = g_shaders.find(shader);
        if (it != g_shaders.end()) return it->second;
    }
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || size < sizeof(DWORD)) return {};
    std::vector<DWORD> code((size + sizeof(DWORD) - 1) / sizeof(DWORD));
    if (FAILED(shader->GetFunction(code.data(), &size))) return {};
    const ShaderInfo info{ hash_bytes(code.data(), size), shader_major(code.data()) };
    std::unique_lock lk(g_shader_mutex);
    return g_shaders.try_emplace(shader, info).first->second;
}

} // namespace

// ── Contexts ─────────────────────────────────────────────────────────────────

void hub_attach(D3D9ContextImpl* ctx) noexcept {
    AcquireSRWLockExclusive(&g_hub);
    g_contexts.push_back(ctx);
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_hub);
}

void hub_detach(D3D9ContextImpl* ctx) noexcept {
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

bool hub_attached(const D3D9ContextImpl* ctx) noexcept {
    for (D3D9ContextImpl* c : g_contexts) if (c == ctx) return true;
    return false;
}

HubReader::HubReader() noexcept { if (t_reader_depth++ == 0) AcquireSRWLockShared(&g_hub); }
HubReader::~HubReader()         { if (--t_reader_depth == 0) ReleaseSRWLockShared(&g_hub); }

ContextSet contexts_for(const IDirect3DDevice9* device) noexcept {
    ContextSet set;
    if (!device) return set;
    for (D3D9ContextImpl* c : g_contexts)
        if (c->device() == device && !c->orphaned() && set.count < ContextSet::kMax) set.items[set.count++] = c;
    return set;
}

InternalScope::InternalScope() noexcept { ++t_internal; }
InternalScope::~InternalScope()         { --t_internal; }
bool internal() noexcept { return t_internal > 0; }

// ── Devices ──────────────────────────────────────────────────────────────────

void read_shaders(IDirect3DDevice9* device, DevState& st) noexcept {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9*  ps = nullptr;
    // A pure device may answer no Get*: the shaders stay unknown until next set.
    if (FAILED(device->GetVertexShader(&vs))) vs = nullptr;
    if (FAILED(device->GetPixelShader(&ps)))  ps = nullptr;
    // Get* AddRefs; the state holds weak pointers, as the binding does not keep
    // a reference VanGFX would own.
    if (vs) vs->Release();
    if (ps) ps->Release();
    const ShaderInfo vi = shader_info(vs), pi = shader_info(ps);
    st.vs = vs; st.ps = ps;
    st.vs_hash = vi.hash; st.vs_major = vi.major;
    st.ps_hash = pi.hash; st.ps_major = pi.major;
    st.vs_swapped = st.ps_swapped = false;
}

void reread_device(IDirect3DDevice9* device, DevState& st) noexcept {
    st.recording = false;
    st.pipeline  = PipelineState{};
    st.surfaces  = FrameSurfaces{};
    read_shaders(device, st);
    read_pipeline(device, st.pipeline);
    read_surfaces(device, st);
}

DevState* dev_state(IDirect3DDevice9* device) noexcept {
    if (!device || t_internal > 0) return nullptr;
    const uint32_t epoch = g_dev_epoch.load(std::memory_order_acquire);
    DevState* st = nullptr;
    bool first_sight = false;
    if (t_dev_cache.dev == device && t_dev_cache.epoch == epoch) {
        st = t_dev_cache.state;
    } else {
        {
            std::shared_lock lk(g_dev_mutex);
            auto it = g_devs.find(device);
            if (it != g_devs.end()) st = it->second.get();
        }
        if (!st) {
            auto fresh = std::make_unique<DevState>();
            fresh->device = device;
            // What was set before VanGFX attached: no hook reported it.
            read_shaders(device, *fresh);
            read_pipeline(device, fresh->pipeline);
            std::unique_lock lk(g_dev_mutex);
            auto [it, inserted] = g_devs.try_emplace(device, std::move(fresh));
            st = it->second.get();
            first_sight = inserted;
        }
        t_dev_cache = { device, st, epoch };
    }
    const uint32_t gen = hub_generation();
    if (st->gen != gen) {
        st->contexts = contexts_for(st->device);
        st->gen      = gen;
    }
    // The targets bound before attaching. Read once the state is in the map
    // and knows its contexts: a back buffer met here is announced (a Create
    // event), and a callback that runs then may come back through a hook.
    if (first_sight) read_surfaces(device, *st);
    return st->contexts ? st : nullptr;
}

void forget_device(const IDirect3DDevice9* device) noexcept {
    {
        HubReader reader;
        bool any = false;
        for (D3D9ContextImpl* c : g_contexts)
            if (c->device() == device) { c->orphan(); any = true; }
        if (any) g_generation.fetch_add(1, std::memory_order_acq_rel);   // cached context sets are stale
    }
    std::unique_lock lk(g_dev_mutex);
    if (g_devs.erase(device)) g_dev_epoch.fetch_add(1, std::memory_order_acq_rel);
}

// ── Resources ────────────────────────────────────────────────────────────────

uint64_t resource_register(IDirect3DResource9* res, ResourceKind kind, IDirect3DDevice9* device,
                           uint64_t alias_of) noexcept {
    if (!res) return 0;
    {
        std::shared_lock lk(g_res_mutex);
        auto it = g_resources.find(res);
        if (it != g_resources.end()) return it->second.id;
    }
    const ResourceRecord rec{ alias_of ? alias_of : ContextImpl::alloc_id(), kind, device, alias_of != 0 };
    std::unique_lock lk(g_res_mutex);
    auto [it, inserted] = g_resources.try_emplace(res, rec);
    if (inserted) {
        if (!rec.alias) g_resource_ids[rec.id] = res;
        lifetime::watch(res, &on_destroyed, &g_resource_key, 0);
    }
    return it->second.id;
}

uint64_t resource_id(const void* res) noexcept {
    if (!res) return 0;
    std::shared_lock lk(g_res_mutex);
    auto it = g_resources.find(res);
    return it != g_resources.end() ? it->second.id : 0;
}

bool resource_lookup(const void* res, uint64_t& id, ResourceKind& kind) noexcept {
    if (!res) return false;
    std::shared_lock lk(g_res_mutex);
    auto it = g_resources.find(res);
    if (it == g_resources.end()) return false;
    id   = it->second.id;
    kind = it->second.kind;
    return true;
}

IDirect3DResource9* resource_from_id(uint64_t id) noexcept {
    std::shared_lock lk(g_res_mutex);
    auto it = g_resource_ids.find(id);
    return it != g_resource_ids.end() ? it->second : nullptr;
}

uint64_t surface_id(IDirect3DSurface9* surface, IDirect3DDevice9* device) noexcept {
    if (!surface) return 0;
    if (const uint64_t id = resource_id(surface)) return id;

    // The top level of a 2D texture is reported as the texture, so a render
    // target reads back and matches its Create event under one id.
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) && tex) {
        IDirect3DSurface9* top = nullptr;
        uint64_t id = 0;
        if (SUCCEEDED(tex->GetSurfaceLevel(0, &top)) && top == surface) {
            id = resource_id(tex);
            if (!id) {
                announce_resource(device, tex);
                id = resource_id(tex);
            }
            if (id) {
                D3DSURFACE_DESC d{};
                surface->GetDesc(&d);
                resource_register(surface, (d.Usage & D3DUSAGE_DEPTHSTENCIL) ? ResourceKind::DepthBuffer
                                                                             : ResourceKind::RenderTarget,
                                  device, id);
            }
        }
        if (top) top->Release();
        tex->Release();
        if (id) return id;
    }
    announce_resource(device, surface);
    return resource_id(surface);
}

// ── Queries and shaders ──────────────────────────────────────────────────────

QueryInfo query_register(IDirect3DQuery9* query) noexcept {
    if (!query) return {};
    const QueryInfo info = describe_query(query);
    std::unique_lock lk(g_query_mutex);
    g_queries.insert_or_assign(query, info);
    return info;
}

QueryInfo query_info(IDirect3DQuery9* query) noexcept {
    if (!query) return {};
    {
        std::shared_lock lk(g_query_mutex);
        auto it = g_queries.find(query);
        if (it != g_queries.end()) return it->second;
    }
    const QueryInfo info = describe_query(query);
    std::unique_lock lk(g_query_mutex);
    return g_queries.try_emplace(query, info).first->second;
}

void shader_register(const void* shader, const void* code, uint32_t size) noexcept {
    if (!shader || !code || size < sizeof(DWORD)) return;
    const ShaderInfo info{ hash_bytes(code, size), shader_major(code) };
    std::unique_lock lk(g_shader_mutex);
    g_shaders.insert_or_assign(shader, info);
}

ShaderInfo shader_info(IDirect3DVertexShader9* vs) noexcept { return lookup_shader(vs); }
ShaderInfo shader_info(IDirect3DPixelShader9* ps) noexcept  { return lookup_shader(ps); }

}  // namespace vangfx::backend::d3d9
