#pragma once

/**
 * compat/hook_site.hpp — one hooked COM method, installed once per vtable.
 *
 * A COM object's first field points at its class's vtable, and every object of
 * that class shares it: every ID3D12GraphicsCommandList a process creates uses
 * the same table in D3D12Core.dll, whether it came from a hardware device or
 * WARP. Patching "a command list" therefore patches all of them, and has to be
 * done once, with the original saved once. The old per-object ScopedPatch did
 * neither: patching a second object read VanGFX's own hook back as "the
 * original", and restoring one object's patch unhooked every object sharing the
 * table (through a pointer to an object that might already be freed).
 *
 * A Site is the hook for one method. It remembers, per vtable, what the slot held
 * before it was patched, and a thunk asks it for that — `original(self)` — rather
 * than ever reading the slot, which holds the thunk itself:
 *
 *     static hooks::Site s_draw{ slots::DrawInstanced, "DrawInstanced" };
 *
 *     static void STDMETHODCALLTYPE hook_draw(ID3D12GraphicsCommandList* self, ...) {
 *         ...
 *         s_draw.original<DrawFn>(self)(self, ...);
 *     }
 *
 *     s_draw.install(list, &hook_draw);   // any object of the class; idempotent
 *
 * RUNTIMES THAT REWRITE THEIR OWN TABLES
 *
 * D3D11's immediate context keeps its vtable on the heap and rewrites entries as
 * it switches internal code paths: Flush (and so Present) puts a different Draw
 * into the slot, and that Draw puts the first one back. A patch made once is gone
 * after the first frame. A Site created with Heal::SameModule repairs itself —
 * `heal(object)`, called after chaining to the original, notices the slot holds
 * something else, and if that something lives in the same module as the original
 * (the runtime swapping between its own functions) adopts it as the original and
 * patches again. A value from another module is another tool's hook stacked on
 * top of ours; that is left alone, because it chains to us already.
 *
 * UNINSTALL
 *
 * `uninstall()` restores each vtable whose slot still holds our thunk. A slot that
 * holds something else has a hook stacked on ours that calls our thunk as its
 * original; putting the old value back would cut that hook off, so the slot is
 * left as it is and our thunk keeps passing calls through.
 *
 * Thread safety: install/heal/uninstall serialise on one lock; original() is
 * lock-free (entries are append-only and published with release ordering).
 */

#include <atomic>
#include <cstdint>

namespace vangfx::hooks {

enum class Heal : uint8_t {
    Never,        // the slot is ours until uninstall
    SameModule,   // re-patch when the runtime swaps in another of its own functions
};

class Site {
public:
    constexpr Site(uint32_t slot, const char* name, Heal heal = Heal::Never) noexcept
        : slot_(slot), name_(name), heal_(heal) {}

    Site(const Site&)            = delete;
    Site& operator=(const Site&) = delete;

    // Patch the vtable of `object` at this site's slot so it calls `thunk`.
    // Once per vtable: a second object of the same class is a no-op. Returns
    // false only when the page protection could not be changed.
    bool install(const void* object, void* thunk) noexcept;
    template <typename Fn>
    bool install(const void* object, Fn thunk) noexcept {
        return install(object, reinterpret_cast<void*>(thunk));
    }

    // What `self`'s vtable held in this slot before it was patched.
    [[nodiscard]] void* original_raw(const void* self) const noexcept;
    template <typename Fn>
    [[nodiscard]] Fn original(const void* self) const noexcept {
        return reinterpret_cast<Fn>(original_raw(self));
    }

    // Heal::SameModule sites: repair the patch if the runtime replaced it.
    // Cheap when nothing changed — one load and one compare.
    void heal(const void* object) noexcept;

    // Restore every vtable whose slot still holds our thunk.
    void uninstall() noexcept;

    // The objects using `vtable` are all gone, and with them the table (D3D11
    // gives every device context a table of its own, on the heap). Its entry is
    // released for reuse, and a new table at the same address is patched afresh.
    void forget(const void* vtable) noexcept;

    // Whether `object`'s vtable is patched by this site now (not uninstalled or
    // forgotten since).
    [[nodiscard]] bool     installed_on(const void* object) const noexcept;
    [[nodiscard]] uint32_t slot() const noexcept { return slot_; }
    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] int      vtable_count() const noexcept { return count_.load(std::memory_order_acquire); }

private:
    struct Entry {
        std::atomic<void**> vtable{ nullptr };
        std::atomic<void*>  original{ nullptr };
        std::atomic<bool>   restored{ false };  // uninstall put the original back
        std::atomic<bool>   dead{ false };      // forget(): the table is gone; the entry may be reused
        void*               module = nullptr;   // module the original lives in (for Heal::SameModule)
    };
    static constexpr int kMaxVtables = 64;

    const Entry* find(void** vtable) const noexcept;

    Entry             entries_[kMaxVtables]{};
    std::atomic<int>  count_{ 0 };
    std::atomic<void*> thunk_{ nullptr };
    uint32_t          slot_;
    const char*       name_;
    Heal              heal_;
};

// The vtable an object points at.
[[nodiscard]] inline void** vtable_of(const void* object) noexcept {
    return *reinterpret_cast<void** const*>(object);
}

// The module an address lies in (null for heap or unknown memory).
[[nodiscard]] void* module_of(const void* address) noexcept;

} // namespace vangfx::hooks
