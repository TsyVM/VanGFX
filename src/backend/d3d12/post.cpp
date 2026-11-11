/**
 * src/backend/d3d12/post.cpp — post-processing passes at Present.
 *
 * VanGFX's own pipeline, so nothing of the game's is touched: a root signature
 * of root constants (vangfx_params, b0), one SRV (the frame, t0) and a static
 * linear-clamp sampler (s0); a full-screen-triangle vertex shader; and a
 * pipeline per (pass, back-buffer format). Each pass copies the back buffer to
 * a texture, then draws into the back buffer reading that texture, so each pass
 * sees what the one before it wrote. The work is recorded on VanGFX's own
 * command list and executed on the swap chain's queue just before Present.
 */

#include "d3d12_impl.hpp"
#include <core/shader_compile.hpp>
#include <vangfx/core/shader.hpp>

#include <dxgi1_4.h>

#include <cstring>
#include <unordered_map>

namespace vangfx::backend::d3d12 {

namespace {

constexpr UINT kAllocators = 3;
constexpr UINT kRootConstants = 36;   // float4[8], time, delta, resolution.xy

constexpr const char* kFullscreenVS = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

template <typename T> void release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) noexcept {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource   = r;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter  = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return x;
}

DXGI_FORMAT typed(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                               return f;
    }
}

} // namespace

class PostProcessor {
public:
    explicit PostProcessor(D3D12ContextImpl& ctx) noexcept : ctx_(ctx) {}
    ~PostProcessor() { release_all(); }

    void run(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue, const ContextImpl::PostPassList& passes,
             const D3D12ContextImpl::Timing& timing);

private:
    bool init();
    bool fit_scene(const D3D12_RESOURCE_DESC& bb);
    ID3D12PipelineState* pipeline(const ContextImpl::PostPass& pass, DXGI_FORMAT format);
    void wait(uint64_t value) noexcept;
    void release_all() noexcept;
    void fail_once(const std::string& why) {
        if (!failed_) ctx_.log(LogLevel::Error, "D3D12 post-processing: " + why);
        failed_ = true;
    }

    D3D12ContextImpl&          ctx_;
    bool                       ready_  = false;
    bool                       failed_ = false;
    ID3D12RootSignature*       root_   = nullptr;
    std::vector<uint8_t>       vs_;
    ID3D12DescriptorHeap*      srv_heap_ = nullptr;   // shader visible, one SRV
    ID3D12DescriptorHeap*      rtv_heap_ = nullptr;   // one RTV, rewritten each frame
    ID3D12CommandAllocator*    alloc_[kAllocators]{};
    uint64_t                   alloc_fence_[kAllocators]{};
    uint32_t                   next_alloc_ = 0;
    ID3D12GraphicsCommandList* list_  = nullptr;
    ID3D12Fence*               fence_ = nullptr;
    HANDLE                     event_ = nullptr;
    uint64_t                   fence_value_ = 0;
    ID3D12Resource*            scene_ = nullptr;
    D3D12_RESOURCE_DESC        scene_desc_{};
    struct Key { uint64_t id; DXGI_FORMAT format; bool operator==(const Key&) const = default; };
    struct KeyHash { size_t operator()(const Key& k) const noexcept { return std::hash<uint64_t>()(k.id * 131 + k.format); } };
    std::unordered_map<Key, ID3D12PipelineState*, KeyHash> psos_;
};

void destroy_post_processor(PostProcessor* p) noexcept { delete p; }

bool PostProcessor::init() {
    if (ready_) return true;
    if (failed_) return false;
    ID3D12Device* dev = ctx_.device();

    D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants                = { 0, 0, kRootConstants };
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable          = { 1, &range };
    params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD   = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{ 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr;
    ID3DBlob* err  = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        release(err);
        fail_once("could not serialise the root signature.");
        return false;
    }
    const HRESULT hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
    release(blob);
    if (FAILED(hr)) { fail_once("could not create the root signature."); return false; }

    const std::string profile = detail::d3d12_default_profile("vs");
    auto vs = detail::compile_hlsl(ShaderBuilder{}.source(kFullscreenVS).entry("main").profile(profile), profile);
    if (!vs) { fail_once("could not compile the full-screen vertex shader: " + vs.error().message); return false; }
    vs_ = std::move(*vs);

    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC rd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_))) ||
        FAILED(dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtv_heap_)))) {
        fail_once("could not create descriptor heaps.");
        return false;
    }
    for (auto& a : alloc_) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) {
            fail_once("could not create command allocators.");
            return false;
        }
    }
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_PPV_ARGS(&list_))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        fail_once("could not create the command list or fence.");
        return false;
    }
    list_->Close();
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = event_ != nullptr;
    return ready_;
}

void PostProcessor::wait(uint64_t value) noexcept {
    if (!value || fence_->GetCompletedValue() >= value) return;
    if (SUCCEEDED(fence_->SetEventOnCompletion(value, event_))) WaitForSingleObject(event_, 5000);
}

bool PostProcessor::fit_scene(const D3D12_RESOURCE_DESC& bb) {
    if (scene_ && scene_desc_.Width == bb.Width && scene_desc_.Height == bb.Height && scene_desc_.Format == bb.Format)
        return true;
    // A resize: the old copy may still be read by a pass in flight.
    wait(fence_value_);
    release(scene_);
    D3D12_RESOURCE_DESC d = bb;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    d.MipLevels = 1;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    if (FAILED(ctx_.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr, IID_PPV_ARGS(&scene_)))) {
        fail_once("could not create the texture the frame is copied into.");
        return false;
    }
    scene_desc_ = bb;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                  = typed(bb.Format);
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    ctx_.device()->CreateShaderResourceView(scene_, &sd, srv_heap_->GetCPUDescriptorHandleForHeapStart());
    return true;
}

