// WARP entry point for DeviceContext12 — test/fallback use only.
// Uses IDXGIFactory4::EnumWarpAdapter instead of a hardware adapter ordinal.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <vangfx/translate/targets/d3d9_to_d3d12/core/DeviceContext12.h>
#include <vangfx/translate/d3d9/Log.h>

namespace vangfx::d3d9to12 {

using namespace vangfx::d3d9;
using Microsoft::WRL::ComPtr;

HRESULT DeviceContext12::CreateWarp(
    HWND hwnd, const D3DPRESENT_PARAMETERS& pp,
    bool multiThreaded, DeviceContext12** ppOut) noexcept
{
    if (!ppOut) return E_POINTER;
    *ppOut = nullptr;
    log::Init();

    auto* self = new (std::nothrow) DeviceContext12();
    if (!self) return E_OUTOFMEMORY;

    self->m_multiThreaded = multiThreaded;
    self->m_targetHwnd    = hwnd ? hwnd : pp.hDeviceWindow;

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(self->m_factory.GetAddressOf())))) {
        delete self; return D3DERR_NOTAVAILABLE;
    }

    ComPtr<IDXGIFactory4> fac4;
    if (FAILED(self->m_factory.As(&fac4))) {
        delete self; return D3DERR_NOTAVAILABLE;
    }

    ComPtr<IDXGIAdapter1> warpAdapter;
    if (FAILED(fac4->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.GetAddressOf())))) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    self->m_adapter = warpAdapter;

    if (FAILED(self->InitD3D12Device(self->m_adapter.Get()))) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitCommandInfra())) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitDescriptorHeaps())) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitUploadRing())) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->InitRootSignature())) {
        delete self; return D3DERR_NOTAVAILABLE;
    }

    self->ApplyWindowMode(pp);

    if (FAILED(self->InitSwapChain(self->m_targetHwnd, pp))) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->AcquireBackBuffers())) {
        delete self; return D3DERR_NOTAVAILABLE;
    }
    if (FAILED(self->OpenCommandList())) {
        delete self; return D3DERR_NOTAVAILABLE;
    }

    s_current.store(self, std::memory_order_release);
    *ppOut = self;
    return S_OK;
}

} // namespace vangfx::d3d9to12
