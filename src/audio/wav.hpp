#pragma once
#include "../../include/vangfx/core/error.hpp"
#include "../../include/vangfx/audio/voice.hpp"
#include <cstdint>
#include <cstring>
#include <vector>

namespace vangfx::audio {

struct WavData {
    WaveFormat           format;
    std::vector<uint8_t> pcm; // raw PCM / IEEE-float samples
};

// Minimal RIFF/WAV parser — no external dependencies.
inline Result<WavData> parse_wav(const void* data, size_t size) {
    if (!data || size < 44)
        return gfx_err<WavData>(ShaderError::InvalidFormat, "WAV data too small");

    const auto* p   = static_cast<const uint8_t*>(data);
    const auto* end = p + size;

    auto read_u16 = [](const uint8_t* s) -> uint16_t {
        uint16_t v; std::memcpy(&v, s, 2); return v;
    };
    auto read_u32 = [](const uint8_t* s) -> uint32_t {
        uint32_t v; std::memcpy(&v, s, 4); return v;
    };

    // RIFF header
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0)
        return gfx_err<WavData>(ShaderError::InvalidFormat, "not a RIFF/WAVE file");

    p += 12;

    WavData out;
    bool have_fmt  = false;
    bool have_data = false;

    while (p + 8 <= end && !have_data) {
        uint32_t chunk_size = read_u32(p + 4);
        const uint8_t* chunk_data = p + 8;
        const uint8_t* chunk_end  = chunk_data + chunk_size;

        if (std::memcmp(p, "fmt ", 4) == 0) {
            if (chunk_size < 16)
                return gfx_err<WavData>(ShaderError::InvalidFormat, "fmt chunk too small");

            uint16_t audio_fmt = read_u16(chunk_data);
            // 1 = PCM, 3 = IEEE_FLOAT, 0xFFFE = EXTENSIBLE
            bool extensible  = (audio_fmt == 0xFFFE);
            bool is_float    = (audio_fmt == 3);
            if (extensible && chunk_size >= 28) {
                uint16_t sub = read_u16(chunk_data + 24);
                is_float = (sub == 3);
            }
            if (audio_fmt != 1 && audio_fmt != 3 && !extensible)
                return gfx_err<WavData>(ShaderError::InvalidFormat,
                    "unsupported WAV format (only PCM/IEEE_FLOAT/EXTENSIBLE)");

            out.format.channels          = read_u16(chunk_data + 2);
            out.format.sample_rate       = read_u32(chunk_data + 4);
            out.format.avg_bytes_per_sec = read_u32(chunk_data + 8);
            out.format.block_align       = read_u16(chunk_data + 12);
            out.format.bits_per_sample   = read_u16(chunk_data + 14);
            out.format.is_float          = is_float;
            out.format.is_extensible     = extensible;
            have_fmt = true;

        } else if (std::memcmp(p, "data", 4) == 0) {
            if (!have_fmt)
                return gfx_err<WavData>(ShaderError::InvalidFormat, "data chunk before fmt");
            if (chunk_end > end)
                return gfx_err<WavData>(ShaderError::InvalidFormat, "data chunk overruns file");

            out.pcm.assign(chunk_data, chunk_end);
            have_data = true;
        }

        // chunks are word-aligned
        p += 8 + ((chunk_size + 1) & ~uint32_t{1});
    }

    if (!have_fmt || !have_data)
        return gfx_err<WavData>(ShaderError::InvalidFormat, "missing fmt or data chunk");

    return out;
}

} // namespace vangfx::audio
