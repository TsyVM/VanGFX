// Before every include, not just before dsound.h: dsound.h gates IDirectSound8
// and the rest of the DX8 interfaces on this, and if anything pulls dsound.h in
// first the guard has already been evaluated and the define comes too late.
// The symptom is not a missing macro but "IDirectSound8 is not a type".
#define DIRECTSOUND_VERSION 0x0800

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
// dsound.h uses WAVEFORMATEX and LPWAVEFORMATEX but does not include the header
// that defines them. windows.h only supplies them when WIN32_LEAN_AND_MEAN is
// absent, so relying on that makes this file depend on how it was configured.
#include <mmreg.h>
#include <dsound.h>

#include "../context_impl.hpp"
#include "../../internal/compat/vtable.hpp"
#include "../../internal/compat/dsound_slots.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  DirectSound 8 vtable hooks
//
//  Hooks IDirectSound8::CreateSoundBuffer to intercept buffer creation, then
//  hooks IDirectSoundBuffer8::Play, Stop, SetVolume, and Lock on each buffer.
//
//  All IDirectSoundBuffer8 objects from the same DLL share a vtable, so the
//  vtable patches are installed once on the first buffer.
// ─────────────────────────────────────────────────────────────────────────────

namespace vangfx::audio::ds_hooks {

// Deliberately NOT `using namespace dsound_slots`. That namespace contains
// sub-namespaces named IDirectSound8 and IDirectSoundBuffer8 -- exactly the
// names of the COM interfaces from dsound.h -- so the directive hides the
// types behind the slot tables, and every `IDirectSound8*` in this file stops
// naming a type. Slots are spelled out in full below instead.
namespace slots = dsound_slots;

// ── IDirectSound8 originals ───────────────────────────────────────────────────

using CreateSoundBuffer_t = HRESULT(STDMETHODCALLTYPE*)(
    IDirectSound8*, const DSBUFFERDESC*, IDirectSoundBuffer**, IUnknown*);

static CreateSoundBuffer_t orig_CreateSoundBuffer = nullptr;

// ── IDirectSoundBuffer8 originals ─────────────────────────────────────────────

using DSBuf_Play_t      = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
using DSBuf_Stop_t      = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*);
using DSBuf_SetVolume_t = HRESULT(STDMETHODCALLTYPE*)(IDirectSoundBuffer*, LONG);
using DSBuf_Lock_t      = HRESULT(STDMETHODCALLTYPE*)(
    IDirectSoundBuffer*, DWORD, DWORD, LPVOID*, LPDWORD, LPVOID*, LPDWORD, DWORD);

static DSBuf_Play_t      orig_Play      = nullptr;
static DSBuf_Stop_t      orig_Stop      = nullptr;
static DSBuf_SetVolume_t orig_SetVolume = nullptr;
static DSBuf_Lock_t      orig_Lock      = nullptr;

static std::atomic<bool> buf_vtable_patched{false};

// ── Helper: build WaveFormat from DSBUFFERDESC ────────────────────────────────

static WaveFormat from_dsbdesc(const DSBUFFERDESC* desc) {
    WaveFormat f;
    if (desc && desc->lpwfxFormat) {
        const WAVEFORMATEX* wfx = desc->lpwfxFormat;
        f.channels          = wfx->nChannels;
        f.sample_rate       = wfx->nSamplesPerSec;
        f.bits_per_sample   = wfx->wBitsPerSample;
        f.block_align       = wfx->nBlockAlign;
        f.avg_bytes_per_sec = wfx->nAvgBytesPerSec;
        f.is_float          = (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        f.is_extensible     = (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    }
    return f;
}

// ── Buffer vtable hooks ───────────────────────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hook_Play(IDirectSoundBuffer* self,
                                            DWORD dwReserved1, DWORD dwPriority,
                                            DWORD dwFlags)
{
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) {
        VoiceHandle h = ctx->find_voice(self);
        if (h.valid()) {
            // Synthesise a BufferSubmitEvent for Play — no raw PCM pointer available.
            BufferSubmitEvent ev{};
            ev.voice      = h;
            ev.audio_data = nullptr;
            ev.loop_count = (dwFlags & DSBPLAY_LOOPING) ? 0xFF : 0;
            ev.intercept  = false;
            ctx->dispatch_buffer_submit(ev);
            if (ev.intercept) return DS_OK;
        }
    }
    return orig_Play(self, dwReserved1, dwPriority, dwFlags);
}

