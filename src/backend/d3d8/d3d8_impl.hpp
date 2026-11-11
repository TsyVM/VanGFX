#pragma once

/**
 * src/backend/d3d8/d3d8_impl.hpp — D3D8 backend shared declarations.
 *
 * D3D8 is a 32-bit only API (no x64 d3d8.dll).  The modern Windows SDK does
 * not ship d3d8.h, so this backend uses opaque forward-declared types with
 * explicit vtable slot indices from d3d8_slots.hpp.
 *
 * "Device" key: IDirect3DDevice8* (opaque void* at this layer).
 *
 * Shader identifiers in D3D8 are DWORD handles, not COM objects.  The D3D8
 * backend tracks (vs_handle, ps_handle) DWORDs and hashes them directly.
 */

#include "../../core/context_impl.hpp"
#include <compat/hook_site.hpp>
#include <internal/compat/d3d8_slots.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <vector>

// Forward-declare the D3D8 COM interfaces as opaque types.  We never call
// methods by name: all calls go through vtable slot helpers below.
struct IDirect3DDevice8;
struct IDirect3D8;

namespace vangfx::backend::d3d8 {

class D3D8ContextImpl;

// ── Vtable call helpers ───────────────────────────────────────────────────────
// Call a method at a fixed vtable slot on an opaque COM object.

template<typename Ret, typename... Args>
[[nodiscard]] static inline Ret vtbl_call(void* obj, uint32_t slot,
                                           Args... args) noexcept {
    using Fn = Ret(STDMETHODCALLTYPE*)(void*, Args...);
    void** vtbl = *reinterpret_cast<void***>(obj);
    return reinterpret_cast<Fn>(vtbl[slot])(obj, args...);
}

template<typename Ret, typename... Args>
static inline void vtbl_patch(void* obj, uint32_t slot, Ret(STDMETHODCALLTYPE* fn)(void*, Args...)) noexcept {
    // Write the hook function pointer into the vtable slot.
    // Requires the vtable page to be writable (DWORD-patched on Win32).
    void** vtbl = *reinterpret_cast<void***>(obj);
    DWORD old; VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old);
    vtbl[slot] = reinterpret_cast<void*>(fn);
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
}

// ── Hub ───────────────────────────────────────────────────────────────────────

void hub_attach(D3D8ContextImpl* ctx) noexcept;
void hub_detach(D3D8ContextImpl* ctx) noexcept;
[[nodiscard]] bool     hub_idle()       noexcept;
[[nodiscard]] uint32_t hub_generation() noexcept;

class HubReader {
public:
    HubReader() noexcept;
    ~HubReader();
    HubReader(const HubReader&) = delete;
    HubReader& operator=(const HubReader&) = delete;
};

struct ContextSet {
    static constexpr uint32_t kMax = 4;
    D3D8ContextImpl* items[kMax]{};
    uint32_t         count = 0;
    D3D8ContextImpl* const* begin() const noexcept { return items; }
    D3D8ContextImpl* const* end()   const noexcept { return items + count; }
    [[nodiscard]] explicit operator bool() const noexcept { return count != 0; }
};

[[nodiscard]] ContextSet contexts_for(IDirect3DDevice8* device) noexcept;

class InternalScope {
public:
    InternalScope() noexcept;
    ~InternalScope();
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};
[[nodiscard]] bool internal() noexcept;

// ── Per-device state ──────────────────────────────────────────────────────────

struct DevState {
    // D3D8 shader handles are DWORDs (not COM pointers).
    uint32_t vs_handle = 0;
    uint32_t ps_handle = 0;
    uint64_t vs_hash   = 0;
    uint64_t ps_hash   = 0;

    uint32_t   gen      = UINT32_MAX;
    ContextSet contexts;
};

[[nodiscard]] DevState* dev_state(IDirect3DDevice8* device) noexcept;

// Hash a D3D8 DWORD shader handle: stable, non-zero for a live shader.
[[nodiscard]] uint64_t make_shader_hash(uint32_t handle) noexcept;

// ── Frame context (FrameContext subclass) ─────────────────────────────────────

class D3D8FrameContextImpl final : public StubFrameContext {
public:
    D3D8FrameContextImpl(D3D8ContextImpl& ctx, uint32_t frame_index,
                          IDirect3DDevice8* device, DevState* state)
        : frame_index_(frame_index), device_(device), state_(state) { (void)ctx; }

    BackendKind backend()    const noexcept override { return BackendKind::D3D8; }
    uint32_t frame_index()   const noexcept override { return frame_index_; }

    void* native_device()  const noexcept override;
    void* native_context() const noexcept override;

private:
    uint32_t          frame_index_;
    IDirect3DDevice8* device_;
    DevState*         state_;
};

// ── Context impl ──────────────────────────────────────────────────────────────

class D3D8ContextImpl final : public ContextImpl {
public:
    D3D8ContextImpl(const ContextBuilder& b, IDirect3DDevice8* device);
    ~D3D8ContextImpl() override;

    BackendKind backend() const noexcept override { return BackendKind::D3D8; }
    IDirect3DDevice8* device() const noexcept { return device_; }

    // Called by vtable hooks to advance timing and dispatch per-frame events.
    struct Timing { float time; float delta; bool first; };
    Timing   tick() noexcept;
    uint32_t next_frame() noexcept { return frame_counter_.fetch_add(1, std::memory_order_relaxed); }
    uint32_t current_frame_index() const noexcept {
        return frame_counter_.load(std::memory_order_acquire);
    }
    std::string post_pass_profile() const override { return "d3d8"; }

private:
    IDirect3DDevice8* device_;

    using Clock = std::chrono::steady_clock;
    Clock::time_point start_{ Clock::now() };
    Clock::time_point last_ { Clock::now() };
    bool              first_frame_ = true;
    std::mutex        timing_mutex_;
    std::atomic<uint32_t> frame_counter_{ 0 };
};

// ── Hook install / uninstall ───────────────────────────────────────────────────

bool install_draw_hooks(IDirect3DDevice8* dev)    noexcept;
bool install_present_hooks(IDirect3DDevice8* dev) noexcept;
void uninstall_all_hooks(IDirect3DDevice8* dev)   noexcept;

} // namespace vangfx::backend::d3d8
