/**
 * tests/capture_writer_tests.cpp — the capture file writers, on their own.
 *
 * Each format gets a pattern that uses its range (gradients, negatives, values
 * above 1 for floats) and is written as PNG, EXR (all four compressions), DDS
 * and Raw into %TEMP%\vgx_capture_writers\, next to the exact decoded values
 * (<name>.ref: width, height, then RGBA floats). The PNGs are decoded here with
 * WIC -- a decoder VanGFX did not write -- and compared; tools/verify_capture.py
 * reads the EXRs with OpenCV's OpenEXR and the PNGs with PIL and compares both
 * with the .ref files.
 */

#include "harness.hpp"
#include <capture/image.hpp>
#include <capture/writers.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace vgx;
namespace cap = vangfx::capture;
using vangfx::SurfaceFormat;

namespace {

std::filesystem::path out_dir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    auto d = std::filesystem::path(tmp) / L"vgx_capture_writers";
    std::filesystem::create_directories(d);
    return d;
}

template <typename T> void put(std::vector<uint8_t>& v, T x) {
    uint8_t b[sizeof(T)];
    std::memcpy(b, &x, sizeof(T));
    v.insert(v.end(), b, b + sizeof(T));
}

// Pixel (x, y) of a W x H pattern, as the value each channel should decode to.
struct Pattern {
    uint32_t w = 37, h = 23;   // odd sizes: rows that are not multiples of anything
    float r(uint32_t x, uint32_t) const { return float(x) / float(w - 1); }
    float g(uint32_t, uint32_t y) const { return float(y) / float(h - 1); }
    float b(uint32_t x, uint32_t y) const { return float((x * 7 + y * 3) % 16) / 15.0f; }
    float a(uint32_t x, uint32_t y) const { return (x + y) % 3 == 0 ? 1.0f : 0.5f; }
};

// Encodes the pattern in `fmt`. Float formats get values over 1 and below 0
// where they can hold them.
std::vector<uint8_t> encode(SurfaceFormat fmt, const Pattern& p) {
    std::vector<uint8_t> v;
    for (uint32_t y = 0; y < p.h; ++y) {
        for (uint32_t x = 0; x < p.w; ++x) {
            const float r = p.r(x, y), g = p.g(x, y), b = p.b(x, y), a = p.a(x, y);
            auto u8 = [](float f) { return static_cast<uint8_t>(std::lround(f * 255.0f)); };
            auto u16 = [](float f) { return static_cast<uint16_t>(std::lround(f * 65535.0f)); };
            switch (fmt) {
            case SurfaceFormat::RGBA8: case SurfaceFormat::RGBA8_SRGB:
                v.push_back(u8(r)); v.push_back(u8(g)); v.push_back(u8(b)); v.push_back(u8(a)); break;
            case SurfaceFormat::BGRA8: case SurfaceFormat::A8R8G8B8:
                v.push_back(u8(b)); v.push_back(u8(g)); v.push_back(u8(r)); v.push_back(u8(a)); break;
            case SurfaceFormat::X8R8G8B8:
                v.push_back(u8(b)); v.push_back(u8(g)); v.push_back(u8(r)); v.push_back(0x5A); break;
            case SurfaceFormat::R8: v.push_back(u8(r)); break;
            case SurfaceFormat::RG8: v.push_back(u8(r)); v.push_back(u8(g)); break;
            case SurfaceFormat::R10G10B10A2: {
                const uint32_t q = uint32_t(std::lround(r * 1023)) | (uint32_t(std::lround(g * 1023)) << 10) |
                                   (uint32_t(std::lround(b * 1023)) << 20) | (uint32_t(std::lround(a * 3)) << 30);
                put(v, q); break;
            }
            case SurfaceFormat::RGBA16F:
                put(v, cap::float_to_half(r * 4 - 1)); put(v, cap::float_to_half(g * 8));
                put(v, cap::float_to_half(b)); put(v, cap::float_to_half(a)); break;
            case SurfaceFormat::RGBA32F:
                put(v, r * 100 - 50); put(v, g * 0.001f); put(v, b); put(v, a); break;
            case SurfaceFormat::RG16F:
                put(v, cap::float_to_half(r * 2 - 1)); put(v, cap::float_to_half(g * 2 - 1)); break;
            case SurfaceFormat::R32F: put(v, r * 1000.0f); break;
            case SurfaceFormat::R16F: put(v, cap::float_to_half(r)); break;
            case SurfaceFormat::R16_UNORM: put(v, u16(r)); break;
            case SurfaceFormat::RGBA16_UNORM: put(v, u16(r)); put(v, u16(g)); put(v, u16(b)); put(v, u16(a)); break;
            case SurfaceFormat::RGBA16_SNORM:
                put(v, int16_t(std::lround((r * 2 - 1) * 32767))); put(v, int16_t(std::lround((g * 2 - 1) * 32767)));
                put(v, int16_t(std::lround((b * 2 - 1) * 32767))); put(v, int16_t(32767)); break;
            case SurfaceFormat::R11G11B10F: {
                // Exactly representable values: multiples of 1/64 in [0,1].
                auto f11 = [](float f) -> uint32_t {   // 11-bit float of f in (0,1]
                    if (f <= 0) return 0;
                    int e; const float m = std::frexp(f, &e);        // f = m * 2^e, m in [0.5,1)
                    const uint32_t mant = uint32_t(std::lround((m * 2 - 1) * 64)) & 0x3F;
                    return (uint32_t(e - 1 + 15) << 6) | mant;
                };
                auto f10 = [](float f) -> uint32_t {
                    if (f <= 0) return 0;
                    int e; const float m = std::frexp(f, &e);
                    const uint32_t mant = uint32_t(std::lround((m * 2 - 1) * 32)) & 0x1F;
                    return (uint32_t(e - 1 + 15) << 5) | mant;
                };
                put(v, f11(r) | (f11(g) << 11) | (f10(b) << 22)); break;
            }
            case SurfaceFormat::D24S8: put(v, uint32_t(std::lround(r * 16777215.0f)) | (0x7Fu << 24)); break;
            case SurfaceFormat::D32F: put(v, 0.9f + r * 0.1f); break;
            case SurfaceFormat::D16: put(v, u16(g)); break;
            case SurfaceFormat::R8_UINT: v.push_back(uint8_t((x * 5 + y) % 200)); break;
            default: fail(__FILE__, __LINE__, "pattern: format not covered");
            }
        }
    }
    return v;
}

