/**
 * src/capture/writers.cpp — see writers.hpp.
 */

#include "writers.hpp"
#include "deflate.hpp"
#include "image.hpp"

#include <format_map.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace vangfx::capture {

namespace {

bool save(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "could not open " + path.string() + " for writing"; return false; }
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) { error = "could not write " + path.string(); return false; }
    return true;
}

void put_u32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
template <typename T> void put_le(std::vector<uint8_t>& v, T x) {
    uint8_t b[sizeof(T)];
    std::memcpy(b, &x, sizeof(T));
    v.insert(v.end(), b, b + sizeof(T));
}
void put_str(std::vector<uint8_t>& v, const char* s) { v.insert(v.end(), s, s + std::strlen(s) + 1); }

float linear_to_srgb(float c) noexcept {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
float srgb_to_linear(float c) noexcept {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// The whole image as RGBA floats.
bool decode_all(const ImageView& img, std::vector<float>& px) {
    px.resize(static_cast<size_t>(img.width) * img.height * 4);
    for (uint32_t y = 0; y < img.height; ++y)
        if (!decode_row(img.format, img.data + static_cast<size_t>(y) * img.row_pitch, img.width,
                        px.data() + static_cast<size_t>(y) * img.width * 4))
            return false;
    return true;
}

bool check(const ImageView& img, WriteResult& r) {
    if (!img.data || !img.width || !img.height || !img.row_pitch) { r.error = "no pixels"; return false; }
    return true;
}

// ── PNG ──────────────────────────────────────────────────────────────────────

void png_chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    put_u32be(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_u32be(out, crc32(out.data() + start, out.size() - start));
}

uint8_t paeth(int a, int b, int c) noexcept {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// Filters each row with whichever of the five filters leaves the smallest
// residuals -- the usual heuristic, and most of what PNG compression is.
std::vector<uint8_t> png_filter(const std::vector<uint8_t>& raw, uint32_t row_bytes, uint32_t rows, uint32_t bpp) {
    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(row_bytes + 1) * rows);
    std::vector<uint8_t> zero(row_bytes, 0), cand(row_bytes), best(row_bytes);
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t* cur  = raw.data() + static_cast<size_t>(y) * row_bytes;
        const uint8_t* prev = y ? cur - row_bytes : zero.data();
        uint64_t best_sum = std::numeric_limits<uint64_t>::max();
        uint8_t  best_filter = 0;
        for (uint8_t f = 0; f < 5; ++f) {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < row_bytes; ++i) {
                const int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
                uint8_t v = cur[i];
                switch (f) {
                case 1: v = uint8_t(v - a); break;
                case 2: v = uint8_t(v - b); break;
                case 3: v = uint8_t(v - ((a + b) >> 1)); break;
                case 4: v = uint8_t(v - paeth(a, b, c)); break;
                default: break;
                }
                cand[i] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (sum < best_sum) { best_sum = sum; best_filter = f; best.swap(cand); }
        }
        out.push_back(best_filter);
        out.insert(out.end(), best.begin(), best.end());
    }
    return out;
}

// ── EXR ──────────────────────────────────────────────────────────────────────

// OpenEXR's preprocessing for RLE and ZIP: bytes split into two interleaved
// halves, then each byte stored as its difference from the previous.
std::vector<uint8_t> exr_predict(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> t(in.size());
    size_t a = 0, b = (in.size() + 1) / 2;
    for (size_t i = 0; i < in.size(); ++i) {
        if (i & 1) t[b++] = in[i]; else t[a++] = in[i];
    }
    for (size_t i = t.size(); i-- > 1;) t[i] = static_cast<uint8_t>(int(t[i]) - int(t[i - 1]) + 128);
    return t;
}

std::vector<uint8_t> exr_rle(const std::vector<uint8_t>& in) {
    constexpr int kMinRun = 3, kMaxRun = 127;
    std::vector<uint8_t> out;
    out.reserve(in.size());
    const uint8_t* p = in.data();
    const uint8_t* end = p + in.size();
    const uint8_t* start = p;
    const uint8_t* run_end = p + 1;
    while (start < end) {
        while (run_end < end && *start == *run_end && run_end - start - 1 < kMaxRun) ++run_end;
        if (run_end - start >= kMinRun) {
            out.push_back(static_cast<uint8_t>((run_end - start) - 1));
            out.push_back(*start);
            start = run_end;
        } else {
            while (run_end < end &&
                   ((run_end + 1 >= end || *run_end != *(run_end + 1)) || (run_end + 2 >= end || *(run_end + 1) != *(run_end + 2))) &&
                   run_end - start < kMaxRun)
                ++run_end;
            out.push_back(static_cast<uint8_t>(start - run_end));
            while (start < run_end) out.push_back(*start++);
        }
        ++run_end;
    }
    return out;
}

void exr_attr(std::vector<uint8_t>& h, const char* name, const char* type, const std::vector<uint8_t>& data) {
    put_str(h, name);
    put_str(h, type);
    put_le<int32_t>(h, static_cast<int32_t>(data.size()));
    h.insert(h.end(), data.begin(), data.end());
}

} // namespace

