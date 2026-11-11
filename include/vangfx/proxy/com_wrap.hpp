#pragma once

/**
 * vangfx/proxy/com_wrap.hpp — COM passthrough wrapper for translating proxies
 *
 * ComWrap<I> implements interface I by forwarding every call to a "real" I*
 * (the system device, or the next proxy in a chain).  The wrapper is the object
 * the game sees; the real pointer is what the driver / translator sees.
 *
 * Usage:
 *   // Wrap a real IDirect3D9 obtained from the system DLL
 *   auto* wrapped = ComWrap<IDirect3D9>::create(real_d3d);
 *
 *   // Later, unwrap back to the real pointer:
 *   IDirect3D9* real = ComWrapRegistry::unwrap<IDirect3D9>(wrapped_ptr);
 *
 * The wrap/unwrap registry is global and lock-protected.  It maps wrapped
 * pointers to their real counterparts.  A wrapper destroyed via Release() with
 * refcount 0 removes itself from the registry automatically.
 *
 * ComWrap<I> is meant to be subclassed: the translator overrides the methods it
 * handles and lets the base forward the rest.  Any call not overridden reaches
 * the real object without a double-dispatch cost.
 */

#include <cstdint>
#include <unknwn.h>

namespace vangfx::proxy {

// ── ComWrapRegistry ───────────────────────────────────────────────────────────
//
// Global table: wrapped IUnknown* → real IUnknown*
// All operations are thread-safe.

class ComWrapRegistry {
public:
    // Record that wrapped wraps real (both AddRef'd by the caller).
    static void register_wrap  (IUnknown* wrapped, IUnknown* real);
    // Remove the entry for wrapped (called from the wrapper's destructor).
    static void unregister_wrap(IUnknown* wrapped);

    // Return the real pointer for wrapped, or nullptr if not in the table.
    static IUnknown* real_of(IUnknown* wrapped);

    // Return the wrapped pointer for real, or nullptr if not in the table.
    static IUnknown* wrap_of(IUnknown* real);

    // Typed helpers.
    template<typename I>
    static I* unwrap(I* wrapped) {
        return static_cast<I*>(real_of(static_cast<IUnknown*>(wrapped)));
    }
    template<typename I>
    static I* wrapped_for(I* real) {
        return static_cast<I*>(wrap_of(static_cast<IUnknown*>(real)));
    }
};

// ── ComWrap<I> base ───────────────────────────────────────────────────────────
//
// A minimal base that implements IUnknown (QueryInterface, AddRef, Release)
// and stores a real* for subclasses to forward through.
//
// Subclasses should:
//   1. Inherit ComWrap<IDirect3DDevice9> (not IDirect3DDevice9 directly).
//   2. Call ComWrap<I>(real) in their constructor.
//   3. Override only the methods that need translation; leave the rest to
//      forward through the base's default implementations (write them in
//      a generated passthrough wrapper, then override selectively).
//
// The wrapper does NOT AddRef the real pointer on construction; it adopts
// whatever refcount the caller intends.  The real pointer is Released when
// the wrapper's refcount reaches 0.

template<typename I>
class ComWrap : public I {
public:
    explicit ComWrap(I* real) noexcept : real_(real) {
        ComWrapRegistry::register_wrap(this, static_cast<IUnknown*>(real_));
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(I))) {
            *pp = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        // Let the real object answer for interfaces we don't wrap.
        HRESULT hr = real_->QueryInterface(riid, pp);
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&refcount_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = InterlockedDecrement(&refcount_);
        if (n == 0) {
            ComWrapRegistry::unregister_wrap(static_cast<IUnknown*>(this));
            if (real_) { real_->Release(); real_ = nullptr; }
            delete this;
        }
        return n;
    }

protected:
    virtual ~ComWrap() = default;  // virtual so delete-this in Release() dispatches to the subclass

    I* real_ = nullptr;

private:
    volatile ULONG refcount_ = 1;
};

} // namespace vangfx::proxy
