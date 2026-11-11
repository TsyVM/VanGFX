#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include "../context_impl.hpp"
#include "../../internal/compat/vtable.hpp"
#include "../../internal/compat/xaudio2_slots.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  XAudio2 IXAudio2 vtable hooks
//
//  These are installed once on the game's IXAudio2 instance.  Since all
//  IXAudio2 objects share the same vtable, one patch covers the whole DLL.
// ─────────────────────────────────────────────────────────────────────────────

namespace vangfx::audio::xa2_hooks {

// Deliberately NOT `using namespace xaudio2_slots`. That namespace contains
// sub-namespaces named IXAudio2, IXAudio2Voice, IXAudio2SourceVoice and
// IXAudio2MasteringVoice -- exactly the names of the COM interfaces from
// xaudio2.h -- so the directive hides the types behind the slot tables and
// every `IXAudio2*` in this file stops naming a type.
namespace slots = xaudio2_slots;

// ── Saved originals ───────────────────────────────────────────────────────────

using CreateSourceVoice_t = HRESULT(STDMETHODCALLTYPE*)(
    IXAudio2*, IXAudio2SourceVoice**, const WAVEFORMATEX*,
    UINT32, float, IXAudio2VoiceCallback*,
    const XAUDIO2_VOICE_SENDS*, const XAUDIO2_EFFECT_CHAIN*);

using CreateSubmixVoice_t = HRESULT(STDMETHODCALLTYPE*)(
    IXAudio2*, IXAudio2SubmixVoice**, UINT32, UINT32,
    UINT32, UINT32, const XAUDIO2_VOICE_SENDS*, const XAUDIO2_EFFECT_CHAIN*);

using CreateMasteringVoice_t = HRESULT(STDMETHODCALLTYPE*)(
    IXAudio2*, IXAudio2MasteringVoice**, UINT32, UINT32,
    UINT32, LPCWSTR, const XAUDIO2_EFFECT_CHAIN*, AUDIO_STREAM_CATEGORY);

using StartEngine_t = HRESULT(STDMETHODCALLTYPE*)(IXAudio2*);
using StopEngine_t  = void   (STDMETHODCALLTYPE*)(IXAudio2*);

static CreateSourceVoice_t  orig_CreateSourceVoice  = nullptr;
static CreateSubmixVoice_t  orig_CreateSubmixVoice  = nullptr;
static CreateMasteringVoice_t orig_CreateMasteringVoice = nullptr;
static StartEngine_t        orig_StartEngine        = nullptr;
static StopEngine_t         orig_StopEngine         = nullptr;

// ── Helper: convert WAVEFORMATEX → WaveFormat ────────────────────────────────

static WaveFormat from_wfx(const WAVEFORMATEX* wfx) {
    if (!wfx) return {};
    WaveFormat f;
    f.channels          = wfx->nChannels;
    f.sample_rate       = wfx->nSamplesPerSec;
    f.bits_per_sample   = wfx->wBitsPerSample;
    f.block_align       = wfx->nBlockAlign;
    f.avg_bytes_per_sec = wfx->nAvgBytesPerSec;
    f.is_float          = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    f.is_extensible     = (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    if (f.is_extensible) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        f.is_float = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return f;
}

// ── Hook: CreateSourceVoice ───────────────────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hook_CreateSourceVoice(
    IXAudio2*               self,
    IXAudio2SourceVoice**   ppSourceVoice,
    const WAVEFORMATEX*     pSourceFormat,
    UINT32                  Flags,
    float                   MaxFrequencyRatio,
    IXAudio2VoiceCallback*  pCallback,
    const XAUDIO2_VOICE_SENDS* pSendList,
    const XAUDIO2_EFFECT_CHAIN* pEffectChain)
{
    HRESULT hr = orig_CreateSourceVoice(self, ppSourceVoice, pSourceFormat,
                                         Flags, MaxFrequencyRatio, pCallback,
                                         pSendList, pEffectChain);
    if (SUCCEEDED(hr) && ppSourceVoice && *ppSourceVoice) {
        auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
        if (ctx) {
            WaveFormat fmt = from_wfx(pSourceFormat);
            VoiceHandle h  = ctx->register_voice(VoiceKind::Source, fmt, *ppSourceVoice);
            ctx->hook_source_voice_vtable(*ppSourceVoice);

            VoiceCreateEvent ev{h, VoiceKind::Source, fmt, Flags, *ppSourceVoice};
            ctx->dispatch_voice_create(ev);
        }
    }
    return hr;
}

// ── Hook: CreateSubmixVoice ───────────────────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hook_CreateSubmixVoice(
    IXAudio2*               self,
    IXAudio2SubmixVoice**   ppSubmixVoice,
    UINT32                  InputChannels,
    UINT32                  InputSampleRate,
    UINT32                  Flags,
    UINT32                  ProcessingStage,
    const XAUDIO2_VOICE_SENDS* pSendList,
    const XAUDIO2_EFFECT_CHAIN* pEffectChain)
{
    HRESULT hr = orig_CreateSubmixVoice(self, ppSubmixVoice, InputChannels,
                                         InputSampleRate, Flags, ProcessingStage,
                                         pSendList, pEffectChain);
    if (SUCCEEDED(hr) && ppSubmixVoice && *ppSubmixVoice) {
        auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
        if (ctx) {
            WaveFormat fmt;
            fmt.channels    = static_cast<uint16_t>(InputChannels);
            fmt.sample_rate = InputSampleRate;
            VoiceHandle h   = ctx->register_voice(VoiceKind::Submix, fmt, *ppSubmixVoice);
            VoiceCreateEvent ev{h, VoiceKind::Submix, fmt, Flags, *ppSubmixVoice};
            ctx->dispatch_voice_create(ev);
        }
    }
    return hr;
}

