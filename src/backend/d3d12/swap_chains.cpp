/**
 * src/backend/d3d12/swap_chains.cpp — the DXGI factory's swap chain creation.
 *
 * A D3D12 swap chain is created on a command queue and presents on it, but
 * will not say which afterwards: GetDevice gives the device and refuses
 * ID3D12CommandQueue. Post passes have to be executed on that queue, so the
 * creation calls are hooked (every factory shares one class) and each D3D12
 * swap chain's queue recorded in the hub. Each new swap chain also has Present
 * hooked through it, which covers a class (a composition swap chain, say) the
 * priming swap chain did not share.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/dxgi_slots.hpp>

#include <dxgi1_2.h>

namespace vangfx::backend::d3d12 {

namespace fs = ::vangfx::dxgi_slots::factory;

namespace {

using CreateFn        = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using ForHwndFn       = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using ForWindowFn     = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                    IDXGIOutput*, IDXGISwapChain1**);
using ForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                     IDXGIOutput*, IDXGISwapChain1**);

hooks::Site s_create         { fs::CreateSwapChain,               "IDXGIFactory::CreateSwapChain" };
hooks::Site s_for_hwnd       { fs::CreateSwapChainForHwnd,        "IDXGIFactory2::CreateSwapChainForHwnd" };
hooks::Site s_for_window     { fs::CreateSwapChainForCoreWindow,  "IDXGIFactory2::CreateSwapChainForCoreWindow" };
hooks::Site s_for_composition{ fs::CreateSwapChainForComposition, "IDXGIFactory2::CreateSwapChainForComposition" };

void created(IUnknown* device, IUnknown* swap_chain) noexcept {
    if (!device || !swap_chain) return;
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) return;   // not D3D12
    IDXGISwapChain* sc = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc))) && sc) {
        swap_chain_created(sc, queue);
        install_present_hooks(sc);
        sc->Release();
    }
    queue->Release();   // the swap chain holds its queue
}

HRESULT STDMETHODCALLTYPE hook_create(IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    const HRESULT hr = s_create.original<CreateFn>(self)(self, device, desc, out);
    if (SUCCEEDED(hr) && out) created(device, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_for_hwnd(IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs_desc, IDXGIOutput* output,
                                        IDXGISwapChain1** out) {
    const HRESULT hr = s_for_hwnd.original<ForHwndFn>(self)(self, device, hwnd, desc, fs_desc, output, out);
    if (SUCCEEDED(hr) && out) created(device, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_for_window(IDXGIFactory2* self, IUnknown* device, IUnknown* window,
                                          const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = s_for_window.original<ForWindowFn>(self)(self, device, window, desc, output, out);
    if (SUCCEEDED(hr) && out) created(device, *out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_for_composition(IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
                                               IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT hr = s_for_composition.original<ForCompositionFn>(self)(self, device, desc, output, out);
    if (SUCCEEDED(hr) && out) created(device, *out);
    return hr;
}

} // namespace

bool install_factory_hooks() noexcept {
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return false;
    bool ok = s_create.install(factory, &hook_create);
    ok &= s_for_hwnd.install(factory, &hook_for_hwnd);
    ok &= s_for_window.install(factory, &hook_for_window);
    ok &= s_for_composition.install(factory, &hook_for_composition);
    factory->Release();
    return ok;
}

void uninstall_factory_hooks() noexcept {
    for (hooks::Site* s : { &s_create, &s_for_hwnd, &s_for_window, &s_for_composition }) s->uninstall();
}

} // namespace vangfx::backend::d3d12
