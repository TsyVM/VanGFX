#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>
#define DIRECTSOUND_VERSION 0x0800
#include <dsound.h>

#include "context_impl.hpp"
#include "wav.hpp"
#include "../../include/vangfx/audio/context.hpp"

#include <fstream>
#include <shared_mutex>

// ── XAudio2 hook thunks (forward declarations) ────────────────────────────────
namespace vangfx::audio::xa2_hooks {
    void install_on_xa2(IXAudio2* xa2);
}
namespace vangfx::audio::ds_hooks {
    void install_on_ds8(IDirectSound8* ds8);
}

// ── Hook for XAudio2Create export ────────────────────────────────────────────
namespace vangfx::audio {

static IXAudio2* hook_xa2_instance = nullptr;

using XA2Create_t = HRESULT(WINAPI*)(IXAudio2**, UINT32, UINT32);
static XA2Create_t orig_XAudio2Create = nullptr;

static HRESULT WINAPI hook_XAudio2Create(IXAudio2** ppXAudio2,
                                          UINT32     Flags,
                                          UINT32     Processor)
{
    HRESULT hr = orig_XAudio2Create(ppXAudio2, Flags, Processor);
    if (SUCCEEDED(hr) && ppXAudio2 && *ppXAudio2) {
        auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
        if (ctx && !ctx->xa2_game_) {
            ctx->xa2_game_ = *ppXAudio2;
            xa2_hooks::install_on_xa2(*ppXAudio2);
        }
    }
    return hr;
}

// Hook for DirectSoundCreate8 export
using DSCreate8_t = HRESULT(WINAPI*)(const GUID*, IDirectSound8**, IUnknown*);
static DSCreate8_t orig_DSCreate8 = nullptr;

static HRESULT WINAPI hook_DSCreate8(const GUID* pcGuidDevice,
                                      IDirectSound8** ppDS8,
                                      IUnknown*       pUnkOuter)
{
    HRESULT hr = orig_DSCreate8(pcGuidDevice, ppDS8, pUnkOuter);
    if (SUCCEEDED(hr) && ppDS8 && *ppDS8) {
        auto* ctx = g_audio_ctx.load(std::memory_order_relaxed);
        if (ctx && !ctx->ds8_) {
            ctx->ds8_ = *ppDS8;
            ds_hooks::install_on_ds8(*ppDS8);
        }
    }
    return hr;
}

} // namespace vangfx::audio

// ─────────────────────────────────────────────────────────────────────────────

