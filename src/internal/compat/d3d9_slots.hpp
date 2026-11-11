#pragma once

/**
 * vangfx/compat/d3d9_slots.hpp — vtable slot indices for D3D9 COM interfaces.
 *
 * Slot indices are derived from the IDL declaration order in d3d9.h (Windows
 * SDK).  IUnknown always occupies slots 0–2 (QueryInterface / AddRef /
 * Release); the interface-specific methods follow.
 *
 * These are compile-time constants so the compiler can constant-fold them into
 * vtable::patch<>() / vtable::read<>() calls.
 *
 * Tested against Windows SDK 10.0.20348.0 (d3d9.h version from DirectX June
 * 2010 SDK through WDK 11).  The D3D9 vtable has been frozen for 15+ years
 * and will not change.
 */

#include <cstdint>

namespace vangfx::d3d9_slots {

// ─────────────────────────────────────────────────────────────────────────────
//  IDirect3DDevice9
//  Source: d3d9.h — IDirect3DDevice9 declaration order
// ─────────────────────────────────────────────────────────────────────────────

// IUnknown (inherited)
inline constexpr uint32_t QueryInterface         = 0;
inline constexpr uint32_t AddRef                 = 1;
inline constexpr uint32_t Release                = 2;

// IDirect3DDevice9 — in declaration order
inline constexpr uint32_t TestCooperativeLevel   =  3;
inline constexpr uint32_t GetAvailableTextureMem =  4;
inline constexpr uint32_t EvictManagedResources  =  5;
inline constexpr uint32_t GetDirect3D            =  6;
inline constexpr uint32_t GetDeviceCaps          =  7;
inline constexpr uint32_t GetDisplayMode         =  8;
inline constexpr uint32_t GetCreationParameters  =  9;
inline constexpr uint32_t SetCursorProperties    = 10;
inline constexpr uint32_t SetCursorPosition      = 11;
inline constexpr uint32_t ShowCursor             = 12;
inline constexpr uint32_t CreateAdditionalSwapChain = 13;
inline constexpr uint32_t GetSwapChain           = 14;
inline constexpr uint32_t GetNumberOfSwapChains  = 15;
inline constexpr uint32_t Reset                  = 16;
inline constexpr uint32_t Present                = 17;   // ← VanGFX frame hook
inline constexpr uint32_t GetBackBuffer          = 18;
inline constexpr uint32_t GetRasterStatus        = 19;
inline constexpr uint32_t SetDialogBoxMode       = 20;
inline constexpr uint32_t SetGammaRamp           = 21;
inline constexpr uint32_t GetGammaRamp           = 22;
inline constexpr uint32_t CreateTexture          = 23;
inline constexpr uint32_t CreateVolumeTexture    = 24;
inline constexpr uint32_t CreateCubeTexture      = 25;
inline constexpr uint32_t CreateVertexBuffer     = 26;
inline constexpr uint32_t CreateIndexBuffer      = 27;
inline constexpr uint32_t CreateRenderTarget     = 28;
inline constexpr uint32_t CreateDepthStencilSurface = 29;
inline constexpr uint32_t UpdateSurface          = 30;
inline constexpr uint32_t UpdateTexture          = 31;
inline constexpr uint32_t GetRenderTargetData    = 32;
inline constexpr uint32_t GetFrontBufferData     = 33;
inline constexpr uint32_t StretchRect            = 34;
inline constexpr uint32_t ColorFill              = 35;
inline constexpr uint32_t CreateOffscreenPlainSurface = 36;
inline constexpr uint32_t SetRenderTarget        = 37;
inline constexpr uint32_t GetRenderTarget        = 38;
inline constexpr uint32_t SetDepthStencilSurface = 39;
inline constexpr uint32_t GetDepthStencilSurface = 40;
inline constexpr uint32_t BeginScene             = 41;
inline constexpr uint32_t EndScene               = 42;
inline constexpr uint32_t Clear                  = 43;
inline constexpr uint32_t SetTransform           = 44;
inline constexpr uint32_t GetTransform           = 45;
inline constexpr uint32_t MultiplyTransform      = 46;
inline constexpr uint32_t SetViewport            = 47;
inline constexpr uint32_t GetViewport            = 48;
inline constexpr uint32_t SetMaterial            = 49;
inline constexpr uint32_t GetMaterial            = 50;
inline constexpr uint32_t SetLight               = 51;
inline constexpr uint32_t GetLight               = 52;
inline constexpr uint32_t LightEnable            = 53;
inline constexpr uint32_t GetLightEnable         = 54;
inline constexpr uint32_t SetClipPlane           = 55;
inline constexpr uint32_t GetClipPlane           = 56;
inline constexpr uint32_t SetRenderState         = 57;
inline constexpr uint32_t GetRenderState         = 58;
inline constexpr uint32_t CreateStateBlock       = 59;
inline constexpr uint32_t BeginStateBlock        = 60;
inline constexpr uint32_t EndStateBlock          = 61;
inline constexpr uint32_t SetClipStatus          = 62;
inline constexpr uint32_t GetClipStatus          = 63;
inline constexpr uint32_t GetTexture             = 64;
inline constexpr uint32_t SetTexture             = 65;   // ← texture binding hook
inline constexpr uint32_t GetTextureStageState   = 66;
inline constexpr uint32_t SetTextureStageState   = 67;
inline constexpr uint32_t GetSamplerState        = 68;
inline constexpr uint32_t SetSamplerState        = 69;
inline constexpr uint32_t ValidateDevice         = 70;
inline constexpr uint32_t SetPaletteEntries      = 71;
inline constexpr uint32_t GetPaletteEntries      = 72;
inline constexpr uint32_t SetCurrentTexturePalette = 73;
inline constexpr uint32_t GetCurrentTexturePalette = 74;
inline constexpr uint32_t SetScissorRect         = 75;
inline constexpr uint32_t GetScissorRect         = 76;
inline constexpr uint32_t SetSoftwareVertexProcessing = 77;
inline constexpr uint32_t GetSoftwareVertexProcessing = 78;
inline constexpr uint32_t SetNPatchMode          = 79;
inline constexpr uint32_t GetNPatchMode          = 80;
inline constexpr uint32_t DrawPrimitive          = 81;   // ← draw hook
inline constexpr uint32_t DrawIndexedPrimitive   = 82;   // ← draw hook
inline constexpr uint32_t DrawPrimitiveUP        = 83;
inline constexpr uint32_t DrawIndexedPrimitiveUP = 84;
inline constexpr uint32_t ProcessVertices        = 85;
inline constexpr uint32_t CreateVertexDeclaration = 86;
inline constexpr uint32_t SetVertexDeclaration   = 87;
inline constexpr uint32_t GetVertexDeclaration   = 88;
inline constexpr uint32_t SetFVF                 = 89;
inline constexpr uint32_t GetFVF                 = 90;
inline constexpr uint32_t CreateVertexShader     = 91;
inline constexpr uint32_t SetVertexShader        = 92;   // ← shader binding hook
inline constexpr uint32_t GetVertexShader        = 93;
inline constexpr uint32_t SetVertexShaderConstantF = 94;
inline constexpr uint32_t GetVertexShaderConstantF = 95;
inline constexpr uint32_t SetVertexShaderConstantI = 96;
inline constexpr uint32_t GetVertexShaderConstantI = 97;
inline constexpr uint32_t SetVertexShaderConstantB = 98;
inline constexpr uint32_t GetVertexShaderConstantB = 99;
inline constexpr uint32_t SetStreamSource        = 100;
inline constexpr uint32_t GetStreamSource        = 101;
inline constexpr uint32_t SetStreamSourceFreq    = 102;
inline constexpr uint32_t GetStreamSourceFreq    = 103;
inline constexpr uint32_t SetIndices             = 104;
inline constexpr uint32_t GetIndices             = 105;
inline constexpr uint32_t CreatePixelShader      = 106;
inline constexpr uint32_t SetPixelShader         = 107;  // ← shader binding hook
inline constexpr uint32_t GetPixelShader         = 108;
inline constexpr uint32_t SetPixelShaderConstantF = 109;
inline constexpr uint32_t GetPixelShaderConstantF = 110;
inline constexpr uint32_t SetPixelShaderConstantI = 111;
inline constexpr uint32_t GetPixelShaderConstantI = 112;
inline constexpr uint32_t SetPixelShaderConstantB = 113;
inline constexpr uint32_t GetPixelShaderConstantB = 114;
inline constexpr uint32_t DrawRectPatch          = 115;
inline constexpr uint32_t DrawTriPatch           = 116;
inline constexpr uint32_t DeletePatch            = 117;
inline constexpr uint32_t CreateQuery            = 118;

// Total: 119 slots (indices 0–118)
inline constexpr uint32_t kSlotCount            = 119;

// IDirect3DDevice9Ex, after the 119 above
namespace device_ex {
    inline constexpr uint32_t PresentEx                     = 121;
    inline constexpr uint32_t CheckDeviceState              = 128;
    inline constexpr uint32_t CreateRenderTargetEx          = 129;
    inline constexpr uint32_t CreateOffscreenPlainSurfaceEx = 130;
    inline constexpr uint32_t CreateDepthStencilSurfaceEx   = 131;
    inline constexpr uint32_t ResetEx                       = 132;
    inline constexpr uint32_t kSlotCount                    = 134;
} // namespace device_ex

// IDirect3D9 / IDirect3D9Ex: device creation, for attaching as a device is made
namespace direct3d {
    inline constexpr uint32_t CreateDevice   = 16;
    inline constexpr uint32_t kSlotCount     = 17;
} // namespace direct3d
namespace direct3d_ex {
    inline constexpr uint32_t CreateDeviceEx = 20;
    inline constexpr uint32_t kSlotCount     = 22;   // GetAdapterLUID is last, at 21
} // namespace direct3d_ex

// IDirect3DSwapChain9 (IDirect3DSwapChain9Ex shares the table)
namespace swap_chain {
    inline constexpr uint32_t Present        = 3;
    inline constexpr uint32_t GetBackBuffer  = 5;
    inline constexpr uint32_t GetDevice      = 8;
    inline constexpr uint32_t kSlotCount     = 10;
} // namespace swap_chain

// IDirect3DQuery9
namespace query {
    inline constexpr uint32_t GetType        = 4;
    inline constexpr uint32_t Issue          = 6;
    inline constexpr uint32_t GetData        = 7;
    inline constexpr uint32_t kSlotCount     = 8;
} // namespace query

// IDirect3DStateBlock9
namespace state_block {
    inline constexpr uint32_t Apply          = 5;
    inline constexpr uint32_t kSlotCount     = 6;
} // namespace state_block

// IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9 (same shape)
namespace vertex_buffer {
    inline constexpr uint32_t Lock           = 11;
    inline constexpr uint32_t Unlock         = 12;
    inline constexpr uint32_t kSlotCount     = 14;
} // namespace vertex_buffer
namespace index_buffer {
    inline constexpr uint32_t Lock           = 11;
    inline constexpr uint32_t Unlock         = 12;
    inline constexpr uint32_t kSlotCount     = 14;
} // namespace index_buffer

// IDirect3DTexture9
namespace texture {
    inline constexpr uint32_t LockRect       = 19;
    inline constexpr uint32_t UnlockRect     = 20;
    inline constexpr uint32_t kSlotCount     = 22;
} // namespace texture

} // namespace vangfx::d3d9_slots
