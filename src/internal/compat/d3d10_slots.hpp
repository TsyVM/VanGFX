#pragma once

/**
 * compat/d3d10_slots.hpp — vtable slot indices for D3D10 COM interfaces.
 *
 * Slots derived from d3d10.h and d3d10_1.h (Windows SDK 10.0.20348.0).
 * The D3D10 vtable has been frozen since Windows Vista and will not change.
 *
 * D3D10 uses DXGI for swap chains, so IDXGISwapChain slots live in
 * d3d11_slots.hpp (swap_chain / swap_chain1 — identical at the DXGI layer).
 */

#include <cstdint>

namespace vangfx::d3d10_slots {

// ─────────────────────────────────────────────────────────────────────────────
//  ID3D10Device
//  IUnknown(0-2) → ID3D10Device(3-93)
// ─────────────────────────────────────────────────────────────────────────────

namespace device {
    inline constexpr uint32_t QueryInterface                  =  0;
    inline constexpr uint32_t AddRef                          =  1;
    inline constexpr uint32_t Release                         =  2;

    // State-setting (draw pipeline)
    inline constexpr uint32_t VSSetConstantBuffers            =  3;
    inline constexpr uint32_t PSSetShaderResources            =  4;
    inline constexpr uint32_t PSSetShader                     =  5;  // ← shader hook
    inline constexpr uint32_t PSSetSamplers                   =  6;
    inline constexpr uint32_t VSSetShader                     =  7;  // ← shader hook
    inline constexpr uint32_t DrawIndexed                     =  8;  // ← draw hook
    inline constexpr uint32_t Draw                            =  9;  // ← draw hook
    inline constexpr uint32_t PSSetConstantBuffers            = 10;
    inline constexpr uint32_t IASetInputLayout                = 11;
    inline constexpr uint32_t IASetVertexBuffers              = 12;
    inline constexpr uint32_t IASetIndexBuffer                = 13;
    inline constexpr uint32_t DrawIndexedInstanced            = 14;
    inline constexpr uint32_t DrawInstanced                   = 15;
    inline constexpr uint32_t GSSetConstantBuffers            = 16;
    inline constexpr uint32_t GSSetShader                     = 17;  // ← shader hook
    inline constexpr uint32_t IASetPrimitiveTopology          = 18;
    inline constexpr uint32_t VSSetShaderResources            = 19;
    inline constexpr uint32_t VSSetSamplers                   = 20;
    inline constexpr uint32_t SetPredication                  = 21;
    inline constexpr uint32_t GSSetShaderResources            = 22;
    inline constexpr uint32_t GSSetSamplers                   = 23;
    inline constexpr uint32_t OMSetRenderTargets              = 24;
    inline constexpr uint32_t OMSetBlendState                 = 25;
    inline constexpr uint32_t OMSetDepthStencilState          = 26;
    inline constexpr uint32_t SOSetTargets                    = 27;
    inline constexpr uint32_t DrawAuto                        = 28;  // ← draw hook
    inline constexpr uint32_t RSSetState                      = 29;
    inline constexpr uint32_t RSSetViewports                  = 30;
    inline constexpr uint32_t RSSetScissorRects               = 31;
    inline constexpr uint32_t CopySubresourceRegion           = 32;
    inline constexpr uint32_t CopyResource                    = 33;
    inline constexpr uint32_t UpdateSubresource               = 34;
    inline constexpr uint32_t ClearRenderTargetView           = 35;
    inline constexpr uint32_t ClearDepthStencilView           = 36;
    inline constexpr uint32_t GenerateMips                    = 37;
    inline constexpr uint32_t ResolveSubresource              = 38;