WriteResult write_png(const std::filesystem::path& path, const ImageView& img) {
    WriteResult r;
    if (!check(img, r)) return r;
    const Layout l = layout_of(img.format);
    if (!l.decodable()) { r.error = "PNG: this format cannot be converted to pixels (block-compressed or planar)"; return r; }
    std::vector<float> px;
    if (!decode_all(img, px)) { r.error = "PNG: decode failed"; return r; }
    const size_t n = static_cast<size_t>(img.width) * img.height;

    // Which channels to write: colour as RGB, alpha only when it varies (a
    // constant alpha -- often 0 in a back buffer -- makes an invisible PNG).
    bool alpha = false;
    if (l.has_alpha) {
        for (size_t i = 1; i < n && !alpha; ++i) alpha = px[i * 4 + 3] != px[3];
    }
    const bool gray   = l.channels == 1;
    const bool eight  = l.kind == ChannelKind::Unorm && l.bits <= 8;
    const int  colour_type = gray ? 0 : alpha ? 6 : 2;
    const uint32_t chans = gray ? 1u : alpha ? 4u : 3u;

    // Deeper sources to [0,1] for 16 bits.
    if (!eight) {
        const bool colour_like = l.kind == ChannelKind::Float && l.channels >= 3;
        if (l.kind == ChannelKind::Snorm) {
            for (auto& v : px) v = v * 0.5f + 0.5f;
            r.note = "signed values mapped from [-1,1] to [0,1]";
        } else if (colour_like) {
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < 3; ++c) px[i * 4 + c] = linear_to_srgb(px[i * 4 + c]);
            r.note = "linear float colour converted to sRGB and clipped to [0,1]; the EXR keeps the values";
        } else if (l.kind != ChannelKind::Unorm) {
            // Depth, one- and two-channel floats, integers: each channel over its
            // own range, so a depth buffer near 1 everywhere still shows.
            for (uint32_t c = 0; c < l.channels && c < 4; ++c) {
                float lo = INFINITY, hi = -INFINITY;
                for (size_t i = 0; i < n; ++i) {
                    const float v = px[i * 4 + c];
                    if (std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
                }
                const float span = hi > lo ? hi - lo : 1.0f;
                for (size_t i = 0; i < n; ++i) {
                    const float v = px[i * 4 + c];
                    px[i * 4 + c] = std::isfinite(v) ? (v - (hi > lo ? lo : 0.0f)) / span : 0.0f;
                }
                if (c == 0) {
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "values stretched from [%g, %g] to [0,1] per channel; the EXR keeps the values",
                                  double(lo), double(hi));
                    r.note = buf;
                }
            }
        }
    }

    const uint32_t bytes_per_sample = eight ? 1 : 2;
    const uint32_t row_bytes = img.width * chans * bytes_per_sample;
    std::vector<uint8_t> raw(static_cast<size_t>(row_bytes) * img.height);
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        const float* p = &px[i * 4];
        const uint32_t order[4] = { 0, 1, 2, 3 };
        for (uint32_t c = 0; c < chans; ++c) {
            const float v = std::clamp(std::isfinite(p[order[c]]) ? p[order[c]] : 0.0f, 0.0f, 1.0f);
            if (eight) {
                raw[o++] = static_cast<uint8_t>(std::lround(v * 255.0f));
            } else {
                const uint16_t s = static_cast<uint16_t>(std::lround(v * 65535.0f));
                raw[o++] = static_cast<uint8_t>(s >> 8);
                raw[o++] = static_cast<uint8_t>(s);
            }
        }
    }

    std::vector<uint8_t> file = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    std::vector<uint8_t> ihdr;
    put_u32be(ihdr, img.width);
    put_u32be(ihdr, img.height);
    ihdr.push_back(static_cast<uint8_t>(bytes_per_sample * 8));
    ihdr.push_back(static_cast<uint8_t>(colour_type));
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    png_chunk(file, "IHDR", ihdr);
    const auto filtered = png_filter(raw, row_bytes, img.height, chans * bytes_per_sample);
    png_chunk(file, "IDAT", zlib_compress(filtered.data(), filtered.size(), 6));
    png_chunk(file, "IEND", {});
    r.ok = save(path, file, r.error);
    return r;
}

