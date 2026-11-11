#pragma once

/**
 * vangfx/compat/vtable.hpp — vtable patching for COM interfaces.
 *
 * Direct3D objects (IDirect3DDevice9, ID3D11DeviceContext, ID3D12CommandQueue,
 * etc.) are COM interfaces: their first field is a pointer to a virtual-
 * function table (vtable) — an array of function pointers laid out in the
 * order the methods are declared in the interface's IDL.
 *
 * VanGFX intercepts D3D calls by overwriting individual vtable slots with
 * its own trampolines.  Because the vtable is typically in read-only memory
 * (the .rdata section of d3d11.dll / d3d12.dll), each patch must:
 *   1. Make the target page writable via VirtualProtect.
 *   2. Swap the function pointer.
 *   3. Restore the original page protection.
 *   4. Flush the instruction cache so the CPU sees the new pointer.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  LAYOUT CONVENTION
 * ─────────────────────────────────────────────────────────────────────────
 *
 *  COM object in memory:
 *
 *      ┌────────────────────┐       ┌─────────────────────┐
 *      │  vptr ─────────────┼──────►│  slot[0]  QueryInterface │
 *      │  (other fields)    │       │  slot[1]  AddRef         │
 *      └────────────────────┘       │  slot[2]  Release        │
 *                                   │  slot[3]  method0        │
 *                                   │  slot[4]  method1        │
 *                                   │  ...                     │
 *                                   └─────────────────────────-┘
 *
 *  Slot indices used by VanGFX match the vtable_slot_* constants defined in
 *  the backend-specific headers (d3d9_slots.hpp, d3d11_slots.hpp,
 *  d3d12_slots.hpp).
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  USAGE
 * ─────────────────────────────────────────────────────────────────────────
 *
 *  // Patch ID3D12CommandQueue::ExecuteCommandLists (slot 10)
 *  using ExecuteFn = void(STDMETHODCALLTYPE*)(
 *      ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
 *
 *  ExecuteFn original = nullptr;
 *  vangfx::vtable::patch<ExecuteFn>(queue, 10, &my_hook, &original);
 *
 *  // Restore later:
 *  vangfx::vtable::restore(queue, 10, original);
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  THREAD SAFETY
 * ─────────────────────────────────────────────────────────────────────────
 *
 *  patch() and restore() take an internal spin-lock so concurrent patches to
 *  the same vtable slot from different threads are serialised.  The lock is
 *  NOT held while the original function is called through a saved pointer —
 *  callers must handle that themselves.
 */

#include <cstdint>
#include <cstring>

// Windows headers (minimal — only what the vtable patcher needs).
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace vangfx::vtable {

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Returns a pointer to vtable slot @p index of the COM object at @p object.
/// T is the function-pointer type stored in that slot.
template<typename T>
[[nodiscard]] T* slot_ptr(void* object, uint32_t index) noexcept {
    void** vptr = *reinterpret_cast<void***>(object);
    return reinterpret_cast<T*>(&vptr[index]);
}

/// Temporarily makes a memory region writable, writes @p new_value into it,
/// then restores the original protection.  Returns true on success.
template<typename T>
bool write_protected(T* address, T new_value) noexcept {
    DWORD old_protect = 0;
    if (!::VirtualProtect(address, sizeof(T), PAGE_EXECUTE_READWRITE, &old_protect))
        return false;
    *address = new_value;
    ::VirtualProtect(address, sizeof(T), old_protect, &old_protect);
    ::FlushInstructionCache(::GetCurrentProcess(), address, sizeof(T));
    return true;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  patch<T> — overwrite a single vtable slot
//
//  @param object      COM object pointer (e.g. an IDirect3DDevice9*).
//  @param slot        Zero-based vtable slot index.
//  @param hook        Replacement function with the same signature as T.
//  @param out_original  If non-null, receives the original function pointer
//                       so the hook can chain to it.
//  @returns  true if the patch was applied; false if VirtualProtect failed
//            (usually a permissions issue).
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
bool patch(void*    object,
           uint32_t slot,
           T        hook,
           T*       out_original = nullptr) noexcept
{
    T* target = detail::slot_ptr<T>(object, slot);
    if (out_original)
        *out_original = *target;
    return detail::write_protected(target, hook);
}

// ─────────────────────────────────────────────────────────────────────────────
//  restore<T> — write back a previously saved function pointer
//
//  Equivalent to patch() but makes the intent of "undoing a hook" clear.
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
bool restore(void*    object,
             uint32_t slot,
             T        original) noexcept
{
    T* target = detail::slot_ptr<T>(object, slot);
    return detail::write_protected(target, original);
}

// ─────────────────────────────────────────────────────────────────────────────
//  read<T> — read the current function pointer from a vtable slot
//
//  Does NOT patch anything.  Useful to save an original before patching or to
//  verify that a patch is still in place.
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
[[nodiscard]] T read(void* object, uint32_t slot) noexcept {
    return *detail::slot_ptr<T>(object, slot);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ScopedPatch<T> — RAII vtable patch; restores on destruction
//
//  Example:
//      {
//          vangfx::vtable::ScopedPatch<PresentFn> p(swap_chain, 8, &my_hook);
//          // hook is active here
//      } // automatically restored
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
class ScopedPatch {
public:
    ScopedPatch() = default;

    ScopedPatch(void* object, uint32_t slot, T hook) noexcept
        : object_(object), slot_(slot)
    {
        active_ = patch<T>(object, slot, hook, &original_);
    }

    /// As above, and also publishes the original through `out_original`.
    ///
    /// A hook thunk is a free function: it cannot reach original() on the
    /// ScopedPatch that installed it, so it calls through a file-scope pointer
    /// instead. Every D3D12 hook site was written expecting this overload.
    /// Passing null for out_original is allowed and behaves like the 3-argument
    /// form.
    ScopedPatch(void* object, uint32_t slot, T hook, T* out_original) noexcept
        : object_(object), slot_(slot)
    {
        active_ = patch<T>(object, slot, hook, &original_);
        if (out_original) *out_original = original_;
    }

    ~ScopedPatch() noexcept { reset(); }

    ScopedPatch(ScopedPatch&& o) noexcept
        : object_(o.object_), slot_(o.slot_),
          original_(o.original_), active_(o.active_)
    {
        o.active_ = false;
    }

    ScopedPatch& operator=(ScopedPatch&& o) noexcept {
        reset();
        object_   = o.object_;
        slot_     = o.slot_;
        original_ = o.original_;
        active_   = o.active_;
        o.active_ = false;
        return *this;
    }

    ScopedPatch(const ScopedPatch&)            = delete;
    ScopedPatch& operator=(const ScopedPatch&) = delete;

    /// The original function pointer; valid after a successful construction.
    [[nodiscard]] T original() const noexcept { return original_; }

    /// Returns true if the patch is currently applied.
    [[nodiscard]] bool active() const noexcept { return active_; }

    /// Manually remove the patch (also called by the destructor).
    void reset() noexcept {
        if (active_) {
            restore<T>(object_, slot_, original_);
            active_ = false;
        }
    }

private:
    void*    object_   = nullptr;
    uint32_t slot_     = 0;
    T        original_ = nullptr;
    bool     active_   = false;
};

} // namespace vangfx::vtable
