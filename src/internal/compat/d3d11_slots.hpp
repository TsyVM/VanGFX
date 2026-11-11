#pragma once

/**
 * vangfx/compat/d3d11_slots.hpp — vtable slot indices for D3D11 COM interfaces.
 *
 * Covers the three interfaces VanGFX hooks on the D3D11 backend:
 *   • ID3D11DeviceContext   — draw calls, shader state
 *   • IDXGISwapChain        — Present for frame interception
 *   • ID3D11Device          — resource creation
 *
 * Slot numbering:
 *   IUnknown        slots  0-2   (QueryInterface / AddRef / Release)
 *   ID3D11DeviceChild  slot  3   (GetDevice, then SetPrivateData etc.)
 *   Interface methods follow in IDL declaration order.
 *
 * Source: d3d11.h and dxgi.h in Windows SDK 10.0.20348.0.
 * The D3D11 vtable layout is stable since Windows 7 / DirectX 11.0.
 */

#include <cstdint>

namespace vangfx::d3d11_slots {

// ─────────────────────────────────────────────────────────────────────────────
//  IDXGISwapChain
//  Inherits: IUnknown (0-2) → IDXGIObject (3-6) → IDXGIDeviceSubObject (7)
//            → IDXGISwapChain (8+)
// ─────────────────────────────────────────────────────────────────────────────

namespace swap_chain {
    // IUnknown
    inline constexpr uint32_t QueryInterface     = 0;
    inline constexpr uint32_t AddRef             = 1;
    inline constexpr uint32_t Release            = 2;

    // IDXGIObject
    inline constexpr uint32_t SetPrivateData         = 3;
    inline constexpr uint32_t SetPrivateDataInterface= 4;
    inline constexpr uint32_t GetPrivateData         = 5;
    inline constexpr uint32_t GetParent              = 6;

    // IDXGIDeviceSubObject
    inline constexpr uint32_t GetDevice              = 7;

    // IDXGISwapChain
    inline constexpr uint32_t Present                = 8;   // ← VanGFX frame hook
    inline constexpr uint32_t GetBuffer              = 9;
    inline constexpr uint32_t SetFullscreenState     = 10;
    inline constexpr uint32_t GetFullscreenState     = 11;
    inline constexpr uint32_t GetDesc                = 12;
    inline constexpr uint32_t ResizeBuffers          = 13;  // ← device-reset hook
    inline constexpr uint32_t ResizeTarget           = 14;
    inline constexpr uint32_t GetContainingOutput    = 15;
    inline constexpr uint32_t GetFrameStatistics     = 16;
    inline constexpr uint32_t GetLastPresentCount    = 17;

    inline constexpr uint32_t kSlotCount             = 18;
} // namespace swap_chain

// ─────────────────────────────────────────────────────────────────────────────
//  IDXGISwapChain1  (DXGI 1.2 — extends IDXGISwapChain)
// ─────────────────────────────────────────────────────────────────────────────

namespace swap_chain1 {
    // Inherits all IDXGISwapChain slots (0-17), then adds:
    inline constexpr uint32_t GetDesc1              = 18;
    inline constexpr uint32_t GetFullscreenDesc     = 19;
    inline constexpr uint32_t GetHwnd               = 20;
    inline constexpr uint32_t GetCoreWindow         = 21;
    inline constexpr uint32_t Present1              = 22;  // ← preferred Present hook (DXGI 1.2+)
    inline constexpr uint32_t IsTemporaryMonoSupported = 23;
    inline constexpr uint32_t GetRestrictToOutput   = 24;
    inline constexpr uint32_t SetBackgroundColor    = 25;
    inline constexpr uint32_t GetBackgroundColor    = 26;
    inline constexpr uint32_t SetRotation           = 27;
    inline constexpr uint32_t GetRotation           = 28;

    inline constexpr uint32_t kSlotCount            = 29;
} // namespace swap_chain1

// ─────────────────────────────────────────────────────────────────────────────
//  ID3D11DeviceContext
//  Inherits: IUnknown (0-2) → ID3D11DeviceChild (3-6) → ID3D11DeviceContext (7+)
// ─────────────────────────────────────────────────────────────────────────────

namespace device_context {
    // IUnknown
    inline constexpr uint32_t QueryInterface     = 0;
    inline constexpr uint32_t AddRef             = 1;
    inline constexpr uint32_t Release            = 2;

    // ID3D11DeviceChild
    inline constexpr uint32_t GetDevice          = 3;
    inline constexpr uint32_t GetPrivateData     = 4;
    inline constexpr uint32_t SetPrivateData     = 5;
    inline constexpr uint32_t SetPrivateDataInterface = 6;