struct Case { SurfaceFormat fmt; const char* name; };
const Case kCases[] = {
    { SurfaceFormat::RGBA8, "rgba8" },           { SurfaceFormat::RGBA8_SRGB, "rgba8_srgb" },
    { SurfaceFormat::BGRA8, "bgra8" },           { SurfaceFormat::A8R8G8B8, "a8r8g8b8" },
    { SurfaceFormat::X8R8G8B8, "x8r8g8b8" },     { SurfaceFormat::R8, "r8" },
    { SurfaceFormat::RG8, "rg8" },               { SurfaceFormat::R10G10B10A2, "r10g10b10a2" },
    { SurfaceFormat::RGBA16F, "rgba16f" },       { SurfaceFormat::RGBA32F, "rgba32f" },
    { SurfaceFormat::RG16F, "rg16f" },           { SurfaceFormat::R32F, "r32f" },
    { SurfaceFormat::R16F, "r16f" },             { SurfaceFormat::R16_UNORM, "r16_unorm" },
    { SurfaceFormat::RGBA16_UNORM, "rgba16_unorm" }, { SurfaceFormat::RGBA16_SNORM, "rgba16_snorm" },
    { SurfaceFormat::R11G11B10F, "r11g11b10f" }, { SurfaceFormat::D24S8, "d24s8" },
    { SurfaceFormat::D32F, "d32f" },             { SurfaceFormat::D16, "d16" },
    { SurfaceFormat::R8_UINT, "r8_uint" },
};

// PNG decoded by WIC to 64bpp RGBA, each sample 0..65535.
bool wic_decode(const std::filesystem::path& path, uint32_t& w, uint32_t& h, std::vector<uint16_t>& rgba) {
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    f->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) return false;
    conv->GetSize(&w, &h);
    rgba.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 8, static_cast<UINT>(rgba.size() * 2), reinterpret_cast<BYTE*>(rgba.data())));
}

} // namespace

VGX_TEST(capture_half_float) {
    // Every half value survives a trip through float and back.
    for (uint32_t h = 0; h < 0x10000; ++h) {
        const float f = cap::half_to_float(static_cast<uint16_t>(h));
        if (std::isnan(f)) continue;
        CHECK_EQ(cap::float_to_half(f), static_cast<uint16_t>(h));
    }
    CHECK_EQ(cap::float_to_half(1.0f), static_cast<uint16_t>(0x3C00));
    CHECK_EQ(cap::float_to_half(65520.0f), static_cast<uint16_t>(0x7C00));   // rounds to infinity
    CHECK_EQ(cap::float_to_half(-2.0f), static_cast<uint16_t>(0xC000));
}

