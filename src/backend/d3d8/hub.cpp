/**
 * src/backend/d3d8/hub.cpp — global context registry for the D3D8 backend.
 *
 * Maps IDirect3DDevice8* → DevState.  The same SRWLOCK+g_contexts+g_devs
 * pattern used by D3D10 and the GL backend.
 */

#include "d3d8_impl.hpp"

#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace vangfx::backend::d3d8 {

namespace {

SRWLOCK                       g_hub = SRWLOCK_INIT;
std::vector<D3D8ContextImpl*> g_contexts;
std::atomic<int>              g_context_count{ 0 };
std::atomic<uint32_t>         g_generation{ 1 };
thread_local int              t_reader_depth = 0;
thread_local int              t_internal     = 0;

std::shared_mutex                                              g_dev_mutex;
std::unordered_map<const void*, std::unique_ptr<DevState>>    g_devs;
std::atomic<uint32_t>                                          g_dev_epoch{ 1 };

struct DevCache { const void* key = nullptr; DevState* state = nullptr; uint32_t epoch = 0; };
thread_local DevCache t_dev_cache;

} // namespace

// ── Hub ───────────────────────────────────────────────────────────────────────

void hub_attach(D3D8ContextImpl* ctx) noexcept {
    AcquireSRWLockExclusive(&g_hub);
    g_contexts.push_back(ctx);
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_hub);
}

void hub_detach(D3D8ContextImpl* ctx) noexcept {
    const bool nested = t_reader_depth > 0;
    if (nested) ReleaseSRWLockShared(&g_hub);
    AcquireSRWLockExclusive(&g_hub);
    for (size_t i = 0; i < g_contexts.size(); ++i) {
        if (g_contexts[i] == ctx) {
            g_contexts.erase(g_contexts.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_hub);
    if (nested) AcquireSRWLockShared(&g_hub);
}

bool     hub_idle()       noexcept { return g_context_count.load(std::memory_order_acquire) == 0; }
uint32_t hub_generation() noexcept { return g_generation.load(std::memory_order_acquire); }

HubReader::HubReader() noexcept { if (t_reader_depth++ == 0) AcquireSRWLockShared(&g_hub); }
HubReader::~HubReader()         { if (--t_reader_depth == 0) ReleaseSRWLockShared(&g_hub); }

ContextSet contexts_for(IDirect3DDevice8* device) noexcept {
    ContextSet set;
    for (D3D8ContextImpl* c : g_contexts) {
        if ((!c->device() || c->device() == device) && set.count < ContextSet::kMax)
            set.items[set.count++] = c;
    }
    return set;
}

InternalScope::InternalScope() noexcept { ++t_internal; }
InternalScope::~InternalScope()         { --t_internal; }
bool internal() noexcept { return t_internal > 0; }

// ── Per-device state ──────────────────────────────────────────────────────────

DevState* dev_state(IDirect3DDevice8* device) noexcept {
    if (!device || t_internal > 0) return nullptr;
    const void* key = static_cast<const void*>(device);
    const uint32_t epoch = g_dev_epoch.load(std::memory_order_acquire);
    DevState* st = nullptr;
    if (t_dev_cache.key == key && t_dev_cache.epoch == epoch) {
        st = t_dev_cache.state;
    } else {
        {
            std::shared_lock lk(g_dev_mutex);
            auto it = g_devs.find(key);
            if (it != g_devs.end()) st = it->second.get();
        }
        if (!st) {
            auto fresh = std::make_unique<DevState>();
            std::unique_lock lk(g_dev_mutex);
            auto [it, inserted] = g_devs.try_emplace(key, std::move(fresh));
            st = it->second.get();
        }
        t_dev_cache = { key, st, epoch };
    }
    const uint32_t gen = hub_generation();
    if (st->gen != gen) {
        st->contexts = contexts_for(device);
        st->gen      = gen;
    }
    return st->contexts ? st : nullptr;
}

// ── Shader hash ───────────────────────────────────────────────────────────────

uint64_t make_shader_hash(uint32_t handle) noexcept {
    if (!handle) return 0;
    // FNV-1a-inspired: mix the DWORD handle into a 64-bit hash.
    uint64_t v = (uint64_t)handle * 0x9e3779b97f4a7c15ULL;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return v ? v : 1;
}

} // namespace vangfx::backend::d3d8
