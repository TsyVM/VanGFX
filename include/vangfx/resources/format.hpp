#pragma once

/**
 * vangfx/resources/format.hpp — SurfaceFormat enum.
 *
 * Single enum covering D3D9 and D3D11 color, depth, compressed, and HDR
 * surface formats.  D3D9 names are provided as aliases where they differ.
 */

#include <cstdint>

namespace vangfx {

enum class SurfaceFormat : uint32_t {
    // ── Standard color ────────────────────────────────────────────────────────
    RGBA8, BGRA8,
    RGBA16F, RGBA32F,
    R11G11B10F,
    R10G10B10A2,
    R8, RG8,
    R16F, R32F,
    RG16F, RG32F,

    // ── D3D9 named equivalents ────────────────────────────────────────────────
    A8R8G8B8, X8R8G8B8,
    A16B16G16R16F,
    A32B32G32R32F,
    R5G6B5,

    // ── Block-compressed ──────────────────────────────────────────────────────
    //
    // The DXT1/DXT3/DXT5 aliases are NOT here. They used to be, written as
    // `DXT1 = BC1` immediately after BC7, and an explicit value in the middle
    // of a sequence resets the implicit counter: the next enumerator continued
    // from DXT5 rather than from BC7, so the whole depth block landed back on
    // top of the compressed one. D16 *was* BC4, D24S8 *was* BC5, D24X8 was
    // BC6H, D32F was BC6H_SF16 and D32FS8 was BC7 -- one value each, so a
    // depth-stencil surface and a BC5 normal map were indistinguishable to
    // every switch, classifier and event in the library.
    //
    // They now live with the other aliases at the bottom, after the last
    // implicitly numbered enumerator, where they cannot do this again.
    BC1, BC2, BC3,
    BC4, BC5,               // ATI1 / ATI2
    BC6H,                   // HDR unsigned float — D3D11/D3D12
    BC6H_SF16,              // HDR signed float   — D3D11/D3D12
    BC7,                    // High-quality — D3D11/D3D12

    // ── Depth / stencil ───────────────────────────────────────────────────────
    D16,
    D24S8,
    D24X8,
    D32F,
    D32FS8,           // DXGI_FORMAT_D32_FLOAT_S8X24_UINT — 64-bit with 8-bit stencil

    // ── D3D9 depth-as-texture ─────────────────────────────────────────────────
    // Selected automatically by depth_read_back() based on driver caps.
    INTZ, RAWZ, DF16, DF24,

    // ── Typeless formats (D3D12) ──────────────────────────────────────────────
    // Used as heap / resource format when views may reinterpret the bits.
    // Cast to a typed format before binding as SRV/RTV/DSV.
    R8_TYPELESS,
    R16_TYPELESS,
    R32_TYPELESS,
    R8G8B8A8_TYPELESS,
    R16G16B16A16_TYPELESS,
    R32G32B32A32_TYPELESS,
    R32G32_TYPELESS,
    R32G32B32_TYPELESS,
    R24G8_TYPELESS,          // paired with D24_UNORM_S8_UINT DSV
    R32_FLOAT_X8X24_TYPELESS,// paired with D32_FLOAT_S8X24_UINT DSV
    BC1_TYPELESS,
    BC2_TYPELESS,
    BC3_TYPELESS,
    BC4_TYPELESS,
    BC5_TYPELESS,
    BC6H_TYPELESS,
    BC7_TYPELESS,

    // ── Integer / unsigned-integer formats (D3D11 / D3D12) ───────────────────
    R8_UINT, R8_SINT,
    R16_UINT, R16_SINT,
    R32_UINT, R32_SINT,
    R8G8_UINT, R8G8_SINT,
    R16G16_UINT, R16G16_SINT,
    R32G32_UINT, R32G32_SINT,
    R8G8B8A8_UINT, R8G8B8A8_SINT,
    R16G16B16A16_UINT, R16G16B16A16_SINT,
    R32G32B32A32_UINT, R32G32B32A32_SINT,

    // ── DXGI swap-chain / display formats ────────────────────────────────────
    B5G6R5,               // DXGI_FORMAT_B5G6R5_UNORM
    B5G5R5A1,             // DXGI_FORMAT_B5G5R5A1_UNORM
    B8G8R8A8_TYPELESS,
    R10G10B10A2_UINT,

    // ── Video / YUV (D3D12 video decode targets) ──────────────────────────────
    NV12,   // 4:2:0 planar — common GPU video decode output
    P010,   // 10-bit NV12
    YUY2,   // 4:2:2 packed

