#pragma once

/**
 * compat/d3d8_slots.hpp — vtable slot indices for D3D8 COM interfaces.
 *
 * D3D8 is a 32-bit only API (no x64 d3d8.dll) but the slot table is still
 * useful for the 32-bit build: the translator, the slot_checks static_asserts,
 * and vtable patching.
 *
 * Slots derived from d3d8.h (DirectX 8.1 SDK, Windows 7 compat headers).
 * The D3D8 vtable has been frozen since 2001 and will not change.
 */

#include <cstdint>

namespace vangfx::d3d8_slots {

// ─────────────────────────────────────────────────────────────────────────────
//  IDirect3D8
//  IUnknown(0-2) → IDirect3D8(3-15)
// ─────────────────────────────────────────────────────────────────────────────

namespace direct3d {
    inline constexpr uint32_t QueryInterface        =  0;
    inline constexpr uint32_t AddRef                =  1;
    inline constexpr uint32_t Release               =  2;

    inline constexpr uint32_t RegisterSoftwareDevice =  3;
    inline constexpr uint32_t GetAdapterCount        =  4;
    inline constexpr uint32_t GetAdapterIdentifier   =  5;
    inline constexpr uint32_t GetAdapterModeCount    =  6;
    inline constexpr uint32_t EnumAdapterModes       =  7;
    inline constexpr uint32_t GetAdapterDisplayMode  =  8;
    inline constexpr uint32_t CheckDeviceType        =  9;
    inline constexpr uint32_t CheckDeviceFormat      = 10;
    inline constexpr uint32_t CheckDeviceMultiSampleType = 11;
    inline constexpr uint32_t CheckDepthStencilMatch = 12;
    inline constexpr uint32_t GetDeviceCaps          = 13;
    inline constexpr uint32_t GetAdapterMonitor      = 14;
    inline constexpr uint32_t CreateDevice           = 15;  // ← device creation hook

