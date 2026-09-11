#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace vangfx {

class AudioContext;

enum class AudioCaptureFormat : uint8_t {
    WAV = 0,
    Raw = 1,
};

struct AudioCaptureConfig {
    std::string        output_dir;
    AudioCaptureFormat format      = AudioCaptureFormat::WAV;
    uint32_t           max_seconds = 0; // 0 = unlimited

    // Optional per-buffer callback, called on the audio thread.
    std::function<void(const void* pcm, uint32_t bytes,
                       uint32_t sample_rate, uint16_t channels,
                       uint16_t bits_per_sample)> on_pcm;
};

// RAII guard that calls start_capture on construction and stop_capture on destruction.
class AudioCaptureSession {
public:
    explicit AudioCaptureSession(AudioContext& ctx, AudioCaptureConfig cfg);
    ~AudioCaptureSession();

    AudioCaptureSession(const AudioCaptureSession&)            = delete;
    AudioCaptureSession& operator=(const AudioCaptureSession&) = delete;

private:
    AudioContext& ctx_;
};

} // namespace vangfx
