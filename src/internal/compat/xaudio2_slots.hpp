#pragma once
#include <cstdint>

/**
 * XAudio2 2.8+ (Windows 8+ in-box) vtable slot indices.
 *
 * IXAudio2 inherits IUnknown (QI=0, AddRef=1, Release=2).
 * IXAudio2Voice does NOT inherit IUnknown — its vtable starts at 0.
 * IXAudio2SourceVoice / SubmixVoice / MasteringVoice extend IXAudio2Voice.
 */

namespace vangfx::xaudio2_slots {

namespace IXAudio2 {
    constexpr uint32_t QueryInterface         = 0;
    constexpr uint32_t AddRef                 = 1;
    constexpr uint32_t Release                = 2;
    constexpr uint32_t RegisterForCallbacks   = 3;
    constexpr uint32_t UnregisterForCallbacks = 4;
    constexpr uint32_t CreateSourceVoice      = 5;
    constexpr uint32_t CreateSubmixVoice      = 6;
    constexpr uint32_t CreateMasteringVoice   = 7;
    constexpr uint32_t StartEngine            = 8;
    constexpr uint32_t StopEngine             = 9;
    constexpr uint32_t CommitChanges          = 10;
    constexpr uint32_t GetPerformanceData     = 11;
    constexpr uint32_t SetDebugConfiguration  = 12;
}

// Shared base — slots 0-18 present on all three voice types.
namespace IXAudio2Voice {
    constexpr uint32_t GetVoiceDetails           = 0;
    constexpr uint32_t SetOutputVoices           = 1;
    constexpr uint32_t SetEffectChain            = 2;
    constexpr uint32_t EnableEffect              = 3;
    constexpr uint32_t DisableEffect             = 4;
    constexpr uint32_t GetEffectState            = 5;
    constexpr uint32_t SetEffectParameters       = 6;
    constexpr uint32_t GetEffectParameters       = 7;
    constexpr uint32_t SetFilterParameters       = 8;
    constexpr uint32_t GetFilterParameters       = 9;
    constexpr uint32_t SetOutputFilterParameters = 10;
    constexpr uint32_t GetOutputFilterParameters = 11;
    constexpr uint32_t SetVolume                 = 12;
    constexpr uint32_t GetVolume                 = 13;
    constexpr uint32_t SetChannelVolumes         = 14;
    constexpr uint32_t GetChannelVolumes         = 15;
    constexpr uint32_t SetOutputMatrix           = 16;
    constexpr uint32_t GetOutputMatrix           = 17;
    constexpr uint32_t DestroyVoice              = 18;
}

namespace IXAudio2SourceVoice {
    constexpr uint32_t Start               = 19;
    constexpr uint32_t Stop                = 20;
    constexpr uint32_t SubmitSourceBuffer  = 21;
    constexpr uint32_t FlushSourceBuffers  = 22;
    constexpr uint32_t Discontinuity       = 23;
    constexpr uint32_t ExitLoop            = 24;
    constexpr uint32_t GetState            = 25;
    constexpr uint32_t SetFrequencyRatio   = 26;
    constexpr uint32_t GetFrequencyRatio   = 27;
    constexpr uint32_t SetSourceSampleRate = 28;
}

namespace IXAudio2MasteringVoice {
    constexpr uint32_t GetChannelMask = 19;
}

} // namespace vangfx::xaudio2_slots