// ── Hook: CreateMasteringVoice ────────────────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hook_CreateMasteringVoice(
    IXAudio2*                self,
    IXAudio2MasteringVoice** ppMasteringVoice,
    UINT32                   InputChannels,
    UINT32                   InputSampleRate,
    UINT32                   Flags,
    LPCWSTR                  szDeviceId,
    const XAUDIO2_EFFECT_CHAIN* pEffectChain,
    AUDIO_STREAM_CATEGORY    StreamCategory)
{
    HRESULT hr = orig_CreateMasteringVoice(self, ppMasteringVoice, InputChannels,
                                            InputSampleRate, Flags, szDeviceId,
                                            pEffectChain, StreamCategory);
    if (SUCCEEDED(hr) && ppMasteringVoice && *ppMasteringVoice) {
        auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
        if (ctx) {
            if (!ctx->xa2_game_master_)
                ctx->xa2_game_master_ = *ppMasteringVoice;

            WaveFormat fmt;
            fmt.channels    = static_cast<uint16_t>(InputChannels);
            fmt.sample_rate = InputSampleRate;
            VoiceHandle h   = ctx->register_voice(VoiceKind::Mastering, fmt, *ppMasteringVoice);
            ctx->hook_mastering_voice_vtable(*ppMasteringVoice);

            VoiceCreateEvent ev{h, VoiceKind::Mastering, fmt, Flags, *ppMasteringVoice};
            ctx->dispatch_voice_create(ev);
        }
    }
    return hr;
}

// ── Hook: StartEngine / StopEngine ───────────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hook_StartEngine(IXAudio2* self) {
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) ctx->dispatch_engine_start();
    return orig_StartEngine(self);
}

static void STDMETHODCALLTYPE hook_StopEngine(IXAudio2* self) {
    orig_StopEngine(self);
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) ctx->dispatch_engine_stop();
}

// ── install_on_xa2 ────────────────────────────────────────────────────────────

void install_on_xa2(IXAudio2* xa2) {
    if (!xa2) return;

    vtable::patch(xa2, slots::IXAudio2::CreateSourceVoice,
                  &hook_CreateSourceVoice, &orig_CreateSourceVoice);

    vtable::patch(xa2, slots::IXAudio2::CreateSubmixVoice,
                  &hook_CreateSubmixVoice, &orig_CreateSubmixVoice);

    vtable::patch(xa2, slots::IXAudio2::CreateMasteringVoice,
                  &hook_CreateMasteringVoice, &orig_CreateMasteringVoice);

    vtable::patch(xa2, slots::IXAudio2::StartEngine,
                  &hook_StartEngine, &orig_StartEngine);

    vtable::patch(xa2, slots::IXAudio2::StopEngine,
                  &hook_StopEngine, &orig_StopEngine);
}

} // namespace vangfx::audio::xa2_hooks
