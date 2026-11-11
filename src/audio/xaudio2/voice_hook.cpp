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
//  IXAudio2SourceVoice and IXAudio2MasteringVoice vtable hooks.
//
//  All source voices share the same vtable (same concrete class inside
//  xaudio2.dll), so patching the vtable of one voice patches all of them.
//  hook_source_voice_vtable / hook_mastering_voice_vtable are idempotent
//  — they check an atomic flag and no-op on the second call.
// ─────────────────────────────────────────────────────────────────────────────

namespace vangfx::audio {

// Deliberately NOT `using namespace xaudio2_slots`. That namespace contains
// sub-namespaces named IXAudio2, IXAudio2Voice, IXAudio2SourceVoice and
// IXAudio2MasteringVoice -- exactly the names of the COM interfaces from
// xaudio2.h -- so the directive hides the types behind the slot tables and
// every `IXAudio2*` in this file stops naming a type.
namespace slots = xaudio2_slots;

// ── SourceVoice originals ─────────────────────────────────────────────────────

using SubmitSourceBuffer_t = HRESULT(STDMETHODCALLTYPE*)(
    IXAudio2SourceVoice*, const XAUDIO2_BUFFER*, const XAUDIO2_BUFFER_WMA*);

using SV_SetVolume_t   = void(STDMETHODCALLTYPE*)(IXAudio2SourceVoice*, float, UINT32);
using SV_DestroyVoice_t = void(STDMETHODCALLTYPE*)(IXAudio2SourceVoice*);

static SubmitSourceBuffer_t  orig_SubmitSourceBuffer  = nullptr;
static SV_SetVolume_t        orig_SV_SetVolume        = nullptr;
static SV_DestroyVoice_t     orig_SV_DestroyVoice     = nullptr;

// ── MasteringVoice originals ──────────────────────────────────────────────────

using MV_SetVolume_t    = void(STDMETHODCALLTYPE*)(IXAudio2MasteringVoice*, float, UINT32);
using MV_DestroyVoice_t = void(STDMETHODCALLTYPE*)(IXAudio2MasteringVoice*);

static MV_SetVolume_t    orig_MV_SetVolume    = nullptr;
static MV_DestroyVoice_t orig_MV_DestroyVoice = nullptr;

// ── SourceVoice hook: SubmitSourceBuffer ─────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hook_SubmitSourceBuffer(
    IXAudio2SourceVoice*     self,
    const XAUDIO2_BUFFER*    pBuffer,
    const XAUDIO2_BUFFER_WMA* pBufferWMA)
{
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx && pBuffer) {
        VoiceHandle h = ctx->find_voice(self);

        BufferSubmitEvent ev{};
        ev.voice       = h;
        ev.audio_data  = pBuffer->pAudioData;
        ev.audio_bytes = pBuffer->AudioBytes;
        ev.play_begin  = pBuffer->PlayBegin;
        ev.play_length = pBuffer->PlayLength;
        ev.loop_begin  = pBuffer->LoopBegin;
        ev.loop_length = pBuffer->LoopLength;
        ev.loop_count  = pBuffer->LoopCount;
        ev.intercept   = false;

        ctx->dispatch_buffer_submit(ev);

        if (ev.intercept) return S_OK;
    }
    return orig_SubmitSourceBuffer(self, pBuffer, pBufferWMA);
}

// ── SourceVoice hook: SetVolume ───────────────────────────────────────────────

static void STDMETHODCALLTYPE hook_SV_SetVolume(IXAudio2SourceVoice* self,
                                                 float Volume, UINT32 OperationSet)
{
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) {
        VoiceHandle h = ctx->find_voice(self);
        VolumeChangeEvent ev{h, VoiceKind::Source, Volume, OperationSet};
        ctx->dispatch_volume_change(ev);
    }
    orig_SV_SetVolume(self, Volume, OperationSet);
}

// ── SourceVoice hook: DestroyVoice ───────────────────────────────────────────

static void STDMETHODCALLTYPE hook_SV_DestroyVoice(IXAudio2SourceVoice* self) {
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) {
        VoiceHandle h = ctx->find_voice(self);
        if (h.valid()) {
            VoiceDestroyEvent ev{h, VoiceKind::Source, self};
            ctx->dispatch_voice_destroy(ev);
            ctx->unregister_voice(self);
        }
    }
    orig_SV_DestroyVoice(self);
}

// ── MasteringVoice hook: SetVolume ────────────────────────────────────────────

static void STDMETHODCALLTYPE hook_MV_SetVolume(IXAudio2MasteringVoice* self,
                                                  float Volume, UINT32 OperationSet)
{
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) {
        VoiceHandle h = ctx->find_voice(self);
        VolumeChangeEvent ev{h, VoiceKind::Mastering, Volume, OperationSet};
        ctx->dispatch_volume_change(ev);
    }
    orig_MV_SetVolume(self, Volume, OperationSet);
}

// ── MasteringVoice hook: DestroyVoice ────────────────────────────────────────

static void STDMETHODCALLTYPE hook_MV_DestroyVoice(IXAudio2MasteringVoice* self) {
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) {
        VoiceHandle h = ctx->find_voice(self);
        if (h.valid()) {
            VoiceDestroyEvent ev{h, VoiceKind::Mastering, self};
            ctx->dispatch_voice_destroy(ev);
            ctx->unregister_voice(self);
        }
        if (ctx->xa2_game_master_ == self)
            ctx->xa2_game_master_ = nullptr;
    }
    orig_MV_DestroyVoice(self);
}

// ── Install helpers ────────────────────────────────────────────────────────────

void AudioContextImpl::hook_source_voice_vtable(IXAudio2SourceVoice* voice) {
    if (source_vtable_patched_.exchange(true)) return;

    vtable::patch(voice, slots::IXAudio2SourceVoice::SubmitSourceBuffer,
                  &hook_SubmitSourceBuffer, &orig_SubmitSourceBuffer);

    vtable::patch(voice, slots::IXAudio2Voice::SetVolume,
                  &hook_SV_SetVolume, &orig_SV_SetVolume);

    vtable::patch(voice, slots::IXAudio2Voice::DestroyVoice,
                  &hook_SV_DestroyVoice, &orig_SV_DestroyVoice);
}

void AudioContextImpl::hook_mastering_voice_vtable(IXAudio2MasteringVoice* voice) {
    if (mastering_vtable_patched_.exchange(true)) return;

    vtable::patch(voice, slots::IXAudio2Voice::SetVolume,
                  &hook_MV_SetVolume, &orig_MV_SetVolume);

    vtable::patch(voice, slots::IXAudio2Voice::DestroyVoice,
                  &hook_MV_DestroyVoice, &orig_MV_DestroyVoice);
}

} // namespace vangfx::audio
