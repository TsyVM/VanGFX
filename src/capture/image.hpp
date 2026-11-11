#pragma once

/**
 * src/capture/image.hpp — reading a surface's pixels whatever format they are
 * in: what a format holds (layout_of) and one row decoded to RGBA floats
 * (decode_row), for the writers that have to turn a render target's bytes into
 * a PNG or an EXR.
 *
 * Covers every colour format a target can be rendered in on the three APIs --
 * UNORM, SNORM, sRGB, float, the packed 10/11-bit ones, integer -- the depth
 * formats as a readback returns them, and the D3D9 spellings of each.
 * Block-compressed and planar video formats are not decoded: nothing renders
 * into them, and their bytes still go out untouched as DDS or Raw.
 */

#include <vangfx/resources/format.hpp>

#include <cstdint>

namespace vangfx::capture {

enum class ChannelKind : uint8_t {
    None,    // not decodable
    Unorm,   // [0,1]
    Snorm,   // [-1,1]
    Float,   // any float
    UInt,    // integer, as its value
    SInt,
    Depth,   // [0,1] (or float depth as stored)
};

struct Layout {
    ChannelKind kind      = ChannelKind::None;
    uint8_t     channels  = 0;      // meaningful channels, RGBA order after decoding
    uint8_t     bits      = 0;      // widest channel
    bool        has_alpha = false;
    bool        srgb      = false;  // stored sRGB-encoded
    uint32_t    bytes     = 0;      // per pixel
    [[nodiscard]] bool decodable() const noexcept { return kind != ChannelKind::None; }
};

[[nodiscard]] Layout layout_of(SurfaceFormat fmt) noexcept;

// One row of `width` pixels to `width * 4` floats, R G B A. Channels a format
// lacks read 0 (colour) and 1 (alpha). False when the format is not decodable.
bool decode_row(SurfaceFormat fmt, const uint8_t* row, uint32_t width, float* rgba) noexcept;

[[nodiscard]] float    half_to_float(uint16_t h) noexcept;
[[nodiscard]] uint16_t float_to_half(float f) noexcept;   // round to nearest even; inf/nan kept

}  // namespace vangfx::capture
