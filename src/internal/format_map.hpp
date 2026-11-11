#pragma once

/**
 * src/internal/format_map.hpp — SurfaceFormat ↔ native format, both ways.
 *
 * Six one-way converters used to live in six backend files: D3DFORMAT and
 * DXGI_FORMAT to SurfaceFormat, so an event could report what it had seen.
 * Nothing went the other way, because until resources could be created there
 * was nothing to convert *to*. Creating a texture from a TextureDesc needs the
 * reverse of every one of them, so they are gathered here and completed.
 *
 * Header-only: src/internal is not part of the source glob in CMakeLists, so a
 * .cpp here would not be compiled and would fail at link with the same shape of
 * error this library has already been caught by once.
 *
 * ALIASES
 *
 * SurfaceFormat has two spellings for many values -- RGBA8 and
 * R8G8B8A8_Unorm are the same enumerator. Only the canonical (first-declared)
 * name appears as a case label below; adding the alias beside it is a
 * duplicate-case error, and the switch already covers both spellings by
 * covering the value.
 *
 * LOSSY DIRECTIONS
 *
 * Some conversions cannot round-trip and should not pretend to:
 *   - D3D9 has no typeless formats. A typeless SurfaceFormat maps to the
 *     closest typed D3D9 format, because that is the only thing D3D9 can make.
 *   - D3D9's INTZ/RAWZ/DF16/DF24 are FourCC depth-as-texture hacks with no
 *     DXGI equivalent; on D3D11/D3D12 they map to the typeless pair that
 *     achieves the same thing.
 *   - NV12/P010/YUY2 have no D3D9 equivalent at all and return D3DFMT_UNKNOWN.
 */

#include <vangfx/resources/format.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <dxgiformat.h>

#include <cstdint>

