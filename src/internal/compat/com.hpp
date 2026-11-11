#pragma once

/**
 * vangfx/compat/com.hpp — Lightweight COM helpers for use inside VanGFX hooks.
 *
 * The Windows Runtime / ATL ComPtr<T> is excellent for application code but
 * pulls in a large dependency tree and may conflict with game headers.  This
 * header provides a minimal, self-contained alternative:
 *
 *   vangfx::com::Ptr<T>         — RAII COM smart pointer (AddRef / Release).
 *   vangfx::com::qi<T>()        — Safe QueryInterface wrapper.
 *   vangfx::com::iid_of<T>()    — Get __uuidof(T) as a GUID without COM headers.
 *   vangfx::com::equal_iid()    — Compare two GUIDs without linking Ole32.
 *   vangfx::com::is_same_object()— QueryInterface to IUnknown and compare ptrs.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  USAGE
 * ─────────────────────────────────────────────────────────────────────────
 *
 *  // Upcast: obtain ID3D12Device5 from an ID3D12Device pointer
 *  auto dev5 = vangfx::com::qi<ID3D12Device5>(device);
 *  if (dev5) {
 *      dev5->CreateStateObject(...);
 *  } // dev5 released here
 *
 *  // Check if two pointers refer to the same COM object
 *  bool same = vangfx::com::is_same_object(ptr_a, ptr_b);
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  IMPORTANT: No WRL / ATL dependency.  Only <unknwn.h> is required for
 *  IUnknown.  Include Windows SDK headers before this header.
 * ─────────────────────────────────────────────────────────────────────────
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>  // IUnknown

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace vangfx::com {

// ─────────────────────────────────────────────────────────────────────────────
//  Ptr<T> — minimal RAII COM smart pointer
//
//  Semantically equivalent to ATL::CComPtr<T> / WRL::ComPtr<T> but with no
//  external dependencies.  Move-only; copies must be explicit via copy().
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
class Ptr {
public:
    Ptr() noexcept = default;

    /// Adopts a raw pointer WITHOUT calling AddRef.
    /// Use this when you receive a pointer from CreateXxx() / GetXxx() that
    /// already holds a reference for the caller (the COM convention).
    explicit Ptr(T* p) noexcept : p_(p) {}

    ~Ptr() noexcept { reset(); }

    Ptr(const Ptr&)            = delete;
    Ptr& operator=(const Ptr&) = delete;

    Ptr(Ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Ptr& operator=(Ptr&& o) noexcept {
        reset();
        p_ = o.p_;
        o.p_ = nullptr;
        return *this;
    }

    // Explicit copy: increments the reference count.
    [[nodiscard]] Ptr copy() const noexcept {
        if (p_) p_->AddRef();
        return Ptr(p_);
    }

    void reset() noexcept {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    [[nodiscard]] T*  get()     const noexcept { return p_; }
    [[nodiscard]] T*  operator->() const noexcept { return p_; }
    [[nodiscard]] T&  operator*()  const noexcept { return *p_; }
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }

    /// Returns the internal pointer and clears the smart pointer WITHOUT
    /// calling Release.  The caller is responsible for the reference.
    [[nodiscard]] T* detach() noexcept {
        T* p = p_;
        p_ = nullptr;
        return p;
    }

    /// Returns a pointer to the internal pointer, for use with factory
    /// functions that write through T**.  Asserts that the pointer is null
    /// (i.e. the Ptr is empty) before overwriting.
    [[nodiscard]] T** put() noexcept {
        // reset() first to avoid leaking an existing reference
        reset();
        return &p_;
    }

    /// Reinterpret the internal pointer as void**, as needed for QueryInterface.
    [[nodiscard]] void** put_void() noexcept {
        return reinterpret_cast<void**>(put());
    }

private:
    T* p_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
//  qi<T> — safe QueryInterface
//
//  Returns a Ptr<T> containing the requested interface, or an empty Ptr if
//  the object does not implement it.  Does NOT throw.
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename U>
[[nodiscard]] Ptr<T> qi(U* object) noexcept {
    if (!object) return {};
    Ptr<T> result;
    object->QueryInterface(__uuidof(T), result.put_void());
    return result;
}

template<typename T, typename U>
[[nodiscard]] Ptr<T> qi(const Ptr<U>& object) noexcept {
    return qi<T>(object.get());
}

// ─────────────────────────────────────────────────────────────────────────────
//  equal_iid — compare two GUIDs without Ole32's IsEqualGUID / linking Ole32
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline bool equal_iid(const IID& a, const IID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(IID)) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  is_same_object — check COM identity (IUnknown pointer equality)
//
//  Two COM pointers refer to the same object if and only if they return the
//  same IUnknown* from QueryInterface.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline bool is_same_object(IUnknown* a, IUnknown* b) noexcept {
    if (a == b) return true;
    if (!a || !b) return false;
    auto ua = qi<IUnknown>(a);
    auto ub = qi<IUnknown>(b);
    return ua.get() == ub.get();
}

// ─────────────────────────────────────────────────────────────────────────────
//  AddRef / Release helpers — for raw pointer management in hook trampolines
//
//  These are thin wrappers so hook code doesn't need to mix raw COM calls with
//  smart-pointer management.
// ─────────────────────────────────────────────────────────────────────────────

inline void add_ref(IUnknown* p) noexcept {
    if (p) p->AddRef();
}

inline void release(IUnknown* p) noexcept {
    if (p) p->Release();
}

// ─────────────────────────────────────────────────────────────────────────────
//  device_from_context — convenience: get the ID3D12Device from any
//  ID3D12DeviceChild (command list, command queue, heap, fence, etc.)
//
//  Returns an empty Ptr if the child or the device pointer is null.
// ─────────────────────────────────────────────────────────────────────────────

template<typename DeviceT, typename ChildT>
[[nodiscard]] Ptr<DeviceT> device_from_child(ChildT* child) noexcept {
    if (!child) return {};
    Ptr<DeviceT> dev;
    child->GetDevice(__uuidof(DeviceT), dev.put_void());
    return dev;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ScopedKeepAlive — AddRef on construction, Release on destruction.
//
//  Use this inside hook implementations to ensure a COM object stays alive
//  for the duration of a callback even if the game releases it concurrently.
// ─────────────────────────────────────────────────────────────────────────────

class ScopedKeepAlive {
public:
    explicit ScopedKeepAlive(IUnknown* p) noexcept : p_(p) { add_ref(p_); }
    ~ScopedKeepAlive() noexcept { release(p_); }

    ScopedKeepAlive(const ScopedKeepAlive&)            = delete;
    ScopedKeepAlive& operator=(const ScopedKeepAlive&) = delete;

private:
    IUnknown* p_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  hr_ok — HRESULT success check; avoids including winerror.h just for SUCCEEDED
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline bool hr_ok(HRESULT hr) noexcept {
    return hr >= 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Private data key — VanGFX stores per-object metadata using the COM
//  SetPrivateDataInterface mechanism.  The GUID below is VanGFX-specific;
//  it will never collide with D3D runtime or debug layer GUIDs.
//
//  {7A4E91B3-2C1D-4F8A-9E6D-0B3C5A72F810}
// ─────────────────────────────────────────────────────────────────────────────

// {7A4E91B3-2C1D-4F8A-9E6D-0B3C5A72F810}
inline constexpr IID kVanGFXPrivateDataGUID = {
    0x7A4E91B3, 0x2C1D, 0x4F8A,
    { 0x9E, 0x6D, 0x0B, 0x3C, 0x5A, 0x72, 0xF8, 0x10 }
};

} // namespace vangfx::com