    // ID3D11DeviceContext — in IDL declaration order
    inline constexpr uint32_t VSSetConstantBuffers       =  7;
    inline constexpr uint32_t PSSetShaderResources       =  8;
    inline constexpr uint32_t PSSetShader                =  9;   // ← shader bind hook
    inline constexpr uint32_t PSSetSamplers              = 10;
    inline constexpr uint32_t VSSetShader                = 11;   // ← shader bind hook
    inline constexpr uint32_t DrawIndexed                = 12;   // ← draw hook
    inline constexpr uint32_t Draw                       = 13;   // ← draw hook
    inline constexpr uint32_t Map                        = 14;
    inline constexpr uint32_t Unmap                      = 15;
    inline constexpr uint32_t PSSetConstantBuffers       = 16;
    inline constexpr uint32_t IASetInputLayout           = 17;
    inline constexpr uint32_t IASetVertexBuffers         = 18;
    inline constexpr uint32_t IASetIndexBuffer           = 19;
    inline constexpr uint32_t DrawIndexedInstanced       = 20;   // ← draw hook
    inline constexpr uint32_t DrawInstanced              = 21;   // ← draw hook
    inline constexpr uint32_t GSSetConstantBuffers       = 22;
    inline constexpr uint32_t GSSetShader                = 23;
    inline constexpr uint32_t IASetPrimitiveTopology     = 24;
    inline constexpr uint32_t VSSetShaderResources       = 25;
    inline constexpr uint32_t VSSetSamplers              = 26;
    inline constexpr uint32_t Begin                      = 27;
    inline constexpr uint32_t End                        = 28;
    inline constexpr uint32_t GetData                    = 29;
    inline constexpr uint32_t SetPredication             = 30;
    inline constexpr uint32_t GSSetShaderResources       = 31;
    inline constexpr uint32_t GSSetSamplers              = 32;
    inline constexpr uint32_t OMSetRenderTargets         = 33;   // ← RT bind hook
    inline constexpr uint32_t OMSetRenderTargetsAndUnorderedAccessViews = 34;
    inline constexpr uint32_t OMSetBlendState            = 35;
    inline constexpr uint32_t OMSetDepthStencilState     = 36;
    inline constexpr uint32_t SOSetTargets               = 37;
    inline constexpr uint32_t DrawAuto                   = 38;
    inline constexpr uint32_t DrawIndexedInstancedIndirect = 39;
    inline constexpr uint32_t DrawInstancedIndirect      = 40;
    inline constexpr uint32_t Dispatch                   = 41;   // ← compute hook
    inline constexpr uint32_t DispatchIndirect           = 42;
    inline constexpr uint32_t RSSetState                 = 43;
    inline constexpr uint32_t RSSetViewports             = 44;
    inline constexpr uint32_t RSSetScissorRects          = 45;
    inline constexpr uint32_t CopySubresourceRegion      = 46;
    inline constexpr uint32_t CopyResource               = 47;
    inline constexpr uint32_t UpdateSubresource          = 48;
    inline constexpr uint32_t CopyStructureCount         = 49;
    inline constexpr uint32_t ClearRenderTargetView      = 50;
    inline constexpr uint32_t ClearUnorderedAccessViewUint = 51;
    inline constexpr uint32_t ClearUnorderedAccessViewFloat = 52;
    inline constexpr uint32_t ClearDepthStencilView      = 53;
    inline constexpr uint32_t GenerateMips               = 54;
    inline constexpr uint32_t SetResourceMinLOD          = 55;
    inline constexpr uint32_t GetResourceMinLOD          = 56;
    inline constexpr uint32_t ResolveSubresource         = 57;
    inline constexpr uint32_t ExecuteCommandList         = 58;
    inline constexpr uint32_t HSSetShaderResources       = 59;
    inline constexpr uint32_t HSSetShader                = 60;
    inline constexpr uint32_t HSSetSamplers              = 61;
    inline constexpr uint32_t HSSetConstantBuffers       = 62;
    inline constexpr uint32_t DSSetShaderResources       = 63;
    inline constexpr uint32_t DSSetShader                = 64;
    inline constexpr uint32_t DSSetSamplers              = 65;
    inline constexpr uint32_t DSSetConstantBuffers       = 66;
    inline constexpr uint32_t CSSetShaderResources       = 67;
    inline constexpr uint32_t CSSetUnorderedAccessViews  = 68;
    inline constexpr uint32_t CSSetShader                = 69;
    inline constexpr uint32_t CSSetSamplers              = 70;
    inline constexpr uint32_t CSSetConstantBuffers       = 71;
    inline constexpr uint32_t VSGetConstantBuffers       = 72;
    inline constexpr uint32_t PSGetShaderResources       = 73;
    inline constexpr uint32_t PSGetShader                = 74;
    inline constexpr uint32_t PSGetSamplers              = 75;
    inline constexpr uint32_t VSGetShader                = 76;
    inline constexpr uint32_t PSGetConstantBuffers       = 77;
    inline constexpr uint32_t IAGetInputLayout           = 78;
    inline constexpr uint32_t IAGetVertexBuffers         = 79;
    inline constexpr uint32_t IAGetIndexBuffer           = 80;
    inline constexpr uint32_t GSGetConstantBuffers       = 81;
    inline constexpr uint32_t GSGetShader                = 82;
    inline constexpr uint32_t IAGetPrimitiveTopology     = 83;
    inline constexpr uint32_t VSGetShaderResources       = 84;
    inline constexpr uint32_t VSGetSamplers              = 85;
    inline constexpr uint32_t GetPredication             = 86;
    inline constexpr uint32_t GSGetShaderResources       = 87;
    inline constexpr uint32_t GSGetSamplers              = 88;
    inline constexpr uint32_t OMGetRenderTargets         = 89;
    inline constexpr uint32_t OMGetRenderTargetsAndUnorderedAccessViews = 90;
    inline constexpr uint32_t OMGetBlendState            = 91;
    inline constexpr uint32_t OMGetDepthStencilState     = 92;
    inline constexpr uint32_t SOGetTargets               = 93;
    inline constexpr uint32_t RSGetState                 = 94;
    inline constexpr uint32_t RSGetViewports             = 95;
    inline constexpr uint32_t RSGetScissorRects          = 96;
    inline constexpr uint32_t HSGetShaderResources       = 97;
    inline constexpr uint32_t HSGetShader                = 98;
    inline constexpr uint32_t HSGetSamplers              = 99;
    inline constexpr uint32_t HSGetConstantBuffers       = 100;
    inline constexpr uint32_t DSGetShaderResources       = 101;
    inline constexpr uint32_t DSGetShader                = 102;
    inline constexpr uint32_t DSGetSamplers              = 103;
    inline constexpr uint32_t DSGetConstantBuffers       = 104;
    inline constexpr uint32_t CSGetShaderResources       = 105;
    inline constexpr uint32_t CSGetUnorderedAccessViews  = 106;
    inline constexpr uint32_t CSGetShader                = 107;
    inline constexpr uint32_t CSGetSamplers              = 108;
    inline constexpr uint32_t CSGetConstantBuffers       = 109;
    inline constexpr uint32_t ClearState                 = 110;
    inline constexpr uint32_t Flush                      = 111;
    inline constexpr uint32_t GetType                    = 112;
    inline constexpr uint32_t GetContextFlags            = 113;
    inline constexpr uint32_t FinishCommandList          = 114;