static HRESULT STDMETHODCALLTYPE hook_Stop(IDirectSoundBuffer* self) {
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) {
        VoiceHandle h = ctx->find_voice(self);
        if (h.valid()) {
            VoiceDestroyEvent ev{h, VoiceKind::DSBuffer, self};
            // Stop is not a destroy, but it signals the voice is inactive.
            // Emit a volume-change-to-zero as a proxy notification.
            VolumeChangeEvent vev{h, VoiceKind::DSBuffer, 0.f, 0};
            ctx->dispatch_volume_change(vev);
        }
    }
    return orig_Stop(self);
}

static HRESULT STDMETHODCALLTYPE hook_SetVolume(IDirectSoundBuffer* self, LONG lVolume) {
    auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
    if (ctx) {
        VoiceHandle h = ctx->find_voice(self);
        if (h.valid()) {
            // DirectSound volume is in 1/100 dB; convert to linear for parity.
            float linear = (lVolume == DSBVOLUME_MIN)
                ? 0.f
                : std::pow(10.f, static_cast<float>(lVolume) / 2000.f);
            VolumeChangeEvent ev{h, VoiceKind::DSBuffer, linear, 0};
            ctx->dispatch_volume_change(ev);
        }
    }
    return orig_SetVolume(self, lVolume);
}

static HRESULT STDMETHODCALLTYPE hook_Lock(
    IDirectSoundBuffer* self, DWORD dwOffset, DWORD dwBytes,
    LPVOID* ppvAudioPtr1, LPDWORD pdwAudioBytes1,
    LPVOID* ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags)
{
    HRESULT hr = orig_Lock(self, dwOffset, dwBytes,
                            ppvAudioPtr1, pdwAudioBytes1,
                            ppvAudioPtr2, pdwAudioBytes2, dwFlags);

    // After Lock succeeds, the caller will write PCM into ppvAudioPtr1/2.
    // We can't easily intercept that write here, so we fire a BufferSubmitEvent
    // after the unlock (see hook_Unlock below).  For now, just forward.
    return hr;
}

// ── Buffer vtable install ─────────────────────────────────────────────────────

static void hook_ds_buffer_vtable(IDirectSoundBuffer* buf) {
    if (buf_vtable_patched.exchange(true)) return;

    vtable::patch(buf, slots::IDirectSoundBuffer8::Play,
                  &hook_Play, &orig_Play);
    vtable::patch(buf, slots::IDirectSoundBuffer8::Stop,
                  &hook_Stop, &orig_Stop);
    vtable::patch(buf, slots::IDirectSoundBuffer8::SetVolume,
                  &hook_SetVolume, &orig_SetVolume);
    vtable::patch(buf, slots::IDirectSoundBuffer8::Lock,
                  &hook_Lock, &orig_Lock);
}

// ── IDirectSound8 hook: CreateSoundBuffer ────────────────────────────────────

static HRESULT STDMETHODCALLTYPE hook_CreateSoundBuffer(
    IDirectSound8*        self,
    const DSBUFFERDESC*   pcDSBufferDesc,
    IDirectSoundBuffer**  ppDSBuffer,
    IUnknown*             pUnkOuter)
{
    HRESULT hr = orig_CreateSoundBuffer(self, pcDSBufferDesc, ppDSBuffer, pUnkOuter);
    if (SUCCEEDED(hr) && ppDSBuffer && *ppDSBuffer) {
        auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
        if (ctx) {
            WaveFormat fmt = from_dsbdesc(pcDSBufferDesc);
            VoiceHandle h  = ctx->register_voice(VoiceKind::DSBuffer, fmt, *ppDSBuffer);
            hook_ds_buffer_vtable(*ppDSBuffer);

            VoiceCreateEvent ev{h, VoiceKind::DSBuffer, fmt, 0, *ppDSBuffer};
            ctx->dispatch_voice_create(ev);
        }
    }
    return hr;
}

// ── install_on_ds8 ────────────────────────────────────────────────────────────

void install_on_ds8(IDirectSound8* ds8) {
    if (!ds8) return;
    vtable::patch(ds8, slots::IDirectSound8::CreateSoundBuffer,
                  &hook_CreateSoundBuffer, &orig_CreateSoundBuffer);
}

} // namespace vangfx::audio::ds_hooks
