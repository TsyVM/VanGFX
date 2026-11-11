#pragma once

/**
 * tests/gpu12.hpp — a small D3D12 "game" for the tests to drive: a device on
 * WARP (or the hardware adapter with VGX_HW=1), a queue and fence, a pipeline
 * that fills a triangle with one colour, a render target and a readback.
 * Everything here is the application's side; VanGFX sees it through its hooks.
 */

#include "harness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <vector>

namespace vgx {

using Microsoft::WRL::ComPtr;

inline ComPtr<ID3DBlob> compile(const char* src, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", profile, 0, 0, &code, &err);
    if (!code) fail(__FILE__, __LINE__, std::string("test shader: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
    return code;
}

// A window nobody sees, for swap chains.
inline HWND hidden_window(UINT w = 64, UINT h = 64) {
    static const wchar_t* kClass = L"VanGFX.Tests";
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

struct Dx12 {
    ComPtr<IDXGIFactory4>      factory;
    ComPtr<ID3D12Device>       dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence>        fence;
    UINT64                     value = 0;
    HANDLE                     event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    Dx12() {
        CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> adapter;
        if (use_hardware()) {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                adapter->GetDesc1(&d);
                if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
                adapter.Reset();
            }
        } else {
            factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        }
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
            fail(__FILE__, __LINE__, "no D3D12 device");
        D3D12_COMMAND_QUEUE_DESC qd{};
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    }
    ~Dx12() { wait(); CloseHandle(event); }

    void wait() {
        queue->Signal(fence.Get(), ++value);
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, 10000);
        }
    }

    void execute(ID3D12GraphicsCommandList* list) {
        ID3D12CommandList* l[] = { list };
        queue->ExecuteCommandLists(1, l);
        wait();
    }

    ComPtr<ID3D12RootSignature> empty_root_signature() {
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, err;
        D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        ComPtr<ID3D12RootSignature> rs;
        dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
        return rs;
    }

    // A pipeline drawing a full-screen triangle in one colour, SM 5.
    ComPtr<ID3D12PipelineState> solid_pipeline(ID3D12RootSignature* rs, const char* rgba,
                                               DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        auto vs = compile("float4 main(uint id : SV_VertexID) : SV_Position {"
                          " float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2,-2) + float2(-1,1), 0, 1); }",
                          "vs_5_0");
        const std::string ps_src = std::string("float4 main() : SV_Target { return float4(") + rgba + "); }";
        auto ps = compile(ps_src.c_str(), "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rs;
        d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = format;
        d.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) fail(__FILE__, __LINE__, "test pipeline refused");
        return pso;
    }

    ComPtr<ID3D12Resource> buffer(UINT64 size, D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT) {
        D3D12_HEAP_PROPERTIES hp{ heap };
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto state = heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                         : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                         : D3D12_RESOURCE_STATE_COMMON;
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
            fail(__FILE__, __LINE__, "buffer creation failed");
        return r;
    }

    ComPtr<ID3D12Resource> render_target(UINT w, UINT h, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = format; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                nullptr, IID_PPV_ARGS(&r))))
            fail(__FILE__, __LINE__, "render target creation failed");
        return r;
    }

    // Reads pixel (x, y) of an RGBA8 texture that is in `state`.
    uint32_t read_pixel(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, UINT x = 0, UINT y = 0) {
        const D3D12_RESOURCE_DESC d = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto rb = buffer(total, D3D12_HEAP_TYPE_READBACK);
        ComPtr<ID3D12CommandAllocator> a;
        ComPtr<ID3D12GraphicsCommandList> l;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_COPY_SOURCE };
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        src.SubresourceIndex = 0;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) l->ResourceBarrier(1, &b);
        l->Close();
        execute(l.Get());
        void* p = nullptr;
        rb->Map(0, nullptr, &p);
        uint32_t px = 0;
        std::memcpy(&px, static_cast<const uint8_t*>(p) + y * fp.Footprint.RowPitch + x * 4, 4);
        rb->Unmap(0, nullptr);
        return px;
    }
};

// Draws one full-screen triangle into `rt` with `pso` on a fresh list.
struct Scene12 {
    Dx12&                              g;
    ComPtr<ID3D12DescriptorHeap>       rtv_heap;
    ComPtr<ID3D12CommandAllocator>     alloc;
    ComPtr<ID3D12GraphicsCommandList>  list;

    explicit Scene12(Dx12& gpu) : g(gpu) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE view(ID3D12Resource* rt, UINT slot = 0) {
        auto h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += slot * g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g.dev->CreateRenderTargetView(rt, nullptr, h);
        return h;
    }

    void draw(D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT w, UINT h, ID3D12RootSignature* rs, ID3D12PipelineState* pso,
              UINT vertices = 3) {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        list->SetPipelineState(pso);
        const float clear[4] = { 0, 0, 0, 1 };
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp{ 0, 0, float(w), float(h), 0, 1 };
        const D3D12_RECT sr{ 0, 0, LONG(w), LONG(h) };
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sr);
        list->SetGraphicsRootSignature(rs);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(vertices, 1, 0, 0);
        list->Close();
        g.execute(list.Get());
    }
};

} // namespace vgx