namespace vangfx::audio {

std::atomic<AudioContextImpl*> g_audio_ctx{nullptr};

// ── AudioContextImpl ──────────────────────────────────────────────────────────

AudioContextImpl::AudioContextImpl(AudioBackendKind backend,
                                   AudioLogLevel    log_level,
                                   AudioLogCallback log_cb)
    : active_backend_(backend)
    , log_level_(log_level)
    , log_cb_(std::move(log_cb))
{}

AudioContextImpl::~AudioContextImpl() { shutdown(); }

void AudioContextImpl::shutdown() {
    g_audio_ctx.store(nullptr, std::memory_order_seq_cst);

    xa2_create_patch_.remove();
    ds_create_patch_.remove();

    {
        std::lock_guard lk(playing_mutex_);
        for (auto& pv : playing_) {
            if (pv.xa2_voice) {
                pv.xa2_voice->Stop(0);
                pv.xa2_voice->DestroyVoice();
            }
        }
        playing_.clear();
    }

    if (xa2_fw_master_) { xa2_fw_master_->DestroyVoice(); xa2_fw_master_ = nullptr; }
    if (xa2_fw_)        { xa2_fw_->Release();              xa2_fw_        = nullptr; }
}

// ── Init ──────────────────────────────────────────────────────────────────────

Result<void> AudioContextImpl::init_xaudio2(void* explicit_xa2) {
    HMODULE mod = GetModuleHandleW(L"xaudio2_9.dll");
    if (!mod) mod = GetModuleHandleW(L"xaudio2_8.dll");

    auto* create_fn = mod
        ? reinterpret_cast<XA2Create_t>(GetProcAddress(mod, "XAudio2Create"))
        : nullptr;

    if (explicit_xa2) {
        xa2_game_ = static_cast<IXAudio2*>(explicit_xa2);
        xa2_hooks::install_on_xa2(xa2_game_);
        log(AudioLogLevel::Info, "XAudio2 backend — using provided instance");
    } else if (create_fn) {
        // Hook XAudio2Create so we catch the game's instance when it's created.
        orig_XAudio2Create = create_fn;
        xa2_create_patch_.target = reinterpret_cast<void*>(create_fn);
        auto res = xa2_create_patch_.install(reinterpret_cast<void*>(&hook_XAudio2Create));
        if (!res)
            log(AudioLogLevel::Warning,
                "XAudio2Create hook failed — game voices will not be intercepted");
        else
            log(AudioLogLevel::Info, "XAudio2 backend — XAudio2Create hook installed");
    } else {
        log(AudioLogLevel::Warning, "XAudio2 not loaded yet — hook deferred");
    }

    active_backend_ = AudioBackendKind::XAudio2;
    return create_fw_xa2();
}

Result<void> AudioContextImpl::init_dsound() {
    HMODULE mod = GetModuleHandleW(L"dsound.dll");
    if (!mod)
        return gfx_err<void>(ShaderError::Unsupported, "dsound.dll not loaded");

    auto* create_fn = reinterpret_cast<DSCreate8_t>(
        GetProcAddress(mod, "DirectSoundCreate8"));
    if (!create_fn)
        return gfx_err<void>(ShaderError::Unsupported, "DirectSoundCreate8 not found");

    orig_DSCreate8 = create_fn;
    ds_create_patch_.target = reinterpret_cast<void*>(create_fn);
    auto res = ds_create_patch_.install(reinterpret_cast<void*>(&hook_DSCreate8));
    if (!res)
        log(AudioLogLevel::Warning, "DirectSoundCreate8 hook failed");
    else
        log(AudioLogLevel::Info, "DirectSound backend — DirectSoundCreate8 hook installed");

    active_backend_ = AudioBackendKind::DirectSound;
    return {};
}

Result<void> AudioContextImpl::create_fw_xa2() {
    HMODULE mod = GetModuleHandleW(L"xaudio2_9.dll");
    if (!mod) mod = GetModuleHandleW(L"xaudio2_8.dll");
    if (!mod) {
        log(AudioLogLevel::Warning, "XAudio2 not loaded — framework playback unavailable");
        return {};
    }

    // Use the trampoline (original) if the hook is installed; otherwise direct.
    XA2Create_t fn = xa2_create_patch_.installed
        ? reinterpret_cast<XA2Create_t>(xa2_create_patch_.trampoline)
        : reinterpret_cast<XA2Create_t>(GetProcAddress(mod, "XAudio2Create"));
    if (!fn) return {};

    IXAudio2* xa2 = nullptr;
    if (FAILED(fn(&xa2, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        log(AudioLogLevel::Warning, "Framework XAudio2Create failed — playback unavailable");
        return {};
    }

    IXAudio2MasteringVoice* mv = nullptr;
    if (FAILED(xa2->CreateMasteringVoice(&mv))) {
        xa2->Release();
        log(AudioLogLevel::Warning, "Framework CreateMasteringVoice failed");
        return {};
    }

    xa2_fw_        = xa2;
    xa2_fw_master_ = mv;
    log(AudioLogLevel::Info, "Framework XAudio2 playback device ready");
    return {};
}

// ── Callback registration ──────────────────────────────────────────────────────

void AudioContextImpl::on_voice_create(std::function<void(VoiceCreateEvent&)> cb) {
    std::unique_lock lk(cb_mutex_);
    voice_create_cbs_.push_back(std::move(cb));
}
void AudioContextImpl::on_voice_destroy(std::function<void(VoiceDestroyEvent&)> cb) {
    std::unique_lock lk(cb_mutex_);
    voice_destroy_cbs_.push_back(std::move(cb));
}
void AudioContextImpl::on_buffer_submit(std::function<void(BufferSubmitEvent&)> cb) {
    std::unique_lock lk(cb_mutex_);
    buffer_submit_cbs_.push_back(std::move(cb));
}
void AudioContextImpl::on_volume_change(std::function<void(VolumeChangeEvent&)> cb) {
    std::unique_lock lk(cb_mutex_);
    volume_change_cbs_.push_back(std::move(cb));
}
void AudioContextImpl::on_engine_start(std::function<void()> cb) {
    std::unique_lock lk(cb_mutex_);
    engine_start_cbs_.push_back(std::move(cb));
}
void AudioContextImpl::on_engine_stop(std::function<void()> cb) {
    std::unique_lock lk(cb_mutex_);
    engine_stop_cbs_.push_back(std::move(cb));
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void AudioContextImpl::dispatch_voice_create(VoiceCreateEvent& ev) {
    std::shared_lock lk(cb_mutex_);
    for (auto& cb : voice_create_cbs_) cb(ev);
}
void AudioContextImpl::dispatch_voice_destroy(VoiceDestroyEvent& ev) {
    std::shared_lock lk(cb_mutex_);
    for (auto& cb : voice_destroy_cbs_) cb(ev);
}
void AudioContextImpl::dispatch_buffer_submit(BufferSubmitEvent& ev) {
    std::shared_lock lk(cb_mutex_);
    for (auto& cb : buffer_submit_cbs_) cb(ev);
}
void AudioContextImpl::dispatch_volume_change(VolumeChangeEvent& ev) {
    std::shared_lock lk(cb_mutex_);
    for (auto& cb : volume_change_cbs_) cb(ev);
}
void AudioContextImpl::dispatch_engine_start() {
    std::shared_lock lk(cb_mutex_);
    for (auto& cb : engine_start_cbs_) cb();
}
void AudioContextImpl::dispatch_engine_stop() {
    std::shared_lock lk(cb_mutex_);
    for (auto& cb : engine_stop_cbs_) cb();
}

// ── Voice registry ────────────────────────────────────────────────────────────

VoiceHandle AudioContextImpl::register_voice(VoiceKind kind,
                                              const WaveFormat& fmt,
                                              void* raw) {
    VoiceHandle h{next_voice_id_++};
    std::unique_lock lk(voice_mutex_);
    voices_[raw] = {h, kind, fmt};
    return h;
}

void AudioContextImpl::unregister_voice(void* raw) {
    std::unique_lock lk(voice_mutex_);
    voices_.erase(raw);
}

VoiceHandle AudioContextImpl::find_voice(void* raw) const {
    std::shared_lock lk(voice_mutex_);
    auto it = voices_.find(raw);
    return (it != voices_.end()) ? it->second.handle : VoiceHandle{};
}

// ── Playback ──────────────────────────────────────────────────────────────────

Result<SoundHandle> AudioContextImpl::load_wav(std::string_view path) {
    std::ifstream f(std::string(path), std::ios::binary | std::ios::ate);
    if (!f)
        return gfx_err<SoundHandle>(ShaderError::FileNotFound, "could not open WAV file");

    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));

    return load_wav_memory(buf.data(), buf.size());
}

Result<SoundHandle> AudioContextImpl::load_wav_memory(const void* data, size_t bytes) {
    auto parsed = parse_wav(data, bytes);
    if (!parsed) return std::unexpected(parsed.error());

    SoundHandle h = alloc_sound_id();
    std::lock_guard lk(sound_mutex_);
    sounds_[h.id] = {std::move(parsed->pcm), parsed->format};
    return h;
}

Result<SoundEntry> AudioContextImpl::find_sound(SoundHandle h) const {
    std::lock_guard lk(sound_mutex_);
    auto it = sounds_.find(h.id);
    if (it == sounds_.end())
        return gfx_err<SoundEntry>(ShaderError::InvalidHandle, "unknown SoundHandle");
    return it->second;
}

Result<void> AudioContextImpl::play(SoundHandle h, float volume, float pitch) {
    return play_internal(h, false, volume, pitch);
}
Result<void> AudioContextImpl::play_looping(SoundHandle h, float volume, float pitch) {
    return play_internal(h, true, volume, pitch);
}

Result<void> AudioContextImpl::play_internal(SoundHandle h, bool loop,
                                              float volume, float pitch) {
    if (!xa2_fw_)
        return gfx_err<void>(ShaderError::NotInitialized, "framework XAudio2 not available");

    auto sound = find_sound(h);
    if (!sound) return std::unexpected(sound.error());

    const WaveFormat& fmt = sound->format;
    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = fmt.is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    wfx.nChannels       = fmt.channels;
    wfx.nSamplesPerSec  = fmt.sample_rate;
    wfx.wBitsPerSample  = fmt.bits_per_sample;
    wfx.nBlockAlign     = fmt.block_align ? fmt.block_align
                            : static_cast<WORD>((fmt.channels * fmt.bits_per_sample) / 8);
    wfx.nAvgBytesPerSec = fmt.avg_bytes_per_sec ? fmt.avg_bytes_per_sec
                            : wfx.nSamplesPerSec * wfx.nBlockAlign;

    IXAudio2SourceVoice* voice = nullptr;
    HRESULT hr = xa2_fw_->CreateSourceVoice(&voice, &wfx);
    if (FAILED(hr))
        return gfx_err<void>(ShaderError::NotInitialized, "CreateSourceVoice failed");

    voice->SetVolume(volume);
    voice->SetFrequencyRatio(pitch);

    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(sound->pcm.size());
    buf.pAudioData = sound->pcm.data();
    buf.Flags      = XAUDIO2_END_OF_STREAM;
    buf.LoopCount  = loop ? XAUDIO2_LOOP_INFINITE : 0;

    hr = voice->SubmitSourceBuffer(&buf);
    if (FAILED(hr)) { voice->DestroyVoice(); return gfx_err<void>(ShaderError::NotInitialized, "SubmitSourceBuffer failed"); }

    hr = voice->Start(0);
    if (FAILED(hr)) { voice->DestroyVoice(); return gfx_err<void>(ShaderError::NotInitialized, "Start failed"); }

    std::lock_guard lk(playing_mutex_);
    playing_.push_back({h, voice});
    return {};
}

Result<void> AudioContextImpl::stop(SoundHandle h) {
    std::lock_guard lk(playing_mutex_);
    for (auto it = playing_.begin(); it != playing_.end();) {
        if (it->sound == h) {
            it->xa2_voice->Stop(0);
            it->xa2_voice->DestroyVoice();
            it = playing_.erase(it);
        } else {
            ++it;
        }
    }
    return {};
}

Result<void> AudioContextImpl::stop_all() {
    std::lock_guard lk(playing_mutex_);
    for (auto& pv : playing_) {
        pv.xa2_voice->Stop(0);
        pv.xa2_voice->DestroyVoice();
    }
    playing_.clear();
    return {};
}

Result<void> AudioContextImpl::set_master_volume(float v) {
    if (xa2_fw_master_) xa2_fw_master_->SetVolume(v);
    if (xa2_game_master_) xa2_game_master_->SetVolume(v);
    return {};
}

// ── DSP effects ───────────────────────────────────────────────────────────────

Result<AudioEffectHandle> AudioContextImpl::inject_master_effect(AudioDspCallback cb) {
    AudioEffectHandle h = alloc_effect_id();
    std::lock_guard lk(effect_mutex_);
    master_effects_.emplace_back(h, std::move(cb));
    log(AudioLogLevel::Info, "master DSP effect registered (XAPO bridge not yet wired — callback stored for future use)");
    return h;
}

Result<void> AudioContextImpl::remove_effect(AudioEffectHandle h) {
    std::lock_guard lk(effect_mutex_);
    auto it = std::find_if(master_effects_.begin(), master_effects_.end(),
                            [&](auto& p){ return p.first == h; });
    if (it == master_effects_.end())
        return gfx_err<void>(ShaderError::InvalidHandle, "unknown AudioEffectHandle");
    master_effects_.erase(it);
    return {};
}

// ── Capture ───────────────────────────────────────────────────────────────────

Result<void> AudioContextImpl::start_capture(AudioCaptureConfig cfg) {
    std::lock_guard lk(capture_mutex_);
    if (capturing_)
        return gfx_err<void>(ShaderError::CaptureNotActive, "capture already active");
    capture_cfg_ = std::move(cfg);
    capturing_   = true;
    log(AudioLogLevel::Info, "audio capture started");
    return {};
}

Result<void> AudioContextImpl::stop_capture() {
    std::lock_guard lk(capture_mutex_);
    if (!capturing_)
        return gfx_err<void>(ShaderError::CaptureNotActive, "capture not active");
    capturing_ = false;
    log(AudioLogLevel::Info, "audio capture stopped");
    return {};
}

// ── Logging ───────────────────────────────────────────────────────────────────

void AudioContextImpl::log(AudioLogLevel level, std::string_view msg) const {
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(log_level_)) return;
    if (log_cb_) log_cb_(level, msg);
}

// ── AudioCaptureSession ───────────────────────────────────────────────────────

} // namespace vangfx::audio

namespace vangfx {

AudioCaptureSession::AudioCaptureSession(AudioContext& ctx, AudioCaptureConfig cfg)
    : ctx_(ctx)
{
    ctx_.start_capture(std::move(cfg));
}
AudioCaptureSession::~AudioCaptureSession() { ctx_.stop_capture(); }

// ── AudioContextBuilder::build ────────────────────────────────────────────────

Result<std::unique_ptr<AudioContext>> AudioContextBuilder::build() {
    auto impl = std::make_unique<audio::AudioContextImpl>(
        backend_, log_level_, log_cb_);

    audio::g_audio_ctx.store(impl.get(), std::memory_order_seq_cst);

    Result<void> res;
    if (backend_ == AudioBackendKind::XAudio2 ||
        backend_ == AudioBackendKind::Auto)
    {
        res = impl->init_xaudio2(xa2_ptr_);
        if (res || backend_ == AudioBackendKind::XAudio2)
            goto done;
    }
    res = impl->init_dsound();

done:
    if (!res) {
        audio::g_audio_ctx.store(nullptr, std::memory_order_seq_cst);
        return std::unexpected(res.error());
    }
    return impl;
}

} // namespace vangfx