WriteResult write_exr(const std::filesystem::path& path, const ImageView& img, ExrCompression compression) {
    WriteResult r;
    if (!check(img, r)) return r;
    const Layout l = layout_of(img.format);
    if (!l.decodable()) { r.error = "EXR: this format cannot be converted to pixels (block-compressed or planar)"; return r; }
    std::vector<float> px;
    if (!decode_all(img, px)) { r.error = "EXR: decode failed"; return r; }
    if (l.srgb) {
        const size_t n = static_cast<size_t>(img.width) * img.height;
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c) px[i * 4 + c] = srgb_to_linear(px[i * 4 + c]);
        r.note = "sRGB-encoded values linearised";
    }

    // Channels, in the alphabetical order the file stores them: which of the
    // decoded RGBA each is, and its name. One channel -- depth included -- is
    // Y, which every reader shows as greyscale (OpenCV, for one, drops a lone
    // Z channel entirely).
    struct Ch { const char* name; int src; };
    std::vector<Ch> chans;
    if (l.channels == 1)                  chans = { { "Y", 0 } };
    else if (l.channels == 2)             chans = { { "G", 1 }, { "R", 0 } };
    else if (l.channels == 3 || !l.has_alpha) chans = { { "B", 2 }, { "G", 1 }, { "R", 0 } };
    else                                  chans = { { "A", 3 }, { "B", 2 }, { "G", 1 }, { "R", 0 } };
    const bool as_float = l.bits > 16 || l.kind == ChannelKind::Depth || l.kind == ChannelKind::UInt ||
                          l.kind == ChannelKind::SInt;
    const int32_t  pixel_type = as_float ? 2 : 1;   // FLOAT : HALF
    const uint32_t sample     = as_float ? 4 : 2;

    std::vector<uint8_t> h = { 0x76, 0x2F, 0x31, 0x01, 2, 0, 0, 0 };
    {
        std::vector<uint8_t> list;
        for (const Ch& c : chans) {
            put_str(list, c.name);
            put_le<int32_t>(list, pixel_type);
            list.push_back(0); list.push_back(0); list.push_back(0); list.push_back(0);   // pLinear, reserved
            put_le<int32_t>(list, 1);
            put_le<int32_t>(list, 1);
        }
        list.push_back(0);
        exr_attr(h, "channels", "chlist", list);
    }
    exr_attr(h, "compression", "compression", { static_cast<uint8_t>(compression) });
    std::vector<uint8_t> box;
    put_le<int32_t>(box, 0); put_le<int32_t>(box, 0);
    put_le<int32_t>(box, static_cast<int32_t>(img.width) - 1); put_le<int32_t>(box, static_cast<int32_t>(img.height) - 1);
    exr_attr(h, "dataWindow", "box2i", box);
    exr_attr(h, "displayWindow", "box2i", box);
    exr_attr(h, "lineOrder", "lineOrder", { 0 });
    std::vector<uint8_t> one;   put_le<float>(one, 1.0f);
    std::vector<uint8_t> centre; put_le<float>(centre, 0.0f); put_le<float>(centre, 0.0f);
    exr_attr(h, "pixelAspectRatio", "float", one);
    exr_attr(h, "screenWindowCenter", "v2f", centre);
    exr_attr(h, "screenWindowWidth", "float", one);
    h.push_back(0);

    const uint32_t lines = compression == ExrCompression::Zip ? 16u : 1u;
    const uint32_t chunk_count = (img.height + lines - 1) / lines;
    const size_t   table_at = h.size();
    h.resize(h.size() + static_cast<size_t>(chunk_count) * 8);

    std::vector<uint8_t> block;
    for (uint32_t chunk = 0; chunk < chunk_count; ++chunk) {
        const uint32_t y0 = chunk * lines;
        const uint32_t y1 = std::min(img.height, y0 + lines);
        block.clear();
        for (uint32_t y = y0; y < y1; ++y) {
            for (const Ch& c : chans) {
                for (uint32_t x = 0; x < img.width; ++x) {
                    const float v = px[(static_cast<size_t>(y) * img.width + x) * 4 + c.src];
                    if (as_float) put_le<float>(block, v);
                    else          put_le<uint16_t>(block, float_to_half(v));
                }
            }
        }
        std::vector<uint8_t> packed;
        if (compression == ExrCompression::Rle)       packed = exr_rle(exr_predict(block));
        else if (compression != ExrCompression::None) {
            const auto pre = exr_predict(block);
            packed = zlib_compress(pre.data(), pre.size(), 6);
        }
        // A chunk that does not shrink is stored as it is; the reader knows by
        // the size.
        const std::vector<uint8_t>& data = (compression == ExrCompression::None || packed.size() >= block.size()) ? block : packed;
        const uint64_t offset = h.size();
        std::memcpy(h.data() + table_at + static_cast<size_t>(chunk) * 8, &offset, 8);
        put_le<int32_t>(h, static_cast<int32_t>(y0));
        put_le<int32_t>(h, static_cast<int32_t>(data.size()));
        h.insert(h.end(), data.begin(), data.end());
    }
    (void)sample;
    r.ok = save(path, h, r.error);
    return r;
}

