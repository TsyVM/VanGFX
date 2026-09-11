#pragma once

/**
 * vangfx/audio/context.hpp — VanGFX audio interception and playback root.
 *
 * AudioContext hooks into the game's XAudio2 or DirectSound layer via the
 * same vtable-patching approach VanGFX uses for graphics.  It lets callers:
 *
 *   • Observe every voice creation / destruction, buffer submission, and
 *     volume change via typed event callbacks.
 *   • Play back WAV sounds through a framework-owned audio device that sits
 *     alongside the game's audio, independent of the game's voice graph.
 *   • Inject floating-point DSP effects after the mastering voice mix.
 *   • Capture the game's raw PCM output to disk or a callback.
 *
 * Usage:
 *   auto audio = vangfx::AudioContextBuilder{}
 *       .log_level(vangfx::AudioLogLevel::Info)
 *       .build()
 *       .value();
 *
 *   audio->on_buffer_submit([](vangfx::BufferSubmitEvent& ev) {
 *       // inspect or suppress ev.audio_data / ev.audio_bytes
 *   });
 *
 *   auto snd = audio->load_wav("explode.wav").value();
 *   audio->play(snd, 0.8f);
 *
 * Built and maintained by TeamVanilla — https://www.teamvanilla.org/
 */

#include "../core/error.hpp"
#include "events.hpp"
#include "playback.hpp"
#include "capture.hpp"
#include "effect.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace vangfx {

enum class AudioBackendKind : uint8_t {
    Auto        = 0, // scan for XAudio2, fall back to DirectSound
    XAudio2     = 1,
    DirectSound = 2,
};

enum class AudioLogLevel : uint8_t {
    None    = 0,
    Error   = 1,
    Warning = 2,
    Info    = 3,
    Verbose = 4,
};

using AudioLogCallback = std::function<void(AudioLogLevel, std::string_view)>;

// ─────────────────────────────────────────────────────────────────────────────
//  AudioContext — root object for all audio operations
// ─────────────────────────────────────────────────────────────────────────────

class AudioContext {
public:
    virtual ~AudioContext() = default;

    // ── Event callbacks ────────────────────────────────────────────────────
    // Multiple callbacks of the same type can be registered; all are called
    // in registration order.  Callbacks may be invoked on the audio thread.

    virtual void on_voice_create(std::function<void(VoiceCreateEvent&)>  cb) = 0;
    virtual void on_voice_destroy(std::function<void(VoiceDestroyEvent&)> cb) = 0;
    virtual void on_buffer_submit(std::function<void(BufferSubmitEvent&)> cb) = 0;
    virtual void on_volume_change(std::function<void(VolumeChangeEvent&)> cb) = 0;
    virtual void on_engine_start(std::function<void()> cb) = 0;
    virtual void on_engine_stop(std::function<void()>  cb) = 0;

    // ── Framework playback ─────────────────────────────────────────────────
    // Sounds are played through a separate XAudio2 instance owned by VanGFX,
    // so they never interfere with the game's voice graph.

    virtual Result<SoundHandle> load_wav(std::string_view path)                     = 0;
    virtual Result<SoundHandle> load_wav_memory(const void* data, size_t bytes)     = 0;
    virtual Result<void>        play(SoundHandle h, float volume = 1.f,
                                     float pitch = 1.f)                             = 0;
    virtual Result<void>        play_looping(SoundHandle h, float volume = 1.f,
                                             float pitch = 1.f)                     = 0;
    virtual Result<void>        stop(SoundHandle h)                                 = 0;
    virtual Result<void>        stop_all()                                          = 0;
    virtual Result<void>        set_master_volume(float v)                          = 0;

    // ── DSP effect injection ───────────────────────────────────────────────
    // Inserts a float-domain DSP stage after the game's mastering voice mix.
    // The callback runs on the XAudio2 XAPO processing thread.

    virtual Result<AudioEffectHandle> inject_master_effect(AudioDspCallback cb) = 0;
    virtual Result<void>              remove_effect(AudioEffectHandle h)         = 0;

    // ── Audio capture ──────────────────────────────────────────────────────
    virtual Result<void> start_capture(AudioCaptureConfig cfg) = 0;
    virtual Result<void> stop_capture()                        = 0;

    // ── Metadata ───────────────────────────────────────────────────────────
    virtual AudioBackendKind active_backend() const noexcept = 0;

    // Uninstalls all vtable hooks and releases the framework audio device.
    virtual void shutdown() = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  AudioContextBuilder — fluent factory
// ─────────────────────────────────────────────────────────────────────────────

class AudioContextBuilder {
public:
    // Select the audio backend.  Auto scans for XAudio2 2.8/2.9 first, then
    // DirectSound.  If the game hasn't loaded either yet, detection is deferred
    // until the first XAudio2Create / DirectSoundCreate8 call.
    AudioContextBuilder& backend(AudioBackendKind k) noexcept  { backend_   = k; return *this; }

    AudioContextBuilder& log_level(AudioLogLevel l) noexcept   { log_level_ = l; return *this; }
    AudioContextBuilder& log_callback(AudioLogCallback cb)     { log_cb_   = std::move(cb); return *this; }

    // Optional: supply the game's existing IXAudio2* directly so hook
    // installation does not require intercepting XAudio2Create.
    AudioContextBuilder& xaudio2_instance(void* p) noexcept   { xa2_ptr_   = p; return *this; }

    [[nodiscard]] Result<std::unique_ptr<AudioContext>> build();

private:
    AudioBackendKind  backend_   = AudioBackendKind::Auto;
    AudioLogLevel     log_level_ = AudioLogLevel::Warning;
    AudioLogCallback  log_cb_;
    void*             xa2_ptr_   = nullptr;
};

} // namespace vangfx
