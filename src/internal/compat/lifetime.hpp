#pragma once

/**
 * compat/lifetime.hpp — being told when a D3D or DXGI object is destroyed,
 * without hooking Release.
 *
 * The old way was to patch IUnknown::Release on each object. Release is a vtable
 * slot like any other, shared by every object of the class, so the per-object
 * patch saved VanGFX's own hook as "the original" on the second object, restored
 * the real Release for everyone when the first object died, and — for any object
 * VanGFX had not seen, including ones the runtime releases internally while
 * creating new objects — fell back to calling the slot, which was the hook
 * itself. On D3D12 that overflowed the stack on the second buffer created.
 *
 * Every object type VanGFX tracks can hold a COM interface as private data:
 * ID3D12Object::SetPrivateDataInterface, ID3D11DeviceChild::SetPrivateDataInterface,
 * IDXGIObject::SetPrivateDataInterface and IDirect3DResource9::SetPrivateData with
 * D3DSPD_IUNKNOWN. The runtime holds a reference on that interface and releases
 * it when the object is destroyed. So VanGFX attaches a small sentinel object and
 * learns of the destruction when the runtime lets go of it.
 *
 * The listener runs while the object is being destroyed: it must not call into
 * the object. It receives the tag it registered with (an id) and nothing else.
 */

#include <cstdint>

struct IUnknown;

namespace vangfx::lifetime {

using Listener = void (*)(void* user, const void* object, uint64_t tag) noexcept;

// Tell `fn(user, object, tag)` once, when `object` is destroyed. One registration
// per (object, user): registering again replaces the tag. Returns false when the
// object offers no private-data slot for an interface (not a D3D9 resource, D3D11
// device child, D3D12 object or DXGI object).
bool watch(IUnknown* object, Listener fn, void* user, uint64_t tag) noexcept;

// Stop telling `user` about `object`, or about anything. The sentinel stays on
// the object (see lifetime.cpp for why) and is reused by the next watch().
void unwatch(const void* object, void* user) noexcept;
void forget(void* user) noexcept;

// Whether `object` carries a sentinel of this module.
[[nodiscard]] bool watched(const void* object) noexcept;

// Take the sentinel off every object. Runs by itself when the DLL this code is
// linked into is unloaded (not at process exit): an object outliving the DLL
// would otherwise release a sentinel whose code is gone.
void detach_all() noexcept;

// Number of objects currently watched (for tests and diagnostics).
[[nodiscard]] uint32_t watched_count() noexcept;

} // namespace vangfx::lifetime
