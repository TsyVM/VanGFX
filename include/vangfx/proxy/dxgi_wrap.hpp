#pragma once

/**
 * vangfx/proxy/dxgi_wrap.hpp — passthrough wrappers for IDXGIFactory1 and
 * IDXGISwapChain.
 *
 * These classes forward every vtable method to the wrapped real object.  The
 * translator subclasses these and overrides only the methods it handles.
 *
 * Both register themselves in ComWrapRegistry on construction.
 *
 * QueryInterface is overridden in each class to handle the full inheritance
 * chain (IDXGIObject, IDXGIFactory, etc.) — the base ComWrap handles only
 * IUnknown and the exact leaf interface.
 */

#include <vangfx/proxy/com_wrap.hpp>
#include <dxgi1_2.h>

namespace vangfx::proxy {

// ── IDXGIFactory1Wrap ─────────────────────────────────────────────────────────

class IDXGIFactory1Wrap : public ComWrap<IDXGIFactory1> {
public:
    explicit IDXGIFactory1Wrap(IDXGIFactory1* real) : ComWrap(real) {}

    // IUnknown — override QI to cover the full hierarchy
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown))     ||
            IsEqualIID(riid, __uuidof(IDXGIObject))  ||
            IsEqualIID(riid, __uuidof(IDXGIFactory)) ||
            IsEqualIID(riid, __uuidof(IDXGIFactory1))) {
            *pp = static_cast<IDXGIFactory1*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, pp);
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT sz, const void* d) override    { return real_->SetPrivateData(g, sz, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return real_->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* sz, void* d) override         { return real_->GetPrivateData(g, sz, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override                    { return real_->GetParent(riid, pp); }

    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT idx, IDXGIAdapter** adapter) override       { return real_->EnumAdapters(idx, adapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND hwnd, UINT flags) override         { return real_->MakeWindowAssociation(hwnd, flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* hwnd) override                     { return real_->GetWindowAssociation(hwnd); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) override { return real_->CreateSwapChain(dev, d, sc); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE mod, IDXGIAdapter** adapter) override { return real_->CreateSoftwareAdapter(mod, adapter); }

    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT idx, IDXGIAdapter1** adapter) override     { return real_->EnumAdapters1(idx, adapter); }
    BOOL    STDMETHODCALLTYPE IsCurrent() override                                           { return real_->IsCurrent(); }

protected:
    ~IDXGIFactory1Wrap() = default;
};

// ── IDXGISwapChainWrap ────────────────────────────────────────────────────────

class IDXGISwapChainWrap : public ComWrap<IDXGISwapChain> {
public:
    explicit IDXGISwapChainWrap(IDXGISwapChain* real) : ComWrap(real) {}

    // IUnknown — override QI to cover the full hierarchy
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (!pp) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown))             ||
            IsEqualIID(riid, __uuidof(IDXGIObject))          ||
            IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)) ||
            IsEqualIID(riid, __uuidof(IDXGISwapChain))) {
            *pp = static_cast<IDXGISwapChain*>(this);
            AddRef();
            return S_OK;
        }
        return real_->QueryInterface(riid, pp);
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT sz, const void* d) override    { return real_->SetPrivateData(g, sz, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return real_->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* sz, void* d) override         { return real_->GetPrivateData(g, sz, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** pp) override                    { return real_->GetParent(riid, pp); }

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override                    { return real_->GetDevice(riid, pp); }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override                       { return real_->Present(sync, flags); }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT idx, REFIID riid, void** surf) override        { return real_->GetBuffer(idx, riid, surf); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL full, IDXGIOutput* target) override   { return real_->SetFullscreenState(full, target); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* full, IDXGIOutput** target) override { return real_->GetFullscreenState(full, target); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override                     { return real_->GetDesc(d); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) override { return real_->ResizeBuffers(n, w, h, fmt, flags); }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* md) override               { return real_->ResizeTarget(md); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** out) override               { return real_->GetContainingOutput(out); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* st) override        { return real_->GetFrameStatistics(st); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* cnt) override                       { return real_->GetLastPresentCount(cnt); }

protected:
    ~IDXGISwapChainWrap() = default;
};

} // namespace vangfx::proxy
