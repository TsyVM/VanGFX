#pragma once
#include "voice.hpp"
#include <cstdint>

namespace vangfx {

struct VoiceCreateEvent {
    VoiceHandle handle;
    VoiceKind   kind;
    WaveFormat  format;
    uint32_t    flags;
    void*       raw_voice; // IXAudio2SourceVoice* or IDirectSoundBuffer8*
};

struct VoiceDestroyEvent {
    VoiceHandle handle;
    VoiceKind   kind;
    void*       raw_voice;
};

struct BufferSubmitEvent {
    VoiceHandle  voice;
    const void*  audio_data;
    uint32_t     audio_bytes;
    uint32_t     play_begin;
    uint32_t     play_length;
    uint32_t     loop_begin;
    uint32_t     loop_length;
    uint32_t     loop_count;  // 0xFF == XAUDIO2_LOOP_INFINITE
    bool         intercept;   // set true to suppress the Submit / Play call
};

struct VolumeChangeEvent {
    VoiceHandle handle;
    VoiceKind   kind;
    float       new_volume;
    uint32_t    operation_set;
};

} // namespace vangfx