namespace vangfx::fmtmap {

// D3D9 FourCC depth formats. Not in d3d9.h: they are vendor extensions
// recognised by the driver, checked for with CheckDeviceFormat.
#ifndef MAKEFOURCC_VGX
#  define MAKEFOURCC_VGX(a,b,c,d) \
     ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | \
     ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#endif

inline constexpr D3DFORMAT kFmtINTZ = static_cast<D3DFORMAT>(MAKEFOURCC_VGX('I','N','T','Z'));
inline constexpr D3DFORMAT kFmtRAWZ = static_cast<D3DFORMAT>(MAKEFOURCC_VGX('R','A','W','Z'));
inline constexpr D3DFORMAT kFmtDF16 = static_cast<D3DFORMAT>(MAKEFOURCC_VGX('D','F','1','6'));
inline constexpr D3DFORMAT kFmtDF24 = static_cast<D3DFORMAT>(MAKEFOURCC_VGX('D','F','2','4'));
inline constexpr D3DFORMAT kFmtATI1 = static_cast<D3DFORMAT>(MAKEFOURCC_VGX('A','T','I','1'));
inline constexpr D3DFORMAT kFmtATI2 = static_cast<D3DFORMAT>(MAKEFOURCC_VGX('A','T','I','2'));

// ─────────────────────────────────────────────────────────────────────────────
//  SurfaceFormat → DXGI_FORMAT
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline DXGI_FORMAT to_dxgi(SurfaceFormat f) noexcept
{
    switch (f) {
    case SurfaceFormat::RGBA8:            return DXGI_FORMAT_R8G8B8A8_UNORM;
    case SurfaceFormat::RGBA8_SRGB:       return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case SurfaceFormat::BGRA8:            return DXGI_FORMAT_B8G8R8A8_UNORM;
    case SurfaceFormat::BGRA8_SRGB:       return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case SurfaceFormat::RGBA16F:          return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case SurfaceFormat::RGBA32F:          return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case SurfaceFormat::RGB32F:           return DXGI_FORMAT_R32G32B32_FLOAT;
    case SurfaceFormat::R11G11B10F:       return DXGI_FORMAT_R11G11B10_FLOAT;
    case SurfaceFormat::R10G10B10A2:      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case SurfaceFormat::R10G10B10A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
    case SurfaceFormat::R8:               return DXGI_FORMAT_R8_UNORM;
    case SurfaceFormat::RG8:              return DXGI_FORMAT_R8G8_UNORM;
    case SurfaceFormat::R16F:             return DXGI_FORMAT_R16_FLOAT;
    case SurfaceFormat::R32F:             return DXGI_FORMAT_R32_FLOAT;
    case SurfaceFormat::RG16F:            return DXGI_FORMAT_R16G16_FLOAT;
    case SurfaceFormat::RG32F:            return DXGI_FORMAT_R32G32_FLOAT;

    // D3D9-named colour. BGRA on D3D9, and the X8 variant has no DXGI
    // equivalent that drops alpha -- B8G8R8X8 is the closest and is what a
    // readback of one actually contains.
    case SurfaceFormat::A8R8G8B8:         return DXGI_FORMAT_B8G8R8A8_UNORM;
    case SurfaceFormat::X8R8G8B8:         return DXGI_FORMAT_B8G8R8X8_UNORM;
    case SurfaceFormat::A16B16G16R16F:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case SurfaceFormat::A32B32G32R32F:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case SurfaceFormat::R5G6B5:           return DXGI_FORMAT_B5G6R5_UNORM;
    case SurfaceFormat::B5G6R5:           return DXGI_FORMAT_B5G6R5_UNORM;
    case SurfaceFormat::B5G5R5A1:         return DXGI_FORMAT_B5G5R5A1_UNORM;

    // Normalised 16-bit.
    case SurfaceFormat::RGBA16_UNORM:     return DXGI_FORMAT_R16G16B16A16_UNORM;
    case SurfaceFormat::RGBA16_SNORM:     return DXGI_FORMAT_R16G16B16A16_SNORM;
    case SurfaceFormat::RG16_UNORM:       return DXGI_FORMAT_R16G16_UNORM;
    case SurfaceFormat::RG16_SNORM:       return DXGI_FORMAT_R16G16_SNORM;
    case SurfaceFormat::R16_UNORM:        return DXGI_FORMAT_R16_UNORM;

    // Block compressed.
    case SurfaceFormat::BC1:              return DXGI_FORMAT_BC1_UNORM;
    case SurfaceFormat::BC2:              return DXGI_FORMAT_BC2_UNORM;
    case SurfaceFormat::BC3:              return DXGI_FORMAT_BC3_UNORM;
    case SurfaceFormat::BC4:              return DXGI_FORMAT_BC4_UNORM;
    case SurfaceFormat::BC5:              return DXGI_FORMAT_BC5_UNORM;
    case SurfaceFormat::BC6H:             return DXGI_FORMAT_BC6H_UF16;
    case SurfaceFormat::BC6H_SF16:        return DXGI_FORMAT_BC6H_SF16;
    case SurfaceFormat::BC7:              return DXGI_FORMAT_BC7_UNORM;
    case SurfaceFormat::BC7_SRGB:         return DXGI_FORMAT_BC7_UNORM_SRGB;

    // Depth / stencil.
    case SurfaceFormat::D16:              return DXGI_FORMAT_D16_UNORM;
    case SurfaceFormat::D24S8:            return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case SurfaceFormat::D24X8:            return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case SurfaceFormat::D32F:             return DXGI_FORMAT_D32_FLOAT;
    case SurfaceFormat::D32FS8:           return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

    // D3D9 depth-as-texture. The DXGI way to sample a depth buffer is a
    // typeless resource with a depth DSV and a colour SRV, so that is what
    // these become.
    case SurfaceFormat::INTZ:             return DXGI_FORMAT_R24G8_TYPELESS;
    case SurfaceFormat::RAWZ:             return DXGI_FORMAT_R24G8_TYPELESS;
    case SurfaceFormat::DF16:             return DXGI_FORMAT_R16_TYPELESS;
    case SurfaceFormat::DF24:             return DXGI_FORMAT_R24G8_TYPELESS;

    // Typeless.
    case SurfaceFormat::R8_TYPELESS:             return DXGI_FORMAT_R8_TYPELESS;
    case SurfaceFormat::R16_TYPELESS:            return DXGI_FORMAT_R16_TYPELESS;
    case SurfaceFormat::R32_TYPELESS:            return DXGI_FORMAT_R32_TYPELESS;
    case SurfaceFormat::R8G8B8A8_TYPELESS:       return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case SurfaceFormat::R16G16B16A16_TYPELESS:   return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case SurfaceFormat::R32G32B32A32_TYPELESS:   return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case SurfaceFormat::R32G32_TYPELESS:         return DXGI_FORMAT_R32G32_TYPELESS;
    case SurfaceFormat::R32G32B32_TYPELESS:      return DXGI_FORMAT_R32G32B32_TYPELESS;
    case SurfaceFormat::R24G8_TYPELESS:          return DXGI_FORMAT_R24G8_TYPELESS;
    case SurfaceFormat::R32_FLOAT_X8X24_TYPELESS:return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case SurfaceFormat::B8G8R8A8_TYPELESS:       return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case SurfaceFormat::BC1_TYPELESS:            return DXGI_FORMAT_BC1_TYPELESS;
    case SurfaceFormat::BC2_TYPELESS:            return DXGI_FORMAT_BC2_TYPELESS;
    case SurfaceFormat::BC3_TYPELESS:            return DXGI_FORMAT_BC3_TYPELESS;
    case SurfaceFormat::BC4_TYPELESS:            return DXGI_FORMAT_BC4_TYPELESS;
    case SurfaceFormat::BC5_TYPELESS:            return DXGI_FORMAT_BC5_TYPELESS;
    case SurfaceFormat::BC6H_TYPELESS:           return DXGI_FORMAT_BC6H_TYPELESS;
    case SurfaceFormat::BC7_TYPELESS:            return DXGI_FORMAT_BC7_TYPELESS;

    // Integer.
    case SurfaceFormat::R8_UINT:              return DXGI_FORMAT_R8_UINT;
    case SurfaceFormat::R8_SINT:              return DXGI_FORMAT_R8_SINT;
    case SurfaceFormat::R16_UINT:             return DXGI_FORMAT_R16_UINT;
    case SurfaceFormat::R16_SINT:             return DXGI_FORMAT_R16_SINT;
    case SurfaceFormat::R32_UINT:             return DXGI_FORMAT_R32_UINT;
    case SurfaceFormat::R32_SINT:             return DXGI_FORMAT_R32_SINT;
    case SurfaceFormat::R8G8_UINT:            return DXGI_FORMAT_R8G8_UINT;
    case SurfaceFormat::R8G8_SINT:            return DXGI_FORMAT_R8G8_SINT;
    case SurfaceFormat::R16G16_UINT:          return DXGI_FORMAT_R16G16_UINT;
    case SurfaceFormat::R16G16_SINT:          return DXGI_FORMAT_R16G16_SINT;
    case SurfaceFormat::R32G32_UINT:          return DXGI_FORMAT_R32G32_UINT;
    case SurfaceFormat::R32G32_SINT:          return DXGI_FORMAT_R32G32_SINT;
    case SurfaceFormat::R8G8B8A8_UINT:        return DXGI_FORMAT_R8G8B8A8_UINT;
    case SurfaceFormat::R8G8B8A8_SINT:        return DXGI_FORMAT_R8G8B8A8_SINT;
    case SurfaceFormat::R16G16B16A16_UINT:    return DXGI_FORMAT_R16G16B16A16_UINT;
    case SurfaceFormat::R16G16B16A16_SINT:    return DXGI_FORMAT_R16G16B16A16_SINT;
    case SurfaceFormat::R32G32B32A32_UINT:    return DXGI_FORMAT_R32G32B32A32_UINT;
    case SurfaceFormat::R32G32B32A32_SINT:    return DXGI_FORMAT_R32G32B32A32_SINT;

    // Video.
    case SurfaceFormat::NV12:             return DXGI_FORMAT_NV12;
    case SurfaceFormat::P010:             return DXGI_FORMAT_P010;
    case SurfaceFormat::YUY2:             return DXGI_FORMAT_YUY2;

    case SurfaceFormat::Unknown:
    default:                              return DXGI_FORMAT_UNKNOWN;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DXGI_FORMAT → SurfaceFormat
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline SurfaceFormat from_dxgi(DXGI_FORMAT f) noexcept
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:            return SurfaceFormat::RGBA8;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:       return SurfaceFormat::RGBA8_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM:            return SurfaceFormat::BGRA8;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:       return SurfaceFormat::BGRA8_SRGB;
    case DXGI_FORMAT_B8G8R8X8_UNORM:            return SurfaceFormat::X8R8G8B8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:        return SurfaceFormat::RGBA16F;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:        return SurfaceFormat::RGBA32F;
    case DXGI_FORMAT_R32G32B32_FLOAT:           return SurfaceFormat::RGB32F;
    case DXGI_FORMAT_R11G11B10_FLOAT:           return SurfaceFormat::R11G11B10F;
    case DXGI_FORMAT_R10G10B10A2_UNORM:         return SurfaceFormat::R10G10B10A2;
    case DXGI_FORMAT_R10G10B10A2_UINT:          return SurfaceFormat::R10G10B10A2_UINT;
    case DXGI_FORMAT_R8_UNORM:                  return SurfaceFormat::R8;
    case DXGI_FORMAT_R8G8_UNORM:                return SurfaceFormat::RG8;
    case DXGI_FORMAT_R16_FLOAT:                 return SurfaceFormat::R16F;
    case DXGI_FORMAT_R32_FLOAT:                 return SurfaceFormat::R32F;
    case DXGI_FORMAT_R16G16_FLOAT:              return SurfaceFormat::RG16F;
    case DXGI_FORMAT_R32G32_FLOAT:              return SurfaceFormat::RG32F;
    case DXGI_FORMAT_B5G6R5_UNORM:              return SurfaceFormat::B5G6R5;
    case DXGI_FORMAT_B5G5R5A1_UNORM:            return SurfaceFormat::B5G5R5A1;

    case DXGI_FORMAT_R16G16B16A16_UNORM:        return SurfaceFormat::RGBA16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_SNORM:        return SurfaceFormat::RGBA16_SNORM;
    case DXGI_FORMAT_R16G16_UNORM:              return SurfaceFormat::RG16_UNORM;
    case DXGI_FORMAT_R16G16_SNORM:              return SurfaceFormat::RG16_SNORM;
    case DXGI_FORMAT_R16_UNORM:                 return SurfaceFormat::R16_UNORM;

    case DXGI_FORMAT_BC1_UNORM:                 return SurfaceFormat::BC1;
    case DXGI_FORMAT_BC1_UNORM_SRGB:            return SurfaceFormat::BC1;
    case DXGI_FORMAT_BC2_UNORM:                 return SurfaceFormat::BC2;
    case DXGI_FORMAT_BC2_UNORM_SRGB:            return SurfaceFormat::BC2;
    case DXGI_FORMAT_BC3_UNORM:                 return SurfaceFormat::BC3;
    case DXGI_FORMAT_BC3_UNORM_SRGB:            return SurfaceFormat::BC3;
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:                 return SurfaceFormat::BC4;
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:                 return SurfaceFormat::BC5;
    case DXGI_FORMAT_BC6H_UF16:                 return SurfaceFormat::BC6H;
    case DXGI_FORMAT_BC6H_SF16:                 return SurfaceFormat::BC6H_SF16;
    case DXGI_FORMAT_BC7_UNORM:                 return SurfaceFormat::BC7;
    case DXGI_FORMAT_BC7_UNORM_SRGB:            return SurfaceFormat::BC7_SRGB;

    case DXGI_FORMAT_D16_UNORM:                 return SurfaceFormat::D16;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:         return SurfaceFormat::D24S8;
    case DXGI_FORMAT_D32_FLOAT:                 return SurfaceFormat::D32F;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:      return SurfaceFormat::D32FS8;

    case DXGI_FORMAT_R8_TYPELESS:               return SurfaceFormat::R8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:              return SurfaceFormat::R16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:              return SurfaceFormat::R32_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:         return SurfaceFormat::R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:     return SurfaceFormat::R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:     return SurfaceFormat::R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:           return SurfaceFormat::R32G32_TYPELESS;
    case DXGI_FORMAT_R32G32B32_TYPELESS:        return SurfaceFormat::R32G32B32_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:            return SurfaceFormat::R24G8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:  return SurfaceFormat::R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:         return SurfaceFormat::B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_BC1_TYPELESS:              return SurfaceFormat::BC1_TYPELESS;
    case DXGI_FORMAT_BC2_TYPELESS:              return SurfaceFormat::BC2_TYPELESS;
    case DXGI_FORMAT_BC3_TYPELESS:              return SurfaceFormat::BC3_TYPELESS;
    case DXGI_FORMAT_BC4_TYPELESS:              return SurfaceFormat::BC4_TYPELESS;
    case DXGI_FORMAT_BC5_TYPELESS:              return SurfaceFormat::BC5_TYPELESS;
    case DXGI_FORMAT_BC6H_TYPELESS:             return SurfaceFormat::BC6H_TYPELESS;
    case DXGI_FORMAT_BC7_TYPELESS:              return SurfaceFormat::BC7_TYPELESS;

    case DXGI_FORMAT_R8_UINT:                   return SurfaceFormat::R8_UINT;
    case DXGI_FORMAT_R8_SINT:                   return SurfaceFormat::R8_SINT;
    case DXGI_FORMAT_R16_UINT:                  return SurfaceFormat::R16_UINT;
    case DXGI_FORMAT_R16_SINT:                  return SurfaceFormat::R16_SINT;
    case DXGI_FORMAT_R32_UINT:                  return SurfaceFormat::R32_UINT;
    case DXGI_FORMAT_R32_SINT:                  return SurfaceFormat::R32_SINT;
    case DXGI_FORMAT_R8G8_UINT:                 return SurfaceFormat::R8G8_UINT;
    case DXGI_FORMAT_R8G8_SINT:                 return SurfaceFormat::R8G8_SINT;
    case DXGI_FORMAT_R16G16_UINT:               return SurfaceFormat::R16G16_UINT;
    case DXGI_FORMAT_R16G16_SINT:               return SurfaceFormat::R16G16_SINT;
    case DXGI_FORMAT_R32G32_UINT:               return SurfaceFormat::R32G32_UINT;
    case DXGI_FORMAT_R32G32_SINT:               return SurfaceFormat::R32G32_SINT;
    case DXGI_FORMAT_R8G8B8A8_UINT:             return SurfaceFormat::R8G8B8A8_UINT;
    case DXGI_FORMAT_R8G8B8A8_SINT:             return SurfaceFormat::R8G8B8A8_SINT;
    case DXGI_FORMAT_R16G16B16A16_UINT:         return SurfaceFormat::R16G16B16A16_UINT;
    case DXGI_FORMAT_R16G16B16A16_SINT:         return SurfaceFormat::R16G16B16A16_SINT;
    case DXGI_FORMAT_R32G32B32A32_UINT:         return SurfaceFormat::R32G32B32A32_UINT;
    case DXGI_FORMAT_R32G32B32A32_SINT:         return SurfaceFormat::R32G32B32A32_SINT;

    case DXGI_FORMAT_NV12:                      return SurfaceFormat::NV12;
    case DXGI_FORMAT_P010:                      return SurfaceFormat::P010;
    case DXGI_FORMAT_YUY2:                      return SurfaceFormat::YUY2;

    default:                                    return SurfaceFormat::Unknown;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SurfaceFormat → D3DFORMAT
//
//  D3D9 is the narrowest of the three. Anything it cannot express returns
//  D3DFMT_UNKNOWN, and the caller reports that rather than creating something
//  subtly different from what was asked for.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline D3DFORMAT to_d3d9(SurfaceFormat f) noexcept
{
    switch (f) {
    // D3D9's channel order is BGRA, so RGBA8 and BGRA8 both land on A8R8G8B8:
    // it is the only 32-bit format the API has, and the swizzle is a property
    // of how the shader reads it rather than of the surface.
    case SurfaceFormat::RGBA8:
    case SurfaceFormat::BGRA8:
    case SurfaceFormat::A8R8G8B8:
    case SurfaceFormat::RGBA8_SRGB:
    case SurfaceFormat::BGRA8_SRGB:
    case SurfaceFormat::R8G8B8A8_TYPELESS:
    case SurfaceFormat::B8G8R8A8_TYPELESS:  return D3DFMT_A8R8G8B8;

    case SurfaceFormat::X8R8G8B8:           return D3DFMT_X8R8G8B8;

    case SurfaceFormat::RGBA16F:
    case SurfaceFormat::A16B16G16R16F:
    case SurfaceFormat::R16G16B16A16_TYPELESS: return D3DFMT_A16B16G16R16F;

    case SurfaceFormat::RGBA32F:
    case SurfaceFormat::A32B32G32R32F:
    case SurfaceFormat::R32G32B32A32_TYPELESS: return D3DFMT_A32B32G32R32F;

    case SurfaceFormat::RGBA16_UNORM:       return D3DFMT_A16B16G16R16;
    case SurfaceFormat::R10G10B10A2:        return D3DFMT_A2B10G10R10;
    case SurfaceFormat::R5G6B5:
    case SurfaceFormat::B5G6R5:             return D3DFMT_R5G6B5;
    case SurfaceFormat::B5G5R5A1:           return D3DFMT_A1R5G5B5;

    case SurfaceFormat::R8:
    case SurfaceFormat::R8_TYPELESS:        return D3DFMT_L8;
    case SurfaceFormat::RG8:                return D3DFMT_A8L8;
    case SurfaceFormat::R16F:
    case SurfaceFormat::R16_TYPELESS:       return D3DFMT_R16F;
    case SurfaceFormat::R32F:
    case SurfaceFormat::R32_TYPELESS:       return D3DFMT_R32F;
    case SurfaceFormat::RG16F:              return D3DFMT_G16R16F;
    case SurfaceFormat::RG32F:
    case SurfaceFormat::R32G32_TYPELESS:    return D3DFMT_G32R32F;
    case SurfaceFormat::RG16_UNORM:         return D3DFMT_G16R16;
    case SurfaceFormat::R16_UNORM:          return D3DFMT_L16;

    case SurfaceFormat::BC1:
    case SurfaceFormat::BC1_TYPELESS:       return D3DFMT_DXT1;
    case SurfaceFormat::BC2:
    case SurfaceFormat::BC2_TYPELESS:       return D3DFMT_DXT3;
    case SurfaceFormat::BC3:
    case SurfaceFormat::BC3_TYPELESS:       return D3DFMT_DXT5;
    case SurfaceFormat::BC4:
    case SurfaceFormat::BC4_TYPELESS:       return kFmtATI1;
    case SurfaceFormat::BC5:
    case SurfaceFormat::BC5_TYPELESS:       return kFmtATI2;

    case SurfaceFormat::D16:                return D3DFMT_D16;
    case SurfaceFormat::D24S8:              return D3DFMT_D24S8;
    case SurfaceFormat::D24X8:              return D3DFMT_D24X8;
    case SurfaceFormat::D32F:               return D3DFMT_D32F_LOCKABLE;
    case SurfaceFormat::D32FS8:             return D3DFMT_D24FS8;

    case SurfaceFormat::INTZ:               return kFmtINTZ;
    case SurfaceFormat::RAWZ:               return kFmtRAWZ;
    case SurfaceFormat::DF16:               return kFmtDF16;
    case SurfaceFormat::DF24:               return kFmtDF24;

    // A D3D9 depth-stencil that a shader can read is one of the FourCC hacks
    // above, so a typeless depth pair means INTZ here.
    case SurfaceFormat::R24G8_TYPELESS:     return kFmtINTZ;

    default:                                return D3DFMT_UNKNOWN;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  D3DFORMAT → SurfaceFormat
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline SurfaceFormat from_d3d9(D3DFORMAT f) noexcept
{
    // The FourCC values are not compile-time constant expressions of the same
    // kind as the enumerators, so they are tested before the switch.
    if (f == kFmtINTZ) return SurfaceFormat::INTZ;
    if (f == kFmtRAWZ) return SurfaceFormat::RAWZ;
    if (f == kFmtDF16) return SurfaceFormat::DF16;
    if (f == kFmtDF24) return SurfaceFormat::DF24;
    if (f == kFmtATI1) return SurfaceFormat::BC4;
    if (f == kFmtATI2) return SurfaceFormat::BC5;

    switch (f) {
    case D3DFMT_A8R8G8B8:       return SurfaceFormat::A8R8G8B8;
    case D3DFMT_X8R8G8B8:       return SurfaceFormat::X8R8G8B8;
    case D3DFMT_A8B8G8R8:       return SurfaceFormat::RGBA8;
    case D3DFMT_A16B16G16R16F:  return SurfaceFormat::A16B16G16R16F;
    case D3DFMT_A32B32G32R32F:  return SurfaceFormat::A32B32G32R32F;
    case D3DFMT_A16B16G16R16:   return SurfaceFormat::RGBA16_UNORM;
    case D3DFMT_A2B10G10R10:    return SurfaceFormat::R10G10B10A2;
    case D3DFMT_R5G6B5:         return SurfaceFormat::R5G6B5;
    case D3DFMT_A1R5G5B5:       return SurfaceFormat::B5G5R5A1;
    case D3DFMT_L8:
    case D3DFMT_A8:             return SurfaceFormat::R8;
    case D3DFMT_A8L8:           return SurfaceFormat::RG8;
    case D3DFMT_R16F:           return SurfaceFormat::R16F;
    case D3DFMT_R32F:           return SurfaceFormat::R32F;
    case D3DFMT_G16R16F:        return SurfaceFormat::RG16F;
    case D3DFMT_G32R32F:        return SurfaceFormat::RG32F;
    case D3DFMT_G16R16:         return SurfaceFormat::RG16_UNORM;
    case D3DFMT_L16:            return SurfaceFormat::R16_UNORM;
    case D3DFMT_DXT1:           return SurfaceFormat::BC1;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:           return SurfaceFormat::BC2;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:           return SurfaceFormat::BC3;
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:   return SurfaceFormat::D16;
    case D3DFMT_D24S8:          return SurfaceFormat::D24S8;
    case D3DFMT_D24X8:          return SurfaceFormat::D24X8;
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:  return SurfaceFormat::D32F;
    case D3DFMT_D24FS8:         return SurfaceFormat::D32FS8;
    default:                    return SurfaceFormat::Unknown;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Size and classification
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline bool is_block_compressed(SurfaceFormat f) noexcept
{
    switch (f) {
    case SurfaceFormat::BC1: case SurfaceFormat::BC2: case SurfaceFormat::BC3:
    case SurfaceFormat::BC4: case SurfaceFormat::BC5:
    case SurfaceFormat::BC6H: case SurfaceFormat::BC6H_SF16:
    case SurfaceFormat::BC7: case SurfaceFormat::BC7_SRGB:
    case SurfaceFormat::BC1_TYPELESS: case SurfaceFormat::BC2_TYPELESS:
    case SurfaceFormat::BC3_TYPELESS: case SurfaceFormat::BC4_TYPELESS:
    case SurfaceFormat::BC5_TYPELESS: case SurfaceFormat::BC6H_TYPELESS:
    case SurfaceFormat::BC7_TYPELESS:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool is_depth(SurfaceFormat f) noexcept
{
    switch (f) {
    case SurfaceFormat::D16:  case SurfaceFormat::D24S8: case SurfaceFormat::D24X8:
    case SurfaceFormat::D32F: case SurfaceFormat::D32FS8:
    case SurfaceFormat::INTZ: case SurfaceFormat::RAWZ:
    case SurfaceFormat::DF16: case SurfaceFormat::DF24:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline bool has_stencil(SurfaceFormat f) noexcept
{
    return f == SurfaceFormat::D24S8 || f == SurfaceFormat::D32FS8;
}

// Bytes one pixel occupies. For a block-compressed format this is the bytes per
// 4x4 block instead -- 8 for BC1/BC4, 16 for the rest -- because a "pixel" is
// not addressable in those and a per-pixel answer would be a fraction.
// Callers must branch on is_block_compressed() before using it as a stride.
[[nodiscard]] inline uint32_t bytes_per_pixel(SurfaceFormat f) noexcept
{
    switch (f) {
    case SurfaceFormat::R8:  case SurfaceFormat::R8_UINT:
    case SurfaceFormat::R8_SINT: case SurfaceFormat::R8_TYPELESS:
        return 1;

    case SurfaceFormat::RG8: case SurfaceFormat::R8G8_UINT:
    case SurfaceFormat::R8G8_SINT:
    case SurfaceFormat::R16F: case SurfaceFormat::R16_UNORM:
    case SurfaceFormat::R16_UINT: case SurfaceFormat::R16_SINT:
    case SurfaceFormat::R16_TYPELESS:
    case SurfaceFormat::D16:
    case SurfaceFormat::R5G6B5: case SurfaceFormat::B5G6R5:
    case SurfaceFormat::B5G5R5A1:
    case SurfaceFormat::DF16:
        return 2;

    case SurfaceFormat::RGBA8: case SurfaceFormat::RGBA8_SRGB:
    case SurfaceFormat::BGRA8: case SurfaceFormat::BGRA8_SRGB:
    case SurfaceFormat::A8R8G8B8: case SurfaceFormat::X8R8G8B8:
    case SurfaceFormat::R8G8B8A8_TYPELESS: case SurfaceFormat::B8G8R8A8_TYPELESS:
    case SurfaceFormat::R8G8B8A8_UINT: case SurfaceFormat::R8G8B8A8_SINT:
    case SurfaceFormat::R11G11B10F:
    case SurfaceFormat::R10G10B10A2: case SurfaceFormat::R10G10B10A2_UINT:
    case SurfaceFormat::RG16F: case SurfaceFormat::RG16_UNORM:
    case SurfaceFormat::RG16_SNORM:
    case SurfaceFormat::R16G16_UINT: case SurfaceFormat::R16G16_SINT:
    case SurfaceFormat::R32F: case SurfaceFormat::R32_UINT:
    case SurfaceFormat::R32_SINT: case SurfaceFormat::R32_TYPELESS:
    case SurfaceFormat::D24S8: case SurfaceFormat::D24X8:
    case SurfaceFormat::D32F:
    case SurfaceFormat::R24G8_TYPELESS:
    case SurfaceFormat::INTZ: case SurfaceFormat::RAWZ: case SurfaceFormat::DF24:
        return 4;

    case SurfaceFormat::RGBA16F: case SurfaceFormat::A16B16G16R16F:
    case SurfaceFormat::RGBA16_UNORM: case SurfaceFormat::RGBA16_SNORM:
    case SurfaceFormat::R16G16B16A16_UINT: case SurfaceFormat::R16G16B16A16_SINT:
    case SurfaceFormat::R16G16B16A16_TYPELESS:
    case SurfaceFormat::RG32F: case SurfaceFormat::R32G32_UINT:
    case SurfaceFormat::R32G32_SINT: case SurfaceFormat::R32G32_TYPELESS:
    case SurfaceFormat::D32FS8:
    case SurfaceFormat::R32_FLOAT_X8X24_TYPELESS:
        return 8;

    case SurfaceFormat::RGB32F: case SurfaceFormat::R32G32B32_TYPELESS:
        return 12;

    case SurfaceFormat::RGBA32F: case SurfaceFormat::A32B32G32R32F:
    case SurfaceFormat::R32G32B32A32_UINT: case SurfaceFormat::R32G32B32A32_SINT:
    case SurfaceFormat::R32G32B32A32_TYPELESS:
        return 16;

    // Block sizes, per 4x4 block.
    case SurfaceFormat::BC1: case SurfaceFormat::BC1_TYPELESS:
    case SurfaceFormat::BC4: case SurfaceFormat::BC4_TYPELESS:
        return 8;

    case SurfaceFormat::BC2: case SurfaceFormat::BC2_TYPELESS:
    case SurfaceFormat::BC3: case SurfaceFormat::BC3_TYPELESS:
    case SurfaceFormat::BC5: case SurfaceFormat::BC5_TYPELESS:
    case SurfaceFormat::BC6H: case SurfaceFormat::BC6H_SF16:
    case SurfaceFormat::BC6H_TYPELESS:
    case SurfaceFormat::BC7: case SurfaceFormat::BC7_SRGB:
    case SurfaceFormat::BC7_TYPELESS:
        return 16;

    // Planar. There is no single pixel size; the caller has to walk the planes.
    case SurfaceFormat::NV12: case SurfaceFormat::P010: case SurfaceFormat::YUY2:
    case SurfaceFormat::Unknown:
    default:
        return 0;
    }
}

// Bytes one row of `width` pixels occupies, block compression accounted for.
[[nodiscard]] inline uint32_t row_pitch(SurfaceFormat f, uint32_t width) noexcept
{
    const uint32_t unit = bytes_per_pixel(f);
    if (unit == 0) return 0;
    if (is_block_compressed(f)) return ((width + 3u) / 4u) * unit;
    return width * unit;
}

// Rows a surface of `height` pixels has. One per pixel row, or one per 4-row
// block for a compressed format.
[[nodiscard]] inline uint32_t row_count(SurfaceFormat f, uint32_t height) noexcept
{
    return is_block_compressed(f) ? ((height + 3u) / 4u) : height;
}

// The typed format to view a typeless one through, for an SRV. Returns the
// format unchanged when it is already typed.
[[nodiscard]] inline DXGI_FORMAT srv_format_for(DXGI_FORMAT f) noexcept
{
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:  return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:              return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:              return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS:               return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:         return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:         return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:     return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:     return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:           return DXGI_FORMAT_R32G32_FLOAT;
    default:                                    return f;
    }
}

// The typed format to view a typeless one through, for a DSV.
[[nodiscard]] inline DXGI_FORMAT dsv_format_for(DXGI_FORMAT f) noexcept
{
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:            return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:  return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R32_TYPELESS:              return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:              return DXGI_FORMAT_D16_UNORM;
    default:                                    return f;
    }
}

}  // namespace vangfx::fmtmap