    inline constexpr uint32_t kSlotCount                 = 115;

    // ID3D11DeviceContext1 (Direct3D 11.1) .. 4, past the 115 above; only a
    // context that answers QueryInterface for the interface has these entries.
    // (Checked against ID3D11DeviceContext4Vtbl by slot_checks.c.)
    inline constexpr uint32_t CopySubresourceRegion1     = 115;
    inline constexpr uint32_t UpdateSubresource1         = 116;
    inline constexpr uint32_t DiscardResource            = 117;
    inline constexpr uint32_t DiscardView                = 118;
    inline constexpr uint32_t VSSetConstantBuffers1      = 119;
    inline constexpr uint32_t PSSetConstantBuffers1      = 123;
    inline constexpr uint32_t SwapDeviceContextState     = 131;   // ← rewrites the table
    inline constexpr uint32_t ClearView                  = 132;
    inline constexpr uint32_t DiscardView1               = 133;
    inline constexpr uint32_t Flush1                     = 144;   // ID3D11DeviceContext3; rewrites the table
    inline constexpr uint32_t Signal                     = 147;   // ID3D11DeviceContext4; rewrites the table
    inline constexpr uint32_t Wait                       = 148;   // ID3D11DeviceContext4; rewrites the table
} // namespace device_context

// ─────────────────────────────────────────────────────────────────────────────
//  ID3D11Multithread — an interface of the immediate context object, with a
//  table of its own. Turning protection on or off rewrites the context's table.
// ─────────────────────────────────────────────────────────────────────────────

namespace multithread {
    inline constexpr uint32_t Enter                      = 3;
    inline constexpr uint32_t Leave                      = 4;
    inline constexpr uint32_t SetMultithreadProtected    = 5;
    inline constexpr uint32_t GetMultithreadProtected    = 6;
    inline constexpr uint32_t kSlotCount                 = 7;
} // namespace multithread

// ─────────────────────────────────────────────────────────────────────────────
//  ID3D11Device
//  Inherits: IUnknown (0-2) → ID3D11Device (3+)
// ─────────────────────────────────────────────────────────────────────────────

namespace device {
    // IUnknown
    inline constexpr uint32_t QueryInterface         = 0;
    inline constexpr uint32_t AddRef                 = 1;
    inline constexpr uint32_t Release                = 2;

