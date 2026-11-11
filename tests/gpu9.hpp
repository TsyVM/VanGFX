#pragma once

/**
 * tests/gpu9.hpp — a small D3D9 "game": a device (plain or Ex) on a hidden
 * window, a full-screen triangle drawn with shader model 3 or 2 or through the
 * fixed-function pipeline, render targets, and pixel readback.
 *
 * Direct3D 9 has no WARP: these tests always run on the hardware adapter.
 * Pixels read as A8R8G8B8, 0xAARRGGBB.
 */

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <string>

namespace vgx {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kRed9   = 0xFFFF0000;
constexpr uint32_t kGreen9 = 0xFF00FF00;
constexpr uint32_t kBlue9  = 0xFF0000FF;

inline ComPtr<ID3DBlob> compile9(const std::string& src, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &err);
    if (!code) fail(__FILE__, __LINE__, std::string("test shader: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
    return code;
}

inline HWND hidden_window9() {
    static const wchar_t* kClass = L"VanGFX.Tests9";
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    return CreateWindowExW(0, kClass, L"vangfx test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

struct Dx9 {
    HWND                               hwnd = nullptr;
    ComPtr<IDirect3D9Ex>               d3d_ex;
    ComPtr<IDirect3D9>                 d3d;
    ComPtr<IDirect3DDevice9>           dev;
    ComPtr<IDirect3DDevice9Ex>         dev_ex;
    D3DPRESENT_PARAMETERS              pp{};
    ComPtr<IDirect3DVertexShader9>     vs3, vs2;
    ComPtr<IDirect3DVertexDeclaration9> decl;
    ComPtr<IDirect3DVertexBuffer9>     tri;   // two copies of one full-screen triangle

    explicit Dx9(bool ex = false, UINT w = 8, UINT h = 8) {
        hwnd = hidden_window9();
        pp.Windowed         = TRUE;
        pp.SwapEffect       = D3DSWAPEFFECT_COPY;   // the back buffer keeps what was presented
        pp.hDeviceWindow    = hwnd;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth  = w;
        pp.BackBufferHeight = h;
        pp.BackBufferCount  = 1;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        const DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
        if (ex) {
            if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d_ex)) ||
                FAILED(d3d_ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags, &pp, nullptr, &dev_ex)))
                fail(__FILE__, __LINE__, "no D3D9Ex device");
            dev = dev_ex;
        } else {
            d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
            if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, flags, &pp, &dev)))
                fail(__FILE__, __LINE__, "no D3D9 device");
        }
        const std::string pass = "float4 main(float4 p : POSITION) : POSITION { return p; }";
        auto b3 = compile9(pass, "vs_3_0");
        auto b2 = compile9(pass, "vs_2_0");
        dev->CreateVertexShader(static_cast<const DWORD*>(b3->GetBufferPointer()), &vs3);
        dev->CreateVertexShader(static_cast<const DWORD*>(b2->GetBufferPointer()), &vs2);
        const D3DVERTEXELEMENT9 el[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
        dev->CreateVertexDeclaration(el, &decl);
        const float v[6][4] = { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 },
                                { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } };
        // Managed, so a plain device's Reset does not need it released (an Ex
        // device has no managed pool, and keeps the default one across Reset).
        dev->CreateVertexBuffer(sizeof(v), D3DUSAGE_WRITEONLY, 0, ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &tri, nullptr);
        void* p = nullptr;
        tri->Lock(0, 0, &p, 0);
        std::memcpy(p, v, sizeof(v));
        tri->Unlock();
    }

    ~Dx9() {
        tri.Reset(); decl.Reset(); vs2.Reset(); vs3.Reset();
        dev_ex.Reset(); dev.Reset(); d3d.Reset(); d3d_ex.Reset();
        if (hwnd) DestroyWindow(hwnd);
    }

    ComPtr<IDirect3DPixelShader9> solid(const char* rgba, const char* profile = "ps_3_0") {
        auto b = compile9(std::string("float4 main() : COLOR0 { return float4(") + rgba + "); }", profile);
        ComPtr<IDirect3DPixelShader9> ps;
        dev->CreatePixelShader(static_cast<const DWORD*>(b->GetBufferPointer()), &ps);
        return ps;
    }

    ComPtr<IDirect3DSurface9> target(UINT w, UINT h) {
        ComPtr<IDirect3DSurface9> s;
        dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s, nullptr);
        return s;
    }

    ComPtr<IDirect3DSurface9> back_buffer() {
        ComPtr<IDirect3DSurface9> s;
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s);
        return s;
    }

    // One full-screen draw of `vertices` vertices (3 covers the target; 6 is two
    // copies of the same triangle) into `rt`: with `vs`, or with the
    // fixed-function pipeline (pre-transformed vertices) when `vs` is null.
    void draw(IDirect3DSurface9* rt, IDirect3DPixelShader9* ps, UINT vertices = 3, IDirect3DVertexShader9* vs = nullptr,
              bool fixed_function = false) {
        dev->BeginScene();
        dev->SetRenderTarget(0, rt);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetPixelShader(ps);
        if (fixed_function) {
            D3DSURFACE_DESC d{};
            rt->GetDesc(&d);
            const float W = float(d.Width) * 2, H = float(d.Height) * 2;
            struct V { float x, y, z, rhw; } v[6] = { { 0, 0, 0, 1 }, { W, 0, 0, 1 }, { 0, H, 0, 1 },
                                                      { 0, 0, 0, 1 }, { W, 0, 0, 1 }, { 0, H, 0, 1 } };
            dev->SetVertexShader(nullptr);
            dev->SetFVF(D3DFVF_XYZRHW);
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, vertices / 3, v, sizeof(V));
        } else {
            dev->SetVertexShader(vs ? vs : vs3.Get());
            dev->SetVertexDeclaration(decl.Get());
            dev->SetStreamSource(0, tri.Get(), 0, 16);
            dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, vertices / 3);
        }
        dev->EndScene();
    }

    uint32_t read_pixel(IDirect3DSurface9* rt, UINT x = 0, UINT y = 0) {
        D3DSURFACE_DESC d{};
        rt->GetDesc(&d);
        ComPtr<IDirect3DSurface9> sys;
        dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        if (FAILED(dev->GetRenderTargetData(rt, sys.Get()))) fail(__FILE__, __LINE__, "GetRenderTargetData failed");
        D3DLOCKED_RECT lr{};
        sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        uint32_t px = 0;
        std::memcpy(&px, static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4, 4);
        sys->UnlockRect();
        return px | (d.Format == D3DFMT_X8R8G8B8 ? 0xFF000000u : 0u);
    }
};

} // namespace vgx
