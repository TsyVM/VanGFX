#pragma once

/**
 * tests/gpu11.hpp — a small D3D11 "game": a device on WARP (or the hardware
 * adapter with VGX_HW=1), a pipeline filling a full-screen triangle with one
 * colour, render targets, a swap chain on a hidden window, and pixel readback.
 */

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

namespace vgx {

using Microsoft::WRL::ComPtr;

inline ComPtr<ID3DBlob> compile11(const char* src, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &err);
    if (!code) fail(__FILE__, __LINE__, std::string("test shader: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
    return code;
}

inline HWND hidden_window11(UINT w, UINT h) {
    static const wchar_t* kClass = L"VanGFX.Tests11";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    RECT r{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, kClass, L"vangfx test", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

struct Dx11 {
    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> dc;
    ComPtr<ID3D11VertexShader>  vs;

    Dx11() {
        const D3D_DRIVER_TYPE type = use_hardware() ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        if (FAILED(D3D11CreateDevice(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &dc)))
            fail(__FILE__, __LINE__, "no D3D11 device");
        auto b = compile11("float4 main(uint id : SV_VertexID) : SV_Position {"
                           " float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }",
                           "vs_5_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
    }

    ComPtr<ID3D11PixelShader> solid(const char* rgba) {
        const std::string src = std::string("float4 main() : SV_Target { return float4(") + rgba + "); }";
        auto b = compile11(src.c_str(), "ps_5_0");
        ComPtr<ID3D11PixelShader> ps;
        dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps);
        return ps;
    }

    ComPtr<ID3D11Texture2D> target(UINT w, UINT h) {
        D3D11_TEXTURE2D_DESC d{ w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        ComPtr<ID3D11Texture2D> t;
        dev->CreateTexture2D(&d, nullptr, &t);
        return t;
    }

    // One full-screen draw of `vertices` vertices (3 covers the screen; 6 is two
    // copies of the same triangle) into `rtv`, on `context` (the immediate one
    // by default).
    void draw(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11PixelShader* ps, UINT vertices = 3,
              ID3D11DeviceContext* context = nullptr) {
        ID3D11DeviceContext* c = context ? context : dc.Get();
        const float clear[4] = { 0, 0, 0, 1 };
        c->ClearRenderTargetView(rtv, clear);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        const D3D11_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        c->RSSetViewports(1, &vp);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->Draw(vertices, 0);
    }

    uint32_t read_pixel(ID3D11Texture2D* tex, UINT x = 0, UINT y = 0) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        dev->CreateTexture2D(&d, nullptr, &staging);
        dc->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        dc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
        uint32_t px = 0;
        std::memcpy(&px, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4, 4);
        dc->Unmap(staging.Get(), 0);
        return px;
    }

    // A swap chain whose back buffer keeps its contents after Present
    // (DXGI_SWAP_EFFECT_SEQUENTIAL), so a test can read what was presented.
    ComPtr<IDXGISwapChain> swap_chain(HWND hwnd, UINT w, UINT h) {
        ComPtr<IDXGIDevice> dxgi;
        dev.As(&dxgi);
        ComPtr<IDXGIAdapter> adapter;
        dxgi->GetAdapter(&adapter);
        ComPtr<IDXGIFactory> factory;
        adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC d{};
        d.BufferDesc.Width = w; d.BufferDesc.Height = h; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount  = 1;
        d.OutputWindow = hwnd;
        d.Windowed     = TRUE;
        d.SwapEffect   = DXGI_SWAP_EFFECT_SEQUENTIAL;
        ComPtr<IDXGISwapChain> sc;
        if (FAILED(factory->CreateSwapChain(dev.Get(), &d, &sc))) fail(__FILE__, __LINE__, "swap chain creation failed");
        return sc;
    }
};

} // namespace vgx
