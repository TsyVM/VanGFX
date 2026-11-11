/**
 * src/backend/d3d11/post.cpp — post-processing passes at Present.
 *
 * The passes draw on the immediate context, which is also the game's. So they
 * run inside a device context state of their own (ID3D11DeviceContext1::
 * SwapDeviceContextState): every binding the game had is swapped out whole and
 * swapped back afterwards, exactly as it was, with nothing to save or restore
 * piece by piece. Each pass copies the back buffer to a texture and draws a
 * full-screen triangle into the back buffer reading it.
 *
 * Needs D3D11.1 (Windows 8 and later); without it post passes are skipped and
 * the reason is logged once.
 */

#include "d3d11_impl.hpp"
#include <core/shader_compile.hpp>
#include <format_map.hpp>

#include <cstring>
#include <unordered_map>

namespace vangfx::backend::d3d11 {

namespace {

constexpr const char* kFullscreenVS = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

template <typename T> void release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }

DXGI_FORMAT typed(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                                return f;
    }
}

struct Constants {
    float slots[8][4];
    float time, delta, width, height;
    float pad[28];   // to 256 bytes
};
static_assert(sizeof(Constants) == 256);

} // namespace

class PostProcessor {
public:
    explicit PostProcessor(D3D11ContextImpl& ctx) noexcept : ctx_(ctx) {}
    ~PostProcessor() { release_targets(); release_all(); }

    void run(IDXGISwapChain* swap_chain, const ContextImpl::PostPassList& passes, const D3D11ContextImpl::Timing& t);
    void release_targets() noexcept { release(scene_srv_); release(scene_); scene_desc_ = {}; }

private:
    bool init();
    void release_all() noexcept;
    void fail_once(const std::string& why) {
        if (!failed_) ctx_.log(LogLevel::Error, "D3D11 post-processing: " + why);
        failed_ = true;
    }

    D3D11ContextImpl&        ctx_;
    bool                     ready_ = false, failed_ = false;
    ID3D11DeviceContext1*    dc1_   = nullptr;
    ID3DDeviceContextState*  state_ = nullptr;   // ours, swapped in for the passes
    ID3D11VertexShader*      vs_    = nullptr;
    ID3D11SamplerState*      sampler_ = nullptr;
    ID3D11Buffer*            cb_    = nullptr;
    ID3D11Texture2D*         scene_ = nullptr;
    ID3D11ShaderResourceView* scene_srv_ = nullptr;
    D3D11_TEXTURE2D_DESC     scene_desc_{};
    std::unordered_map<uint64_t, ID3D11PixelShader*> ps_;
};

void destroy_post_processor(PostProcessor* p) noexcept { delete p; }

bool PostProcessor::init() {
    if (ready_) return true;
    if (failed_) return false;
    ID3D11Device* dev = ctx_.device();
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev1))) || !dev1 ||
        FAILED(ctx_.immediate()->QueryInterface(IID_PPV_ARGS(&dc1_))) || !dc1_) {
        release(dev1);
        fail_once("the device has no Direct3D 11.1 interfaces; post passes need SwapDeviceContextState.");
        return false;
    }
    const D3D_FEATURE_LEVEL level = dev->GetFeatureLevel();
    const UINT flags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                     ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    const HRESULT hr = dev1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
                                                      __uuidof(ID3D11Device), nullptr, &state_);
    release(dev1);
    if (FAILED(hr)) { fail_once("CreateDeviceContextState failed."); return false; }

    auto vs = detail::compile_hlsl(ShaderBuilder{}.source(kFullscreenVS).entry("main").profile("vs_5_0"), "vs_5_0");
    if (!vs) { fail_once("could not compile the full-screen vertex shader: " + vs.error().message); return false; }
    if (FAILED(dev->CreateVertexShader(vs->data(), vs->size(), nullptr, &vs_))) { fail_once("vertex shader refused."); return false; }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC bd{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(dev->CreateSamplerState(&sd, &sampler_)) || FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) {
        fail_once("could not create the sampler or constant buffer.");
        return false;
    }
    ready_ = true;
    return true;
}