    // State-getting
    inline constexpr uint32_t VSGetConstantBuffers            = 39;
    inline constexpr uint32_t PSGetShaderResources            = 40;
    inline constexpr uint32_t PSGetShader                     = 41;
    inline constexpr uint32_t PSGetSamplers                   = 42;
    inline constexpr uint32_t VSGetShader                     = 43;
    inline constexpr uint32_t PSGetConstantBuffers            = 44;
    inline constexpr uint32_t IAGetInputLayout                = 45;
    inline constexpr uint32_t IAGetVertexBuffers              = 46;
    inline constexpr uint32_t IAGetIndexBuffer                = 47;
    inline constexpr uint32_t GSGetConstantBuffers            = 48;
    inline constexpr uint32_t GSGetShader                     = 49;
    inline constexpr uint32_t IAGetPrimitiveTopology          = 50;
    inline constexpr uint32_t VSGetShaderResources            = 51;
    inline constexpr uint32_t VSGetSamplers                   = 52;
    inline constexpr uint32_t GetPredication                  = 53;
    inline constexpr uint32_t GSGetShaderResources            = 54;
    inline constexpr uint32_t GSGetSamplers                   = 55;
    inline constexpr uint32_t OMGetRenderTargets              = 56;
    inline constexpr uint32_t OMGetBlendState                 = 57;
    inline constexpr uint32_t OMGetDepthStencilState          = 58;
    inline constexpr uint32_t SOGetTargets                    = 59;
    inline constexpr uint32_t RSGetState                      = 60;
    inline constexpr uint32_t RSGetViewports                  = 61;
    inline constexpr uint32_t RSGetScissorRects               = 62;
    inline constexpr uint32_t GetDeviceRemovedReason          = 63;
    inline constexpr uint32_t SetExceptionMode                = 64;
    inline constexpr uint32_t GetExceptionMode                = 65;

    // Private data, state, and resource creation
    inline constexpr uint32_t GetPrivateData                  = 66;
    inline constexpr uint32_t SetPrivateData                  = 67;
    inline constexpr uint32_t SetPrivateDataInterface         = 68;
    inline constexpr uint32_t ClearState                      = 69;
    inline constexpr uint32_t Flush                           = 70;
    inline constexpr uint32_t CreateBuffer                    = 71;
    inline constexpr uint32_t CreateTexture1D                 = 72;
    inline constexpr uint32_t CreateTexture2D                 = 73;
    inline constexpr uint32_t CreateTexture3D                 = 74;
    inline constexpr uint32_t CreateShaderResourceView        = 75;
    inline constexpr uint32_t CreateRenderTargetView          = 76;
    inline constexpr uint32_t CreateDepthStencilView          = 77;
    inline constexpr uint32_t CreateInputLayout               = 78;
    inline constexpr uint32_t CreateVertexShader              = 79;
    inline constexpr uint32_t CreateGeometryShader            = 80;
    inline constexpr uint32_t CreateGeometryShaderWithStreamOutput = 81;
    inline constexpr uint32_t CreatePixelShader               = 82;  // ← resource hook
    inline constexpr uint32_t CreateBlendState                = 83;
    inline constexpr uint32_t CreateDepthStencilState         = 84;
    inline constexpr uint32_t CreateRasterizerState           = 85;
    inline constexpr uint32_t CreateSamplerState              = 86;
    inline constexpr uint32_t CreateQuery                     = 87;
    inline constexpr uint32_t CreatePredicate                 = 88;
    inline constexpr uint32_t CreateCounter                   = 89;
    inline constexpr uint32_t CheckFormatSupport              = 90;
    inline constexpr uint32_t CheckMultisampleQualityLevels   = 91;
    inline constexpr uint32_t CheckCounterInfo                = 92;
    inline constexpr uint32_t CheckCounter                    = 93;
    inline constexpr uint32_t GetCreationFlags                = 94;
    inline constexpr uint32_t OpenSharedResource              = 95;

    inline constexpr uint32_t kSlotCount                      = 96;

    // ID3D10Device1 extends ID3D10Device (after slot 95)
    inline constexpr uint32_t CreateShaderResourceView1       = 96;
    inline constexpr uint32_t CreateBlendState1               = 97;
    inline constexpr uint32_t GetFeatureLevel                 = 98;
    inline constexpr uint32_t kSlotCount1                     = 99;
} // namespace device

// ─────────────────────────────────────────────────────────────────────────────
//  IDXGISwapChain slots for D3D10 are in d3d11_slots.hpp
//  (swap_chain / swap_chain1 — same DXGI interface)
// ─────────────────────────────────────────────────────────────────────────────

} // namespace vangfx::d3d10_slots
