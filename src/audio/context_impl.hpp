#pragma once

#include "../../include/vangfx/audio/context.hpp"
#include "../internal/compat/vtable.hpp"
#include "../internal/compat/function_hook.hpp"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declare XAudio2 / DirectSound types to keep Windows SDK headers out
// of this header.  Implementation files include <xaudio2.h> / <dsound.h>.
struct IXAudio2;
struct IXAudio2SourceVoice;
struct IXAudio2SubmixVoice;
struct IXAudio2MasteringVoice;
struct IDirectSound8;
struct IDirectSoundBuffer8;

namespace vangfx::audio {

struct SoundEntry {
    std::vector<uint8_t> pcm;
    WaveFormat           format;
};

struct VoiceEntry {
    VoiceHandle handle;
    VoiceKind   kind;
    WaveFormat  format;
};

struct PlayingVoice {
    SoundHandle         sound;
    IXAudio2SourceVoice* xa2_voice = nullptr;
};

class AudioContextImpl final : public AudioContext {
public:
    AudioContextImpl(AudioBackendKind backend,
                     AudioLogLevel    log_level,
                     AudioLogCallback log_cb);
    ~AudioContextImpl() override;

    // ── AudioContext interface ─────────────────────────────────────────────
    void on_voice_create(std::function<void(VoiceCreateEvent&)>  cb) override;
    void on_voice_destroy(std::function<void(VoiceDestroyEvent&)> cb) override;
    void on_buffer_submit(std::function<void(BufferSubmitEvent&)> cb) override;
    void on_volume_change(std::function<void(VolumeChangeEvent&)> cb) override;
    void on_engine_start(std::function<void()> cb) override;
    void on_engine_stop(std::function<void()>  cb) override;

    Result<SoundHandle> load_wav(std::string_view path)                  override;
    Result<SoundHandle> load_wav_memory(const void* data, size_t bytes)  override;
    Result<void>        play(SoundHandle h, float volume, float pitch)   override;
    Result<void>        play_looping(SoundHandle h, float volume, float pitch) override;
    Result<void>        stop(SoundHandle h)                              override;
    Result<void>        stop_all()                                       override;
    Result<void>        set_master_volume(float v)                       override;

    Result<AudioEffectHandle> inject_master_effect(AudioDspCallback cb)  override;
    Result<void>              remove_effect(AudioEffectHandle h)          override;

    Result<void> start_capture(AudioCaptureConfig cfg)                   override;
    Result<void> stop_capture()                                          override;

    AudioBackendKind active_backend() const noexcept override { return active_backend_; }
    void             shutdown()                       override;

    // ── Internal: called from hook thunks ─────────────────────────────────
    VoiceHandle register_voice(VoiceKind kind, const WaveFormat& fmt, void* raw);
    void        unregister_voice(void* raw);
    VoiceHandle find_voice(void* raw) const;

    void dispatch_voice_create(VoiceCreateEvent& ev);
    void dispatch_voice_destroy(VoiceDestroyEvent& ev);
    void dispatch_buffer_submit(BufferSubmitEvent& ev);
    void dispatch_volume_change(VolumeChangeEvent& ev);
    void dispatch_engine_start();
    void dispatch_engine_stop();

    // XAudio2 vtable patching — called once per vtable type.
    void hook_source_voice_vtable(IXAudio2SourceVoice* voice);
    void hook_mastering_voice_vtable(IXAudio2MasteringVoice* voice);

    void log(AudioLogLevel level, std::string_view msg) const;

    // XAudio2 context (game's instance + framework's instance)
    IXAudio2*              xa2_game_      = nullptr; // game's IXAudio2*
    IXAudio2MasteringVoice* xa2_game_master_ = nullptr;
    IXAudio2*              xa2_fw_        = nullptr; // framework playback instance
    IXAudio2MasteringVoice* xa2_fw_master_ = nullptr;

    // DirectSound context
    IDirectSound8* ds8_ = nullptr;

    // Whether vtable for each voice type is already patched (shared vtable).
    std::atomic<bool> source_vtable_patched_{false};
    std::atomic<bool> mastering_vtable_patched_{false};

private:
    // AudioContextBuilder::build() is what decides between the two backends and
    // drives initialisation, so it is the one caller that legitimately reaches
    // these. Friendship rather than making them public: nothing else should be
    // able to re-initialise a live context.
    friend class ::vangfx::AudioContextBuilder;

    Result<void> init_xaudio2(void* explicit_xa2);
    Result<void> init_dsound();
    Result<void> create_fw_xa2();

    Result<void>     play_internal(SoundHandle h, bool loop,
                                   float volume, float pitch);
    Result<SoundEntry> find_sound(SoundHandle h) const;

    SoundHandle   alloc_sound_id()  noexcept { return {next_sound_id_++}; }
    AudioEffectHandle alloc_effect_id() noexcept { return {next_effect_id_++}; }

    AudioBackendKind  active_backend_;
    AudioLogLevel     log_level_;
    AudioLogCallback  log_cb_;

    // Callbacks
    mutable std::shared_mutex cb_mutex_;
    std::vector<std::function<void(VoiceCreateEvent&)>>  voice_create_cbs_;
    std::vector<std::function<void(VoiceDestroyEvent&)>> voice_destroy_cbs_;
    std::vector<std::function<void(BufferSubmitEvent&)>> buffer_submit_cbs_;
    std::vector<std::function<void(VolumeChangeEvent&)>> volume_change_cbs_;
    std::vector<std::function<void()>>                   engine_start_cbs_;
    std::vector<std::function<void()>>                   engine_stop_cbs_;

    // Voice registry
    mutable std::shared_mutex voice_mutex_;
    std::unordered_map<void*, VoiceEntry> voices_;
    std::atomic<uint64_t> next_voice_id_{1};

    // Sound library
    mutable std::mutex sound_mutex_;
    std::unordered_map<uint64_t, SoundEntry> sounds_;
    std::atomic<uint64_t> next_sound_id_{1};

    // Active playback voices (framework-owned)
    std::mutex playing_mutex_;
    std::vector<PlayingVoice> playing_;

    // DSP effects on master output
    mutable std::mutex effect_mutex_;
    std::vector<std::pair<AudioEffectHandle, AudioDspCallback>> master_effects_;
    std::atomic<uint64_t> next_effect_id_{1};

    // Capture state
    std::mutex        capture_mutex_;
    AudioCaptureConfig capture_cfg_;
    bool               capturing_ = false;

    // Inline export hook for XAudio2Create / DirectSoundCreate8
    function_hook::InlinePatch xa2_create_patch_;
    function_hook::InlinePatch ds_create_patch_;
};

// Global used by hook thunks — set in init, cleared in shutdown.
extern std::atomic<AudioContextImpl*> g_audio_ctx;

} // namespace vangfx::audio
