#pragma once

/**
 * vangfx/backend/dxgi/present.hpp — shared DXGI swap chain front end.
 *
 * D3D10, D3D11, and D3D12 all present through IDXGISwapChain.  This layer
 * owns the vtable hook sites once and dispatches to any registered backend
 * callbacks, so the swap chain vtable is patched exactly once regardless of
 * how many backends are active.
 *
 * Usage:
 *   // Backend initialisation (called once from each backend's init):
 *   dxgi::register_backend({ .on_present = my_present, .on_resize = my_resize });
 *
 *   // Hook installation (called once after the swap chain's vtable is known):
 *   dxgi::install_present_hooks(swap_chain);
 *
 *   // Teardown:
 *   dxgi::uninstall_present_hooks();
 *   dxgi::clear_backends();
 */

#include <cstdint>
#include <dxgi1_2.h>

namespace vangfx::backend::dxgi {

// A backend registers these callbacks to hear about every present/resize.
// Callbacks are called with the internal (original) swap chain pointer.
// They must be non-null to be invoked; null means "not interested".
struct BackendCallbacks {
    void (*on_present )(IDXGISwapChain*, UINT sync, UINT flags)                                        = nullptr;
    void (*on_present1)(IDXGISwapChain1*, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*)       = nullptr;
    void (*on_resize  )(IDXGISwapChain*, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)         = nullptr;
};

// Register a backend.  At most kMaxBackends (4) may be registered.
// Thread-safe: backends must be registered before any present hook fires.
bool register_backend(BackendCallbacks cb) noexcept;
void clear_backends()                      noexcept;

// Install (or verify already-installed) Present / Present1 / ResizeBuffers
// hooks on the vtable class of swap_chain.  Safe to call multiple times from
// different backends — only the first call patches the vtable.
bool install_present_hooks(IDXGISwapChain* swap_chain) noexcept;

// Create a minimal WARP swap chain, install hooks from it, then destroy it.
// Can be called from D3D10 / D3D11 / D3D12 init before the game's swap chain
// is created.  Needs a D3D11 device pointer (or a D3D12 command queue — pass
// any IUnknown and we'll QI for the right type).
bool prime_present_hooks(IUnknown* device_or_queue) noexcept;

void uninstall_present_hooks() noexcept;

// Number of Present calls observed since the last call to reset_present_count.
// Intended for unit tests only.
uint32_t present_count()       noexcept;
void     reset_present_count() noexcept;

static constexpr int kMaxBackends = 4;

} // namespace vangfx::backend::dxgi