    inline constexpr uint32_t kSlotCount             = 16;
} // namespace direct3d

// ─────────────────────────────────────────────────────────────────────────────
//  IDirect3DDevice8
//  IUnknown(0-2) → IDirect3DDevice8(3-96)
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr uint32_t QueryInterface             =  0;
inline constexpr uint32_t AddRef                     =  1;
inline constexpr uint32_t Release                    =  2;

inline constexpr uint32_t TestCooperativeLevel       =  3;
inline constexpr uint32_t GetAvailableTextureMem     =  4;
inline constexpr uint32_t ResourceManagerDiscardBytes =  5;
inline constexpr uint32_t GetDirect3D                =  6;
inline constexpr uint32_t GetDeviceCaps              =  7;
inline constexpr uint32_t GetDisplayMode             =  8;
inline constexpr uint32_t GetCreationParameters      =  9;
inline constexpr uint32_t SetCursorProperties        = 10;
inline constexpr uint32_t SetCursorPosition          = 11;
inline constexpr uint32_t ShowCursor                 = 12;
inline constexpr uint32_t CreateAdditionalSwapChain  = 13;
inline constexpr uint32_t Reset                      = 14;
inline constexpr uint32_t Present                    = 15;  // ← frame hook
inline constexpr uint32_t GetBackBuffer              = 16;
inline constexpr uint32_t GetRasterStatus            = 17;
inline constexpr uint32_t SetGammaRamp               = 18;
inline constexpr uint32_t GetGammaRamp               = 19;
inline constexpr uint32_t CreateTexture              = 20;
inline constexpr uint32_t CreateVolumeTexture        = 21;
inline constexpr uint32_t CreateCubeTexture          = 22;
inline constexpr uint32_t CreateVertexBuffer         = 23;
inline constexpr uint32_t CreateIndexBuffer          = 24;
inline constexpr uint32_t CreateRenderTarget         = 25;
inline constexpr uint32_t CreateDepthStencilSurface  = 26;
inline constexpr uint32_t CreateImageSurface         = 27;
inline constexpr uint32_t CopyRects                  = 28;
inline constexpr uint32_t UpdateTexture              = 29;
inline constexpr uint32_t GetFrontBuffer             = 30;
inline constexpr uint32_t SetRenderTarget            = 31;
inline constexpr uint32_t GetRenderTarget            = 32;
inline constexpr uint32_t GetDepthStencilSurface     = 33;
inline constexpr uint32_t BeginScene                 = 34;
inline constexpr uint32_t EndScene                   = 35;
inline constexpr uint32_t Clear                      = 36;
inline constexpr uint32_t SetTransform               = 37;
inline constexpr uint32_t GetTransform               = 38;
inline constexpr uint32_t MultiplyTransform          = 39;
inline constexpr uint32_t SetViewport                = 40;
inline constexpr uint32_t GetViewport                = 41;
inline constexpr uint32_t SetMaterial                = 42;
inline constexpr uint32_t GetMaterial                = 43;
inline constexpr uint32_t SetLight                   = 44;
inline constexpr uint32_t GetLight                   = 45;
inline constexpr uint32_t LightEnable                = 46;
inline constexpr uint32_t GetLightEnable             = 47;
inline constexpr uint32_t SetClipPlane               = 48;
inline constexpr uint32_t GetClipPlane               = 49;
inline constexpr uint32_t SetRenderState             = 50;
inline constexpr uint32_t GetRenderState             = 51;
inline constexpr uint32_t BeginStateBlock            = 52;
inline constexpr uint32_t EndStateBlock              = 53;
inline constexpr uint32_t ApplyStateBlock            = 54;
inline constexpr uint32_t CaptureStateBlock          = 55;
inline constexpr uint32_t DeleteStateBlock           = 56;
inline constexpr uint32_t CreateStateBlock           = 57;
inline constexpr uint32_t SetClipStatus              = 58;
inline constexpr uint32_t GetClipStatus              = 59;
inline constexpr uint32_t GetTexture                 = 60;
inline constexpr uint32_t SetTexture                 = 61;  // ← texture binding hook
inline constexpr uint32_t GetTextureStageState       = 62;
inline constexpr uint32_t SetTextureStageState       = 63;
inline constexpr uint32_t ValidateDevice             = 64;
inline constexpr uint32_t GetInfo                    = 65;
inline constexpr uint32_t SetPaletteEntries          = 66;
inline constexpr uint32_t GetPaletteEntries          = 67;
inline constexpr uint32_t SetCurrentTexturePalette   = 68;
inline constexpr uint32_t GetCurrentTexturePalette   = 69;
inline constexpr uint32_t DrawPrimitive              = 70;  // ← draw hook
inline constexpr uint32_t DrawIndexedPrimitive       = 71;  // ← draw hook
inline constexpr uint32_t DrawPrimitiveUP            = 72;
inline constexpr uint32_t DrawIndexedPrimitiveUP     = 73;
inline constexpr uint32_t ProcessVertices            = 74;
inline constexpr uint32_t CreateVertexShader         = 75;
inline constexpr uint32_t SetVertexShader            = 76;  // ← shader binding hook
inline constexpr uint32_t GetVertexShader            = 77;
inline constexpr uint32_t DeleteVertexShader         = 78;
inline constexpr uint32_t SetVertexShaderConstant    = 79;
inline constexpr uint32_t GetVertexShaderConstant    = 80;
inline constexpr uint32_t GetVertexShaderDeclaration = 81;
inline constexpr uint32_t GetVertexShaderFunction    = 82;
inline constexpr uint32_t SetStreamSource            = 83;
inline constexpr uint32_t GetStreamSource            = 84;
inline constexpr uint32_t SetIndices                 = 85;
inline constexpr uint32_t GetIndices                 = 86;
inline constexpr uint32_t CreatePixelShader          = 87;
inline constexpr uint32_t SetPixelShader             = 88;  // ← shader binding hook
inline constexpr uint32_t GetPixelShader             = 89;
inline constexpr uint32_t DeletePixelShader          = 90;
inline constexpr uint32_t SetPixelShaderConstant     = 91;
inline constexpr uint32_t GetPixelShaderConstant     = 92;
inline constexpr uint32_t GetPixelShaderFunction     = 93;
inline constexpr uint32_t DrawRectPatch              = 94;
inline constexpr uint32_t DrawTriPatch               = 95;
inline constexpr uint32_t DeletePatch                = 96;

inline constexpr uint32_t kSlotCount                 = 97;

// IDirect3DSwapChain8
namespace swap_chain {
    inline constexpr uint32_t Present       = 3;
    inline constexpr uint32_t GetBackBuffer = 5;
    inline constexpr uint32_t kSlotCount    = 7;
} // namespace swap_chain

} // namespace vangfx::d3d8_slots