    // ID3D11Device
    inline constexpr uint32_t CreateBuffer                      =  3;
    inline constexpr uint32_t CreateTexture1D                   =  4;
    inline constexpr uint32_t CreateTexture2D                   =  5;   // ← resource creation hook
    inline constexpr uint32_t CreateTexture3D                   =  6;
    inline constexpr uint32_t CreateShaderResourceView          =  7;
    inline constexpr uint32_t CreateUnorderedAccessView         =  8;
    inline constexpr uint32_t CreateRenderTargetView            =  9;   // ← RT creation hook
    inline constexpr uint32_t CreateDepthStencilView            = 10;
    inline constexpr uint32_t CreateInputLayout                 = 11;
    inline constexpr uint32_t CreateVertexShader                = 12;
    inline constexpr uint32_t CreateGeometryShader              = 13;
    inline constexpr uint32_t CreateGeometryShaderWithStreamOutput = 14;
    inline constexpr uint32_t CreatePixelShader                 = 15;
    inline constexpr uint32_t CreateHullShader                  = 16;
    inline constexpr uint32_t CreateDomainShader                = 17;
    inline constexpr uint32_t CreateComputeShader               = 18;
    inline constexpr uint32_t CreateClassLinkage                = 19;
    inline constexpr uint32_t CreateBlendState                  = 20;
    inline constexpr uint32_t CreateDepthStencilState           = 21;
    inline constexpr uint32_t CreateRasterizerState             = 22;
    inline constexpr uint32_t CreateSamplerState                = 23;
    inline constexpr uint32_t CreateQuery                       = 24;
    inline constexpr uint32_t CreatePredicate                   = 25;
    inline constexpr uint32_t CreateCounter                     = 26;
    inline constexpr uint32_t CreateDeferredContext             = 27;
    inline constexpr uint32_t OpenSharedResource                = 28;
    inline constexpr uint32_t CheckFormatSupport                = 29;
    inline constexpr uint32_t CheckMultisampleQualityLevels     = 30;
    inline constexpr uint32_t CheckCounterInfo                  = 31;
    inline constexpr uint32_t CheckCounter                      = 32;
    inline constexpr uint32_t CheckFeatureSupport               = 33;
    inline constexpr uint32_t GetPrivateData                    = 34;
    inline constexpr uint32_t SetPrivateData                    = 35;
    inline constexpr uint32_t SetPrivateDataInterface           = 36;
    inline constexpr uint32_t GetFeatureLevel                   = 37;
    inline constexpr uint32_t GetCreationFlags                  = 38;
    inline constexpr uint32_t GetDeviceRemovedReason            = 39;
    inline constexpr uint32_t GetImmediateContext               = 40;
    inline constexpr uint32_t SetExceptionMode                  = 41;
    inline constexpr uint32_t GetExceptionMode                  = 42;

    inline constexpr uint32_t kSlotCount                        = 43;

    // ID3D11Device1 (checked against ID3D11Device5Vtbl by slot_checks.c)
    inline constexpr uint32_t GetImmediateContext1              = 43;
    inline constexpr uint32_t CreateDeferredContext1            = 44;   // ← deferred context hook
    inline constexpr uint32_t CreateDeviceContextState          = 47;
    // ID3D11Device2
    inline constexpr uint32_t GetImmediateContext2              = 50;
    inline constexpr uint32_t CreateDeferredContext2            = 51;   // ← deferred context hook
    // ID3D11Device3
    inline constexpr uint32_t CreateTexture2D1                  = 54;   // ← resource hook
    inline constexpr uint32_t CreateTexture3D1                  = 55;   // ← resource hook
    inline constexpr uint32_t GetImmediateContext3              = 61;
    inline constexpr uint32_t CreateDeferredContext3            = 62;   // ← deferred context hook
} // namespace device

} // namespace vangfx::d3d11_slots