VGX_TEST(capture_writers) {
    const auto dir = out_dir();
    const Pattern p;
    for (const Case& c : kCases) {
        const auto bytes = encode(c.fmt, p);
        const auto layout = cap::layout_of(c.fmt);
        CHECK_MSG(layout.decodable(), c.name);
        cap::ImageView img{ c.fmt, p.w, p.h, p.w * layout.bytes, bytes.data() };

        // The reference: what decode_row makes of it.
        std::vector<float> ref(size_t(p.w) * p.h * 4);
        for (uint32_t y = 0; y < p.h; ++y)
            CHECK(cap::decode_row(c.fmt, bytes.data() + size_t(y) * img.row_pitch, p.w, ref.data() + size_t(y) * p.w * 4));
        {
            std::ofstream f(dir / (std::string(c.name) + ".ref"), std::ios::binary);
            f.write(reinterpret_cast<const char*>(&p.w), 4);
            f.write(reinterpret_cast<const char*>(&p.h), 4);
            f.write(reinterpret_cast<const char*>(ref.data()), std::streamsize(ref.size() * 4));
        }

        const auto png = dir / (std::string(c.name) + ".png");
        auto r = cap::write_png(png, img);
        CHECK_MSG(r.ok, std::string(c.name) + ": " + r.error);
        const std::pair<cap::ExrCompression, const char*> exrs[] = {
            { cap::ExrCompression::None, "none" }, { cap::ExrCompression::Rle, "rle" },
            { cap::ExrCompression::Zips, "zips" }, { cap::ExrCompression::Zip, "zip" } };
        for (const auto& [comp, suffix] : exrs) {
            r = cap::write_exr(dir / (std::string(c.name) + "." + suffix + ".exr"), img, comp);
            CHECK_MSG(r.ok, std::string(c.name) + ": " + r.error);
        }
        r = cap::write_dds(dir / (std::string(c.name) + ".dds"), img);
        CHECK_MSG(r.ok, std::string(c.name) + ": " + r.error);
        r = cap::write_raw(dir / (std::string(c.name) + ".raw"), img);
        CHECK_MSG(r.ok, std::string(c.name) + ": " + r.error);
        CHECK_EQ(std::filesystem::file_size(dir / (std::string(c.name) + ".raw")), uintmax_t(bytes.size()));

        // 8-bit UNORM sources come back from an independent PNG decoder exactly.
        uint32_t w = 0, h = 0;
        std::vector<uint16_t> dec;
        CHECK_MSG(wic_decode(png, w, h, dec), std::string("WIC could not read ") + c.name);
        CHECK_EQ(w, p.w);
        CHECK_EQ(h, p.h);
        if (layout.kind == cap::ChannelKind::Unorm && layout.bits == 8) {
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                for (uint32_t ch = 0; ch < 3; ++ch) {
                    const float want = layout.channels == 1 ? ref[i * 4] : ref[i * 4 + ch];
                    const float got  = dec[i * 4 + ch] / 65535.0f;
                    if (layout.channels == 2 && ch == 2) continue;
                    CHECK_MSG(std::fabs(got - want) < 0.5f / 255.0f + 1e-6f,
                              std::string(c.name) + " pixel " + std::to_string(i) + " channel " + std::to_string(ch));
                }
            }
        }
    }
    note("files in %ls (check with: py tools/verify_capture.py \"%ls\")", dir.c_str(), dir.c_str());
}

VGX_TEST(capture_png_compresses) {
    // A flat 512x512 RGBA8 image is almost nothing once deflated.
    std::vector<uint8_t> px(512 * 512 * 4, 0x40);
    cap::ImageView img{ SurfaceFormat::RGBA8, 512, 512, 512 * 4, px.data() };
    const auto path = out_dir() / "flat.png";
    CHECK(cap::write_png(path, img).ok);
    CHECK(std::filesystem::file_size(path) < 4096);
    uint32_t w = 0, h = 0;
    std::vector<uint16_t> dec;
    CHECK(wic_decode(path, w, h, dec));
    CHECK_EQ(dec[1000], static_cast<uint16_t>(0x4040));
}