WriteResult write_dds(const std::filesystem::path& path, const ImageView& img) {
    WriteResult r;
    if (!check(img, r)) return r;
    const DXGI_FORMAT dxgi = fmtmap::to_dxgi(img.format);
    const uint32_t    pitch = fmtmap::row_pitch(img.format, img.width);
    const uint32_t    rows  = fmtmap::row_count(img.format, img.height);
    if (dxgi == DXGI_FORMAT_UNKNOWN || !pitch) { r.error = "DDS: the format has no DXGI equivalent"; return r; }

    std::vector<uint8_t> f;
    put_le<uint32_t>(f, 0x20534444u);                      // "DDS "
    put_le<uint32_t>(f, 124);
    put_le<uint32_t>(f, 0x1 | 0x2 | 0x4 | 0x1000 | (fmtmap::is_block_compressed(img.format) ? 0x80000 : 0x8));
    put_le<uint32_t>(f, img.height);
    put_le<uint32_t>(f, img.width);
    put_le<uint32_t>(f, fmtmap::is_block_compressed(img.format) ? pitch * rows : pitch);
    put_le<uint32_t>(f, 0);                                // depth
    put_le<uint32_t>(f, 1);                                // mip count
    for (int i = 0; i < 11; ++i) put_le<uint32_t>(f, 0);
    put_le<uint32_t>(f, 32);                               // pixel format: size
    put_le<uint32_t>(f, 0x4);                              // DDPF_FOURCC
    put_le<uint32_t>(f, 0x30315844u);                      // "DX10"
    for (int i = 0; i < 5; ++i) put_le<uint32_t>(f, 0);
    put_le<uint32_t>(f, 0x1000);                           // DDSCAPS_TEXTURE
    for (int i = 0; i < 4; ++i) put_le<uint32_t>(f, 0);
    put_le<uint32_t>(f, static_cast<uint32_t>(dxgi));      // DDS_HEADER_DXT10
    put_le<uint32_t>(f, 3);                                // TEXTURE2D
    put_le<uint32_t>(f, 0);
    put_le<uint32_t>(f, 1);                                // array size
    put_le<uint32_t>(f, 0);
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t* row = img.data + static_cast<size_t>(y) * img.row_pitch;
        f.insert(f.end(), row, row + pitch);
    }
    r.ok = save(path, f, r.error);
    return r;
}

WriteResult write_raw(const std::filesystem::path& path, const ImageView& img) {
    WriteResult r;
    if (!check(img, r)) return r;
    uint32_t pitch = fmtmap::row_pitch(img.format, img.width);
    uint32_t rows  = fmtmap::row_count(img.format, img.height);
    if (!pitch) { pitch = img.row_pitch; rows = img.height; }
    std::vector<uint8_t> f;
    f.reserve(static_cast<size_t>(pitch) * rows);
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t* row = img.data + static_cast<size_t>(y) * img.row_pitch;
        f.insert(f.end(), row, row + std::min(pitch, img.row_pitch));
    }
    r.ok = save(path, f, r.error);
    return r;
}

}  // namespace vangfx::capture
