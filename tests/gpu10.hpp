#pragma once

/**
 * tests/gpu10.hpp — a small D3D10 "game": a device on WARP (or the hardware
 * adapter with VGX_HW=1), a pipeline filling a full-screen triangle with one
 * colour, render targets, a swap chain on a hidden window, and pixel readback.
 */

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d10_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

namespace vgx {

using Microsoft::WRL::ComPtr;

inline ComPtr<ID3DBlob> compile10(const char* src, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &err);
    if (!code) fail(__FILE__, __LINE__, std::string("test shader: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
    return code;
}

inline HWND hidden_window10(UINT w, UINT h) {
    static const wchar_t* kClass = L"VanGFX.Tests10";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    RECT r{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, kClass, L"vangfx test10", WS_OVERLAPPEDWINDOW, 0, 0, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

struct Dx10 {
    ComPtr<ID3D10Device> dev;
    ComPtr<ID3D10VertexShader> vs;

    Dx10() {
        const D3D10_DRIVER_TYPE type = use_hardware() ? D3D10_DRIVER_TYPE_HARDWARE : D3D10_DRIVER_TYPE_WARP;
        if (FAILED(D3D10CreateDevice(nullptr, type, nullptr, 0, D3D10_SDK_VERSION, &dev)))
            fail(__FILE__, __LINE__, "no D3D10 device");
        auto b = compile10(
            "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
            "VS_OUT main(uint id : SV_VertexID) {\n"
            "    float2 p = float2((id << 1) & 2, id & 2);\n"
            "    VS_OUT o;\n"
            "    o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
            "    o.uv  = float2(p.x, 1.0 - p.y);\n"
            "    return o;\n"
            "}\n",
            "vs_4_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), &vs);
    }

    ComPtr<ID3D10PixelShader> solid(const char* rgba) {
        const std::string src = std::string("float4 main() : SV_Target { return float4(") + rgba + "); }";
        auto b = compile10(src.c_str(), "ps_4_0");
        ComPtr<ID3D10PixelShader> ps;
        dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), &ps);
        return ps;
    }

    ComPtr<ID3D10Texture2D> target(UINT w, UINT h) {
        D3D10_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D10_USAGE_DEFAULT;
        d.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
        ComPtr<ID3D10Texture2D> t;
        dev->CreateTexture2D(&d, nullptr, &t);
        return t;
    }

    void draw(ID3D10RenderTargetView* rtv, UINT w, UINT h, ID3D10PixelShader* ps, UINT vertices = 3) {
        const float clear[4] = { 0, 0, 0, 1 };
        dev->ClearRenderTargetView(rtv, clear);
        dev->OMSetRenderTargets(1, &rtv, nullptr);
        const D3D10_VIEWPORT vp{ 0, 0, w, h, 0.0f, 1.0f };
        dev->RSSetViewports(1, &vp);
        dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dev->IASetInputLayout(nullptr);
        dev->VSSetShader(vs.Get());
        dev->PSSetShader(ps);
        dev->Draw(vertices, 0);
    }

    uint32_t read_pixel(ID3D10Texture2D* tex, UINT x = 0, UINT y = 0) {
        D3D10_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        d.Usage = D3D10_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D10_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D10Texture2D> staging;
        dev->CreateTexture2D(&d, nullptr, &staging);
        dev->CopyResource(staging.Get(), tex);
        D3D10_MAPPED_TEXTURE2D m{};
        staging->Map(0, D3D10_MAP_READ, 0, &m);
        uint32_t px = 0;
        std::memcpy(&px, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4, 4);
        staging->Unmap(0);
        return px;
    }

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
        if (FAILED(factory->CreateSwapChain(dev.Get(), &d, &sc))) fail(__FILE__, __LINE__, "D3D10 swap chain creation failed");
        return sc;
    }
};

} // namespace vgx
