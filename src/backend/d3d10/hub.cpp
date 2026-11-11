/**
 * src/backend/d3d10/hub.cpp — global context registry for the D3D10 backend.
 */

#include "d3d10_impl.hpp"
#include <compat/lifetime.hpp>

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d10 {

namespace {

SRWLOCK                          g_hub = SRWLOCK_INIT;
std::vector<D3D10ContextImpl*>   g_contexts;
std::atomic<int>                 g_context_count{ 0 };
std::atomic<uint32_t>            g_generation{ 1 };
thread_local int                 t_reader_depth = 0;
thread_local int                 t_internal     = 0;

// ── Per-device state ──────────────────────────────────────────────────────────

std::shared_mutex                                           g_dev_mutex;
std::unordered_map<const void*, std::unique_ptr<DevState>> g_devs;
std::atomic<uint32_t>                                       g_dev_epoch{ 1 };

struct DevCache { const void* dev = nullptr; DevState* state = nullptr; uint32_t epoch = 0; };
thread_local DevCache t_dev_cache;

// ── Shader registry ───────────────────────────────────────────────────────────

enum : uint64_t { kShaderTag = 1 };
char g_shader_key{};

std::shared_mutex                          g_shader_mutex;
std::unordered_map<const void*, uint64_t>  g_shaders;

void on_destroyed(void*, const void* object, uint64_t tag) noexcept {
    if (tag == kShaderTag) {
        std::unique_lock lk(g_shader_mutex);
        g_shaders.erase(object);
    }
}

void reset_registries() noexcept {
    lifetime::forget(&g_shader_key);
    std::unique_lock lk(g_shader_mutex);
    g_shaders.clear();
}

} // namespace

// ── Hub ───────────────────────────────────────────────────────────────────────

void hub_attach(D3D10ContextImpl* ctx) noexcept {
    AcquireSRWLockExclusive(&g_hub);
    g_contexts.push_back(ctx);
    g_context_count.store(static_cast<int>(g_contexts.size()), std::memory_order_release);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_hub);
}

void hub_detach(D3D10ContextImpl* ctx) noexcept {
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
    if (g_contexts.empty() && !nested) reset_registries();
    ReleaseSRWLockExclusive(&g_hub);
    if (nested) AcquireSRWLockShared(&g_hub);
}

bool     hub_idle()       noexcept { return g_context_count.load(std::memory_order_acquire) == 0; }
uint32_t hub_generation() noexcept { return g_generation.load(std::memory_order_acquire); }

bool hub_attached(const D3D10ContextImpl* ctx) noexcept {
    for (D3D10ContextImpl* c : g_contexts) if (c == ctx) return true;
    return false;
}

HubReader::HubReader() noexcept { if (t_reader_depth++ == 0) AcquireSRWLockShared(&g_hub); }
HubReader::~HubReader()         { if (--t_reader_depth == 0) ReleaseSRWLockShared(&g_hub); }

ContextSet contexts_for(const ID3D10Device* device) noexcept {
    ContextSet set;
    if (!device) return set;
    for (D3D10ContextImpl* c : g_contexts)
        if (c->device() == device && set.count < ContextSet::kMax)
            set.items[set.count++] = c;
    return set;
}

InternalScope::InternalScope() noexcept { ++t_internal; }
InternalScope::~InternalScope()         { --t_internal; }
bool internal() noexcept { return t_internal > 0; }

// ── Per-device state ──────────────────────────────────────────────────────────

DevState* dev_state(ID3D10Device* dev) noexcept {
    if (!dev || t_internal > 0) return nullptr;
    const uint32_t epoch = g_dev_epoch.load(std::memory_order_acquire);
    DevState* st = nullptr;
    if (t_dev_cache.dev == dev && t_dev_cache.epoch == epoch) {
        st = t_dev_cache.state;
    } else {
        {
            std::shared_lock lk(g_dev_mutex);
            auto it = g_devs.find(dev);
            if (it != g_devs.end()) st = it->second.get();
        }
        if (!st) {
            auto fresh = std::make_unique<DevState>();
            std::unique_lock lk(g_dev_mutex);
            auto [it, inserted] = g_devs.try_emplace(dev, std::move(fresh));
            st = it->second.get();
        }
        t_dev_cache = { dev, st, epoch };
    }
    const uint32_t gen = hub_generation();
    if (st->gen != gen) {
        st->contexts = contexts_for(dev);
        st->gen      = gen;
    }
    return st->contexts ? st : nullptr;
}

// ── Shader registry ───────────────────────────────────────────────────────────

void shader_register(ID3D10DeviceChild* shader, uint64_t hash) noexcept {
    if (!shader || !hash) return;
    std::unique_lock lk(g_shader_mutex);
    auto [it, inserted] = g_shaders.insert_or_assign(shader, hash);
    if (inserted) lifetime::watch(shader, &on_destroyed, &g_shader_key, kShaderTag);
}

uint64_t shader_hash(const void* shader) noexcept {
    if (!shader) return 0;
    std::shared_lock lk(g_shader_mutex);
    auto it = g_shaders.find(shader);
    return it != g_shaders.end() ? it->second : 0;
}

} // namespace vangfx::backend::d3d10