    // ── sRGB and normalised variants ──────────────────────────────────────────
    // The short names above carry no colour space, and there was no way to say
    // "the sRGB one" for the two swap-chain formats a modern backend actually
    // presents in. The 16-bit normalised set is here for the same reason: the
    // float spellings existed, the UNORM/SNORM ones did not.
    RGBA8_SRGB,
    BGRA8_SRGB,
    BC7_SRGB,
    RGBA16_UNORM,
    RGBA16_SNORM,
    RG16_UNORM,
    RG16_SNORM,
    R16_UNORM,
    RGB32F,

    Unknown,

    // ── DXGI-style spellings ──────────────────────────────────────────────────
    //
    // The D3D11 and D3D12 backends were both written against DXGI's own naming
    // -- R8G8B8A8_Unorm rather than RGBA8 -- which is the more precise spelling
    // when the surrounding code is already switching on DXGI_FORMAT. These
    // names were never added to the enum, so neither backend compiled.
    //
    // Aliases rather than a rename: the short names are what the D3D9 backend
    // and the public headers use, and both spellings are right for the audience
    // that reads them. Being aliases, they share a value with their target and
    // so must never appear as a case label alongside it in the same switch.
    //
    // Note both _sRGB and _SRGB: the two backends disagree on the
    // capitalisation, and picking one would silently break the other.

    R8G8B8A8_Unorm            = RGBA8,
    R8G8B8A8_Unorm_SRGB       = RGBA8_SRGB,
    R8G8B8A8_Unorm_sRGB       = RGBA8_SRGB,
    B8G8R8A8_Unorm            = BGRA8,
    B8G8R8A8_Unorm_SRGB       = BGRA8_SRGB,

    R8_Unorm                  = R8,
    R8G8_Unorm                = RG8,
    R10G10B10A2_Unorm         = R10G10B10A2,
    R11G11B10_Float           = R11G11B10F,

    R16_Float                 = R16F,
    R16_Unorm                 = R16_UNORM,
    R16_Uint                  = R16_UINT,
    R32_Float                 = R32F,
    R32_Uint                  = R32_UINT,

    R16G16_Float              = RG16F,
    R16G16_Unorm              = RG16_UNORM,
    R16G16_Snorm              = RG16_SNORM,
    R16G16_SNorm              = RG16_SNORM,

    R16G16B16A16_Float        = RGBA16F,
    R16G16B16A16_Unorm        = RGBA16_UNORM,
    R16G16B16A16_Snorm        = RGBA16_SNORM,

    R32G32B32_Float           = RGB32F,
    R32G32B32A32_Float        = RGBA32F,
    R32G32B32A32_Uint         = R32G32B32A32_UINT,

    BC1_Unorm                 = BC1,
    BC2_Unorm                 = BC2,
    BC3_Unorm                 = BC3,
    BC4_Unorm                 = BC4,
    BC5_Unorm                 = BC5,
    BC6H_UF16                 = BC6H,
    BC7_Unorm                 = BC7,
    BC7_Unorm_SRGB            = BC7_SRGB,

    D16_Unorm                 = D16,
    D24_Unorm_S8_Uint         = D24S8,
    D32_Float                 = D32F,
    D32_Float_S8X24_Uint      = D32FS8,

    // The D3D9-era spellings of the first three block-compressed formats.
    DXT1                      = BC1,
    DXT3                      = BC2,
    DXT5                      = BC3,
};

// Every enumerator above this line is either implicitly numbered or an alias in
// the trailing block. An explicit value placed in the middle of the sequence
// restarts the counter from there, which is how the depth formats silently
// became the compressed ones. These assertions are the tripwire: they fail at
// compile time rather than letting a depth buffer be classified as a normal map.
static_assert(SurfaceFormat::D16    != SurfaceFormat::BC4,
              "SurfaceFormat: depth block collides with the compressed block — "
              "an explicit enumerator was added mid-sequence again.");
static_assert(SurfaceFormat::D24S8  != SurfaceFormat::BC5,       "see above");
static_assert(SurfaceFormat::D24X8  != SurfaceFormat::BC6H,      "see above");
static_assert(SurfaceFormat::D32F   != SurfaceFormat::BC6H_SF16, "see above");
static_assert(SurfaceFormat::D32FS8 != SurfaceFormat::BC7,       "see above");
static_assert(SurfaceFormat::INTZ   != SurfaceFormat::Unknown,   "see above");

} // namespace vangfx
