/**
 * src/capture/image.cpp — see image.hpp.
 */

#include "image.hpp"

#include <cmath>
#include <cstring>

namespace vangfx::capture {

namespace {

template <typename T> T load(const uint8_t* p) noexcept { T v; std::memcpy(&v, p, sizeof v); return v; }

// The packed float formats of R11G11B10: 5-bit exponent (bias 15), no sign.
float unpack_small_float(uint32_t bits, uint32_t mantissa_bits) noexcept {
    const uint32_t e = (bits >> mantissa_bits) & 0x1Fu;
    const uint32_t m = bits & ((1u << mantissa_bits) - 1u);
    const int      shift = static_cast<int>(mantissa_bits);
    if (e == 0)  return std::ldexp(static_cast<float>(m), -14 - shift);
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(static_cast<float>((1u << mantissa_bits) | m), static_cast<int>(e) - 15 - shift);
}

float unorm(uint32_t v, uint32_t bits) noexcept { return static_cast<float>(v) / static_cast<float>((1ull << bits) - 1ull); }
float snorm(int32_t v, uint32_t bits) noexcept {
    const float f = static_cast<float>(v) / static_cast<float>((1 << (bits - 1)) - 1);
    return f < -1.0f ? -1.0f : f;
}

Layout make(ChannelKind k, uint8_t ch, uint8_t bits, bool alpha, uint32_t bytes, bool srgb = false) noexcept {
    Layout l;
    l.kind = k; l.channels = ch; l.bits = bits; l.has_alpha = alpha; l.bytes = bytes; l.srgb = srgb;
    return l;
}

} // namespace

float half_to_float(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h) & 0x8000u) << 16;
    uint32_t       exp  = (h >> 10) & 0x1Fu;
    uint32_t       mant = h & 0x3FFu;
    uint32_t       out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            int e = -1;
            do { ++e; mant <<= 1; } while (!(mant & 0x400u));
            mant &= 0x3FFu;
            out = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &out, 4);
    return f;
}

uint16_t float_to_half(float f) noexcept {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t exp  = (x >> 23) & 0xFFu;
    uint32_t       mant = x & 0x7FFFFFu;
    if (exp == 0xFF) return static_cast<uint16_t>(sign | 0x7C00u | (mant ? (0x200u | (mant >> 13)) : 0u));
    const int e = static_cast<int>(exp) - 127 + 15;
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const uint32_t shift   = static_cast<uint32_t>(14 - e);
        uint32_t       half_m  = mant >> shift;
        const uint32_t rem     = mant & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (half_m & 1u))) ++half_m;
        return static_cast<uint16_t>(sign | half_m);
    }
    uint32_t       half = sign | (static_cast<uint32_t>(e) << 10) | (mant >> 13);
    const uint32_t rem  = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;   // a carry into the exponent is correct
    return static_cast<uint16_t>(half);
}

