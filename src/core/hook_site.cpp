/**
 * src/core/hook_site.cpp — see compat/hook_site.hpp.
 */

#include <compat/hook_site.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vangfx::hooks {

namespace {

// One lock for every site: patching is rare (once per vtable, plus repairs), and
// a single lock means two sites never interleave writes to the same page.
SRWLOCK g_lock = SRWLOCK_INIT;

struct Exclusive {
    Exclusive()  noexcept { AcquireSRWLockExclusive(&g_lock); }
    ~Exclusive() noexcept { ReleaseSRWLockExclusive(&g_lock); }
};

// Write one pointer into a vtable. The table normally lives in a read-only data
// section, so the page is made writable for the store and put back afterwards.
// The store itself is a single aligned pointer write, which other threads calling
// through the table see either before or after, never torn.
bool write_slot(void** where, void* value) noexcept {
    DWORD old = 0;
    if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) {
        if (!VirtualProtect(where, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
            return false;
    }
    InterlockedExchangePointer(where, value);
    DWORD ignored = 0;
    VirtualProtect(where, sizeof(void*), old, &ignored);
    return true;
}

} // namespace

void* module_of(const void* address) noexcept {
    HMODULE m = nullptr;
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &m))
        return nullptr;
    return m;
}

const Site::Entry* Site::find(void** vtable) const noexcept {
    const int n = count_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (entries_[i].vtable.load(std::memory_order_acquire) == vtable)
            return &entries_[i];
    return nullptr;
}

bool Site::install(const void* object, void* thunk) noexcept {
    if (!object || !thunk) return false;
    void** vt = vtable_of(object);
    if (!vt) return false;
    if (const Entry* e = find(vt); e && !e->restored.load(std::memory_order_acquire))
        return true;

    Exclusive lock;
    thunk_.store(thunk, std::memory_order_relaxed);
    void* current = vt[slot_];

    if (auto* e = const_cast<Entry*>(find(vt))) {
        if (!e->restored.load(std::memory_order_relaxed)) return true;
        // Uninstalled earlier and now wanted again. Whatever the slot holds now is
        // what to chain to: the original we put back, or a hook installed since.
        if (current != thunk) {
            e->original.store(current, std::memory_order_release);
            e->module = module_of(current);
        }
        e->dead.store(false, std::memory_order_relaxed);
        e->restored.store(false, std::memory_order_release);
        return current == thunk || write_slot(&vt[slot_], thunk);
    }

    // Our thunk with no record of what it replaced would have nothing to chain
    // to; refuse rather than install a dead end.
    if (current == thunk) return false;
    const int n = count_.load(std::memory_order_relaxed);
    if (n < kMaxVtables) {
        Entry& e = entries_[n];
        e.original.store(current, std::memory_order_relaxed);
        e.module = module_of(current);
        e.restored.store(false, std::memory_order_relaxed);
        e.vtable.store(vt, std::memory_order_relaxed);
        // Publish the entry before the thunk becomes reachable, so a call arriving
        // through the patched slot always finds its original.
        count_.store(n + 1, std::memory_order_release);
        return write_slot(&vt[slot_], thunk);
    }

    // Full: reuse the entry of a table whose objects are gone. Nothing calls
    // through a dead table, so only the order of these stores matters: the
    // vtable last, with release, so a reader that finds it sees the original.
    for (int i = 0; i < n; ++i) {
        Entry& e = entries_[i];
        if (!e.dead.load(std::memory_order_relaxed)) continue;
        e.original.store(current, std::memory_order_relaxed);
        e.module = module_of(current);
        e.restored.store(false, std::memory_order_relaxed);
        e.dead.store(false, std::memory_order_relaxed);
        e.vtable.store(vt, std::memory_order_release);
        return write_slot(&vt[slot_], thunk);
    }
    return false;
}

void Site::forget(const void* vtable) noexcept {
    Exclusive lock;
    if (auto* e = const_cast<Entry*>(find(static_cast<void**>(const_cast<void*>(vtable))))) {
        e->restored.store(true, std::memory_order_relaxed);
        e->dead.store(true, std::memory_order_release);
    }
}

void* Site::original_raw(const void* self) const noexcept {
    if (self) {
        if (const Entry* e = find(vtable_of(self)))
            return e->original.load(std::memory_order_acquire);
    }
    // A call can only reach a thunk through a vtable it was installed in, so a
    // miss means the object's vtable pointer was swapped after the call began
    // (some runtimes do this for debug layers). The first table's original is
    // the same method of the same class, which is the best available answer.
    if (count_.load(std::memory_order_acquire) > 0)
        return entries_[0].original.load(std::memory_order_acquire);
    return nullptr;
}

bool Site::installed_on(const void* object) const noexcept {
    if (!object) return false;
    const Entry* e = find(vtable_of(object));
    return e && !e->restored.load(std::memory_order_acquire);
}

void Site::heal(const void* object) noexcept {
    if (heal_ != Heal::SameModule || !object) return;
    void** vt = vtable_of(object);
    void* thunk = thunk_.load(std::memory_order_relaxed);
    if (!vt || vt[slot_] == thunk) return;

    Exclusive lock;
    auto* e = const_cast<Entry*>(find(vt));
    // Not a table of ours, or one uninstalled (or forgotten) since: a thunk
    // still running when uninstall() put the original back must not patch
    // the table again on its way out.
    if (!e || e->restored.load(std::memory_order_relaxed)) return;
    void* now = vt[slot_];
    if (now == thunk || now == nullptr) return;
    // Adopt only what is certainly the runtime's own: a function of the module
    // the original came from. Not when that is unknown (the original was in
    // memory no module owns, like another tool's trampoline), and never a
    // function of the module this code is in -- that can be a second hook on
    // the slot, and adopting it would have the two call each other forever.
    const void* runtime = e->module;
    if (!runtime || runtime == module_of(thunk)) return;
    if (module_of(now) != runtime) return;   // another tool's hook on top of ours
    e->original.store(now, std::memory_order_release);
    write_slot(&vt[slot_], thunk);
}

void Site::uninstall() noexcept {
    Exclusive lock;
    void* thunk = thunk_.load(std::memory_order_relaxed);
    const int n = count_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        Entry& e = entries_[i];
        if (e.restored.load(std::memory_order_relaxed)) continue;
        void** vt = e.vtable.load(std::memory_order_relaxed);
        // Only a slot that still holds our thunk goes back. One holding another
        // hook has that hook calling our thunk as its original: the entry stays
        // live so the thunk can keep passing calls through.
        if (vt && vt[slot_] == thunk &&
            write_slot(&vt[slot_], e.original.load(std::memory_order_relaxed)))
            e.restored.store(true, std::memory_order_release);
    }
}

} // namespace vangfx::hooks
