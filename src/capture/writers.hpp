#pragma once

/**
 * src/capture/writers.hpp — a captured surface to a file: PNG, OpenEXR, DDS or
 * raw bytes. Every writer is VanGFX's own (the EXR one replaced a vendored
 * tinyexr that never compiled); none needs COM or anything beyond the C++
 * standard library, so they run on any thread.
 *
 *   PNG  what a viewer shows. 8-bit sources stay 8-bit and exact; deeper,
 *        float, signed, integer and depth sources become 16-bit, with float
 *        colour taken from linear to sRGB and data channels (motion vectors,
 *        depth, IDs) stretched over their own range so they can be seen.
 *        The Result's note says which.
 *   EXR  the values. HALF for sources of 16 bits or fewer, FLOAT for 32-bit
 *        floats and depth; sRGB-encoded sources linearised. Scanline, with
 *        NONE, RLE, ZIPS or ZIP compression.
 *   DDS  the bytes, under a DX10 header naming the exact DXGI format.
 *   Raw  the bytes, rows packed.
 */

#include <vangfx/resources/format.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace vangfx::capture {

struct ImageView {
    SurfaceFormat  format    = SurfaceFormat::Unknown;
    uint32_t       width     = 0;
    uint32_t       height    = 0;
    uint32_t       row_pitch = 0;   // bytes from one row to the next
    const uint8_t* data      = nullptr;
};

struct WriteResult {
    bool        ok = false;
    std::string error;   // why not, when !ok
    std::string note;    // how the pixels were converted, when they were
};

enum class ExrCompression : uint8_t { None = 0, Rle = 1, Zips = 2, Zip = 3 };

WriteResult write_png(const std::filesystem::path& path, const ImageView& img);
WriteResult write_exr(const std::filesystem::path& path, const ImageView& img, ExrCompression c = ExrCompression::Zip);
WriteResult write_dds(const std::filesystem::path& path, const ImageView& img);
WriteResult write_raw(const std::filesystem::path& path, const ImageView& img);

}  // namespace vangfx::capture