void PostProcessor::run(IDXGISwapChain* swap_chain, const ContextImpl::PostPassList& passes, const D3D11ContextImpl::Timing& t) {
    if (passes.empty()) return;
    InternalScope internal_work;
    if (!init()) return;
    ID3D11Device* dev = ctx_.device();

    // Shaders of passes since removed.
    for (auto it = ps_.begin(); it != ps_.end();) {
        bool live = false;
        for (const auto& p : passes) live |= p.id == it->first;
        if (live) { ++it; continue; }
        if (it->second) it->second->Release();
        it = ps_.erase(it);
    }

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) { fail_once("could not get the back buffer."); return; }
    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    if (bd.SampleDesc.Count > 1) { bb->Release(); fail_once("the back buffer is multisampled; post passes need a resolved one."); return; }

    if (!scene_ || scene_desc_.Width != bd.Width || scene_desc_.Height != bd.Height || scene_desc_.Format != bd.Format) {
        release_targets();
        D3D11_TEXTURE2D_DESC sd = bd;
        sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.CPUAccessFlags = 0;
        sd.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = typed(bd.Format);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &scene_)) || FAILED(dev->CreateShaderResourceView(scene_, &vd, &scene_srv_))) {
            bb->Release();
            release_targets();
            fail_once("could not create the texture the frame is copied into.");
            return;
        }
        scene_desc_ = bd;
    }
    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = typed(bd.Format);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateRenderTargetView(bb, &rd, &rtv))) { bb->Release(); fail_once("could not view the back buffer."); return; }

    Constants c{};
    const auto slots = ctx_.uniforms();
    for (int i = 0; i < 8; ++i) std::memcpy(c.slots[i], slots[i].data(), 16);
    c.time = t.time; c.delta = t.delta;
    c.width = static_cast<float>(bd.Width); c.height = static_cast<float>(bd.Height);

    ID3DDeviceContextState* game = nullptr;
    dc1_->SwapDeviceContextState(state_, &game);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(dc1_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &c, sizeof(c));
        dc1_->Unmap(cb_, 0);
    }
    const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(bd.Width), static_cast<float>(bd.Height), 0, 1 };
    for (const auto& pass : passes) {
        ID3D11PixelShader*& ps = ps_[pass.id];
        if (!ps && FAILED(dev->CreatePixelShader(pass.ps.data(), pass.ps.size(), nullptr, &ps))) {
            ctx_.log(LogLevel::Error, "D3D11 post-processing: a pass's shader was refused; that pass is skipped.");
            ps = nullptr;
            continue;
        }
        if (!ps) continue;
        ID3D11ShaderResourceView* no_srv = nullptr;
        dc1_->PSSetShaderResources(0, 1, &no_srv);   // the copy writes what the last pass read
        dc1_->CopyResource(scene_, bb);
        dc1_->OMSetRenderTargets(1, &rtv, nullptr);
        dc1_->RSSetViewports(1, &vp);
        dc1_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc1_->VSSetShader(vs_, nullptr, 0);
        dc1_->PSSetShader(ps, nullptr, 0);
        dc1_->PSSetShaderResources(0, 1, &scene_srv_);
        dc1_->PSSetSamplers(0, 1, &sampler_);
        dc1_->PSSetConstantBuffers(0, 1, &cb_);
        dc1_->Draw(3, 0);
    }
    dc1_->ClearState();   // our state object holds no references past the passes
    dc1_->SwapDeviceContextState(game, nullptr);
    if (game) game->Release();
    rtv->Release();
    bb->Release();
}

void PostProcessor::release_all() noexcept {
    for (auto& [id, p] : ps_) if (p) p->Release();
    ps_.clear();
    release(cb_);
    release(sampler_);
    release(vs_);
    release(state_);
    release(dc1_);
}

void run_post_passes(D3D11ContextImpl& ctx, PostProcessor*& processor, IDXGISwapChain* swap_chain,
                     const D3D11ContextImpl::Timing& t) {
    const auto passes = ctx.post_passes();
    if (!passes || passes->empty()) return;
    if (!processor) processor = new PostProcessor(ctx);
    processor->run(swap_chain, *passes, t);
}

void release_post_targets(PostProcessor* p) noexcept {
    if (p) p->release_targets();
}

}  // namespace vangfx::backend::d3d11
