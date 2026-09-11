#pragma once
#include <cstdint>
#include <functional>

namespace vangfx {

struct AudioEffectHandle {
    uint64_t id = 0;
    constexpr bool valid()                              const noexcept { return id != 0; }
    constexpr bool operator==(const AudioEffectHandle&) const noexcept = default;
};

// Floating-point DSP callback inserted after the mastering voice mix.
// samples:     interleaved float PCM, frame_count * channels floats total.
// Runs on the XAudio2 processing thread — must be lock-free and fast.
using AudioDspCallback = std::function<void(
    float*   samples,
    uint32_t frame_count,
    uint16_t channels,
    uint32_t sample_rate
)>;

} // namespace vangfx
