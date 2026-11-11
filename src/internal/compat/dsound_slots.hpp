#pragma once
#include <cstdint>

// DirectSound 8 vtable slot indices.  Both interfaces inherit IUnknown (0,1,2).

namespace vangfx::dsound_slots {

namespace IDirectSound8 {
    constexpr uint32_t QueryInterface       = 0;
    constexpr uint32_t AddRef               = 1;
    constexpr uint32_t Release              = 2;
    constexpr uint32_t CreateSoundBuffer    = 3;
    constexpr uint32_t GetCaps              = 4;
    constexpr uint32_t DuplicateSoundBuffer = 5;
    constexpr uint32_t SetCooperativeLevel  = 6;
    constexpr uint32_t Compact              = 7;
    constexpr uint32_t GetSpeakerConfig     = 8;
    constexpr uint32_t SetSpeakerConfig     = 9;
    constexpr uint32_t Initialize           = 10;
    constexpr uint32_t VerifyCertification  = 11;
}

namespace IDirectSoundBuffer8 {
    constexpr uint32_t QueryInterface     = 0;
    constexpr uint32_t AddRef             = 1;
    constexpr uint32_t Release            = 2;
    constexpr uint32_t GetCaps            = 3;
    constexpr uint32_t GetCurrentPosition = 4;
    constexpr uint32_t GetFormat          = 5;
    constexpr uint32_t GetVolume          = 6;
    constexpr uint32_t GetPan             = 7;
    constexpr uint32_t GetFrequency       = 8;
    constexpr uint32_t GetStatus          = 9;
    constexpr uint32_t Initialize         = 10;
    constexpr uint32_t Lock               = 11;
    constexpr uint32_t Play               = 12;
    constexpr uint32_t SetCurrentPosition = 13;
    constexpr uint32_t SetFormat          = 14;
    constexpr uint32_t SetVolume          = 15;
    constexpr uint32_t SetPan             = 16;
    constexpr uint32_t SetFrequency       = 17;
    constexpr uint32_t Stop               = 18;
    constexpr uint32_t Unlock             = 19;
    constexpr uint32_t Restore            = 20;
    constexpr uint32_t SetFX              = 21;
    constexpr uint32_t AcquireResources   = 22;
    constexpr uint32_t GetObjectInPath    = 23;
}

} // namespace vangfx::dsound_slots