Layout layout_of(SurfaceFormat f) noexcept {
    using K = ChannelKind;
    using S = SurfaceFormat;
    switch (f) {
    case S::RGBA8: case S::BGRA8: case S::A8R8G8B8:
    case S::R8G8B8A8_TYPELESS: case S::B8G8R8A8_TYPELESS:  return make(K::Unorm, 4, 8, true, 4);
    case S::RGBA8_SRGB: case S::BGRA8_SRGB:                  return make(K::Unorm, 4, 8, true, 4, true);
    case S::X8R8G8B8:                                        return make(K::Unorm, 3, 8, false, 4);
    case S::R8: case S::R8_TYPELESS:                         return make(K::Unorm, 1, 8, false, 1);
    case S::RG8:                                             return make(K::Unorm, 2, 8, false, 2);
    case S::R10G10B10A2:                                     return make(K::Unorm, 4, 10, true, 4);
    case S::R10G10B10A2_UINT:                                return make(K::UInt, 4, 10, true, 4);
    case S::R11G11B10F:                                      return make(K::Float, 3, 11, false, 4);
    case S::RGBA16F: case S::A16B16G16R16F:
    case S::R16G16B16A16_TYPELESS:                           return make(K::Float, 4, 16, true, 8);
    case S::RGBA32F: case S::A32B32G32R32F:
    case S::R32G32B32A32_TYPELESS:                           return make(K::Float, 4, 32, true, 16);
    case S::RGB32F: case S::R32G32B32_TYPELESS:              return make(K::Float, 3, 32, false, 12);
    case S::RG16F:                                           return make(K::Float, 2, 16, false, 4);
    case S::RG32F: case S::R32G32_TYPELESS:                  return make(K::Float, 2, 32, false, 8);
    case S::R16F:                                            return make(K::Float, 1, 16, false, 2);
    case S::R32F: case S::R32_TYPELESS:                      return make(K::Float, 1, 32, false, 4);
    case S::R16_UNORM: case S::R16_TYPELESS:                 return make(K::Unorm, 1, 16, false, 2);
    case S::RG16_UNORM:                                      return make(K::Unorm, 2, 16, false, 4);
    case S::RGBA16_UNORM:                                    return make(K::Unorm, 4, 16, true, 8);
    case S::RG16_SNORM:                                      return make(K::Snorm, 2, 16, false, 4);
    case S::RGBA16_SNORM:                                    return make(K::Snorm, 4, 16, true, 8);
    case S::B5G6R5: case S::R5G6B5:                          return make(K::Unorm, 3, 6, false, 2);
    case S::B5G5R5A1:                                        return make(K::Unorm, 4, 5, true, 2);

    case S::R8_UINT:            return make(K::UInt, 1, 8, false, 1);
    case S::R8_SINT:            return make(K::SInt, 1, 8, false, 1);
    case S::R16_UINT:           return make(K::UInt, 1, 16, false, 2);
    case S::R16_SINT:           return make(K::SInt, 1, 16, false, 2);
    case S::R32_UINT:           return make(K::UInt, 1, 32, false, 4);
    case S::R32_SINT:           return make(K::SInt, 1, 32, false, 4);
    case S::R8G8_UINT:          return make(K::UInt, 2, 8, false, 2);
    case S::R8G8_SINT:          return make(K::SInt, 2, 8, false, 2);
    case S::R16G16_UINT:        return make(K::UInt, 2, 16, false, 4);
    case S::R16G16_SINT:        return make(K::SInt, 2, 16, false, 4);
    case S::R32G32_UINT:        return make(K::UInt, 2, 32, false, 8);
    case S::R32G32_SINT:        return make(K::SInt, 2, 32, false, 8);
    case S::R8G8B8A8_UINT:      return make(K::UInt, 4, 8, true, 4);
    case S::R8G8B8A8_SINT:      return make(K::SInt, 4, 8, true, 4);
    case S::R16G16B16A16_UINT:  return make(K::UInt, 4, 16, true, 8);
    case S::R16G16B16A16_SINT:  return make(K::SInt, 4, 16, true, 8);
    case S::R32G32B32A32_UINT:  return make(K::UInt, 4, 32, true, 16);
    case S::R32G32B32A32_SINT:  return make(K::SInt, 4, 32, true, 16);

    case S::D16:                                             return make(K::Depth, 1, 16, false, 2);
    case S::D24S8: case S::D24X8: case S::R24G8_TYPELESS:   return make(K::Depth, 1, 24, false, 4);
    case S::D32F:                                            return make(K::Depth, 1, 32, false, 4);
    case S::D32FS8: case S::R32_FLOAT_X8X24_TYPELESS:        return make(K::Depth, 1, 32, false, 8);
    default:                                                 return {};
    }
}

