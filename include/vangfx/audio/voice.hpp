#pragma once
#include <cstdint>

namespace vangfx {

struct VoiceHandle {
    uint64_t id = 0;
    constexpr bool valid()                        const noexcept { return id != 0; }
    constexpr bool operator==(const VoiceHandle&) const noexcept = default;
};

enum class VoiceKind : uint8_t {
    Source    = 0,
    Submix    = 1,
    Mastering = 2,
    DSBuffer  = 3, // DirectSoundBuffer8
};

// Audio format description — mirrors WAVEFORMATEX without requiring Windows headers.
struct WaveFormat {
    uint16_t channels          = 0;
    uint32_t sample_rate       = 0;
    uint16_t bits_per_sample   = 0;
    uint16_t block_align       = 0;
    uint32_t avg_bytes_per_sec = 0;
    bool     is_float          = false; // IEEE_FLOAT vs PCM
    bool     is_extensible     = false;
};

} // namespace vangfx