ID3D12PipelineState* PostProcessor::pipeline(const ContextImpl::PostPass& pass, DXGI_FORMAT format) {
    const Key key{ pass.id, format };
    if (auto it = psos_.find(key); it != psos_.end()) return it->second;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_;
    d.VS = { vs_.data(), vs_.size() };
    d.PS = { pass.ps.data(), pass.ps.size() };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0]    = format;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(ctx_.device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) pso = nullptr;
    psos_.emplace(key, pso);   // a refusal is remembered too, not retried every frame
    if (!pso) ctx_.log(LogLevel::Error, "D3D12 post-processing: a pass's pipeline was refused; that pass is skipped.");
    return pso;
}

void PostProcessor::run(IDXGISwapChain* swap_chain, ID3D12CommandQueue* queue, const ContextImpl::PostPassList& passes,
                        const D3D12ContextImpl::Timing& timing) {
    if (passes.empty()) return;
    InternalScope internal_work;
    if (!init()) return;

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(swap_chain->QueryInterface(IID_PPV_ARGS(&sc3)))) { fail_once("the swap chain is not an IDXGISwapChain3."); return; }
    ID3D12Resource* bb = nullptr;
    const HRESULT got = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(got) || !bb) { fail_once("could not get the back buffer."); return; }

    // Pipelines of passes since removed. Rare, so waiting for the GPU to finish
    // with them first costs nothing that matters.
    for (auto it = psos_.begin(); it != psos_.end();) {
        bool live = false;
        for (const auto& p : passes) live |= p.id == it->first.id;
        if (live) { ++it; continue; }
        wait(fence_value_);
        if (it->second) it->second->Release();
        it = psos_.erase(it);
    }

    const D3D12_RESOURCE_DESC bd = bb->GetDesc();
    const DXGI_FORMAT rtv_format = typed(bd.Format);
    if (!fit_scene(bd)) { bb->Release(); return; }

    const uint32_t slot = next_alloc_;
    next_alloc_ = (next_alloc_ + 1) % kAllocators;
    wait(alloc_fence_[slot]);
    alloc_[slot]->Reset();
    list_->Reset(alloc_[slot], nullptr);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rvd{};
    rvd.Format        = rtv_format;
    rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    ctx_.device()->CreateRenderTargetView(bb, &rvd, rtv);

    float constants[kRootConstants]{};
    const auto slots = ctx_.uniforms();
    for (int i = 0; i < 8; ++i) std::memcpy(&constants[i * 4], slots[i].data(), 16);
    constants[32] = timing.time;
    constants[33] = timing.delta;
    constants[34] = static_cast<float>(bd.Width);
    constants[35] = static_cast<float>(bd.Height);

    const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(bd.Width), static_cast<float>(bd.Height), 0, 1 };
    const D3D12_RECT     sr{ 0, 0, static_cast<LONG>(bd.Width), static_cast<LONG>(bd.Height) };
    D3D12_RESOURCE_STATES scene_state = D3D12_RESOURCE_STATE_COMMON;
    bool drew = false;

    for (const auto& pass : passes) {
        ID3D12PipelineState* pso = pipeline(pass, rtv_format);
        if (!pso) continue;
        D3D12_RESOURCE_BARRIER to_copy[2] = {
            transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(scene_, scene_state, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list_->ResourceBarrier(scene_state == D3D12_RESOURCE_STATE_COPY_DEST ? 1 : 2, to_copy);
        list_->CopyResource(scene_, bb);
        D3D12_RESOURCE_BARRIER to_draw[2] = {
            transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            transition(scene_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        list_->ResourceBarrier(2, to_draw);
        scene_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list_->RSSetViewports(1, &vp);
        list_->RSSetScissorRects(1, &sr);
        list_->SetGraphicsRootSignature(root_);
        list_->SetDescriptorHeaps(1, &srv_heap_);
        list_->SetGraphicsRoot32BitConstants(0, kRootConstants, constants, 0);
        list_->SetGraphicsRootDescriptorTable(1, srv_heap_->GetGPUDescriptorHandleForHeapStart());
        list_->SetPipelineState(pso);
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list_->DrawInstanced(3, 1, 0, 0);

        const D3D12_RESOURCE_BARRIER back = transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list_->ResourceBarrier(1, &back);
        drew = true;
    }
    if (scene_state != D3D12_RESOURCE_STATE_COMMON) {
        const D3D12_RESOURCE_BARRIER rest = transition(scene_, scene_state, D3D12_RESOURCE_STATE_COMMON);
        list_->ResourceBarrier(1, &rest);
    }
    list_->Close();
    if (drew) {
        ID3D12CommandList* lists[] = { list_ };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence_, ++fence_value_);
        alloc_fence_[slot] = fence_value_;
    }
    bb->Release();
}

void PostProcessor::release_all() noexcept {
    wait(fence_value_);
    for (auto& [k, p] : psos_) if (p) p->Release();
    psos_.clear();
    release(scene_);
    release(list_);
    for (auto& a : alloc_) release(a);
    release(fence_);
    release(srv_heap_);
    release(rtv_heap_);
    release(root_);
    if (event_) { CloseHandle(event_); event_ = nullptr; }
}

void run_post_passes(D3D12ContextImpl& ctx, PostProcessor*& processor, IDXGISwapChain* swap_chain,
                     ID3D12CommandQueue* queue, const D3D12ContextImpl::Timing& timing) {
    const auto passes = ctx.post_passes();
    if (!passes || passes->empty()) return;
    if (!processor) processor = new PostProcessor(ctx);
    processor->run(swap_chain, queue, *passes, timing);
}

} // namespace vangfx::backend::d3d12