bool decode_row(SurfaceFormat f, const uint8_t* row, uint32_t width, float* out) noexcept {
    using S = SurfaceFormat;
    const Layout l = layout_of(f);
    if (!l.decodable() || !row || !out) return false;
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* p = row + static_cast<size_t>(x) * l.bytes;
        float r = 0, g = 0, b = 0, a = 1;
        switch (f) {
        case S::RGBA8: case S::RGBA8_SRGB: case S::R8G8B8A8_TYPELESS:
            r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f; a = p[3] / 255.0f; break;
        case S::BGRA8: case S::BGRA8_SRGB: case S::A8R8G8B8: case S::B8G8R8A8_TYPELESS:
            r = p[2] / 255.0f; g = p[1] / 255.0f; b = p[0] / 255.0f; a = p[3] / 255.0f; break;
        case S::X8R8G8B8:
            r = p[2] / 255.0f; g = p[1] / 255.0f; b = p[0] / 255.0f; break;
        case S::R8: case S::R8_TYPELESS: r = p[0] / 255.0f; break;
        case S::RG8: r = p[0] / 255.0f; g = p[1] / 255.0f; break;
        case S::R10G10B10A2: case S::R10G10B10A2_UINT: {
            const uint32_t v = load<uint32_t>(p);
            const bool     u = f == S::R10G10B10A2_UINT;
            r = u ? float(v & 0x3FF) : unorm(v & 0x3FF, 10);
            g = u ? float((v >> 10) & 0x3FF) : unorm((v >> 10) & 0x3FF, 10);
            b = u ? float((v >> 20) & 0x3FF) : unorm((v >> 20) & 0x3FF, 10);
            a = u ? float(v >> 30) : unorm(v >> 30, 2);
            break;
        }
        case S::R11G11B10F: {
            const uint32_t v = load<uint32_t>(p);
            r = unpack_small_float(v & 0x7FF, 6);
            g = unpack_small_float((v >> 11) & 0x7FF, 6);
            b = unpack_small_float((v >> 22) & 0x3FF, 5);
            break;
        }
        case S::RGBA16F: case S::A16B16G16R16F: case S::R16G16B16A16_TYPELESS:
            r = half_to_float(load<uint16_t>(p)); g = half_to_float(load<uint16_t>(p + 2));
            b = half_to_float(load<uint16_t>(p + 4)); a = half_to_float(load<uint16_t>(p + 6)); break;
        case S::RGBA32F: case S::A32B32G32R32F: case S::R32G32B32A32_TYPELESS:
            r = load<float>(p); g = load<float>(p + 4); b = load<float>(p + 8); a = load<float>(p + 12); break;
        case S::RGB32F: case S::R32G32B32_TYPELESS:
            r = load<float>(p); g = load<float>(p + 4); b = load<float>(p + 8); break;
        case S::RG16F: r = half_to_float(load<uint16_t>(p)); g = half_to_float(load<uint16_t>(p + 2)); break;
        case S::RG32F: case S::R32G32_TYPELESS: r = load<float>(p); g = load<float>(p + 4); break;
        case S::R16F: r = half_to_float(load<uint16_t>(p)); break;
        case S::R32F: case S::R32_TYPELESS: r = load<float>(p); break;
        case S::R16_UNORM: case S::R16_TYPELESS: r = unorm(load<uint16_t>(p), 16); break;
        case S::RG16_UNORM: r = unorm(load<uint16_t>(p), 16); g = unorm(load<uint16_t>(p + 2), 16); break;
        case S::RGBA16_UNORM:
            r = unorm(load<uint16_t>(p), 16); g = unorm(load<uint16_t>(p + 2), 16);
            b = unorm(load<uint16_t>(p + 4), 16); a = unorm(load<uint16_t>(p + 6), 16); break;
        case S::RG16_SNORM: r = snorm(load<int16_t>(p), 16); g = snorm(load<int16_t>(p + 2), 16); break;
        case S::RGBA16_SNORM:
            r = snorm(load<int16_t>(p), 16); g = snorm(load<int16_t>(p + 2), 16);
            b = snorm(load<int16_t>(p + 4), 16); a = snorm(load<int16_t>(p + 6), 16); break;
        case S::B5G6R5: case S::R5G6B5: {
            const uint32_t v = load<uint16_t>(p);
            b = unorm(v & 0x1F, 5); g = unorm((v >> 5) & 0x3F, 6); r = unorm((v >> 11) & 0x1F, 5); break;
        }
        case S::B5G5R5A1: {
            const uint32_t v = load<uint16_t>(p);
            b = unorm(v & 0x1F, 5); g = unorm((v >> 5) & 0x1F, 5); r = unorm((v >> 10) & 0x1F, 5); a = float(v >> 15); break;
        }
        case S::R8_UINT:  r = float(p[0]); break;
        case S::R8_SINT:  r = float(load<int8_t>(p)); break;
        case S::R16_UINT: r = float(load<uint16_t>(p)); break;
        case S::R16_SINT: r = float(load<int16_t>(p)); break;
        case S::R32_UINT: r = float(load<uint32_t>(p)); break;
        case S::R32_SINT: r = float(load<int32_t>(p)); break;
        case S::R8G8_UINT: r = float(p[0]); g = float(p[1]); break;
        case S::R8G8_SINT: r = float(load<int8_t>(p)); g = float(load<int8_t>(p + 1)); break;
        case S::R16G16_UINT: r = float(load<uint16_t>(p)); g = float(load<uint16_t>(p + 2)); break;
        case S::R16G16_SINT: r = float(load<int16_t>(p)); g = float(load<int16_t>(p + 2)); break;
        case S::R32G32_UINT: r = float(load<uint32_t>(p)); g = float(load<uint32_t>(p + 4)); break;
        case S::R32G32_SINT: r = float(load<int32_t>(p)); g = float(load<int32_t>(p + 4)); break;
        case S::R8G8B8A8_UINT: r = float(p[0]); g = float(p[1]); b = float(p[2]); a = float(p[3]); break;
        case S::R8G8B8A8_SINT:
            r = float(load<int8_t>(p)); g = float(load<int8_t>(p + 1)); b = float(load<int8_t>(p + 2)); a = float(load<int8_t>(p + 3)); break;
        case S::R16G16B16A16_UINT:
            r = float(load<uint16_t>(p)); g = float(load<uint16_t>(p + 2)); b = float(load<uint16_t>(p + 4)); a = float(load<uint16_t>(p + 6)); break;
        case S::R16G16B16A16_SINT:
            r = float(load<int16_t>(p)); g = float(load<int16_t>(p + 2)); b = float(load<int16_t>(p + 4)); a = float(load<int16_t>(p + 6)); break;
        case S::R32G32B32A32_UINT:
            r = float(load<uint32_t>(p)); g = float(load<uint32_t>(p + 4)); b = float(load<uint32_t>(p + 8)); a = float(load<uint32_t>(p + 12)); break;
        case S::R32G32B32A32_SINT:
            r = float(load<int32_t>(p)); g = float(load<int32_t>(p + 4)); b = float(load<int32_t>(p + 8)); a = float(load<int32_t>(p + 12)); break;
        // Depth as a readback returns it: 24-bit UNORM in the low bits beside an
        // 8-bit stencil; a float, then the stencil in the next dword.
        case S::D16: r = unorm(load<uint16_t>(p), 16); break;
        case S::D24S8: case S::D24X8: case S::R24G8_TYPELESS: r = unorm(load<uint32_t>(p) & 0xFFFFFFu, 24); break;
        case S::D32F: case S::D32FS8: case S::R32_FLOAT_X8X24_TYPELESS: r = load<float>(p); break;
        default: return false;
        }
        out[x * 4 + 0] = r; out[x * 4 + 1] = g; out[x * 4 + 2] = b; out[x * 4 + 3] = a;
    }
    return true;
}

}  // namespace vangfx::capture
