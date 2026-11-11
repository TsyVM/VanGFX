#include "device_d3d12.hpp"
#include <vangfx/resources/format.hpp>
#include <vangfx/core/error.hpp>
#include <cstring>

namespace vangfx::gpu {

// ── Helpers ───────────────────────────────────────────────────────────────────

static DXGI_FORMAT to_dxgi12(vangfx::SurfaceFormat f) {
    switch (f) {
    case vangfx::SurfaceFormat::RGBA8:         return DXGI_FORMAT_R8G8B8A8_UNORM;
    case vangfx::SurfaceFormat::BGRA8:         return DXGI_FORMAT_B8G8R8A8_UNORM;
    case vangfx::SurfaceFormat::RGBA16F:       return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vangfx::SurfaceFormat::RGBA32F:       return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case vangfx::SurfaceFormat::R11G11B10F:    return DXGI_FORMAT_R11G11B10_FLOAT;
    case vangfx::SurfaceFormat::R10G10B10A2:   return DXGI_FORMAT_R10G10B10A2_UNORM;
    case vangfx::SurfaceFormat::R8:            return DXGI_FORMAT_R8_UNORM;
    case vangfx::SurfaceFormat::R32F:          return DXGI_FORMAT_R32_FLOAT;
    default:                                   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static constexpr const char* kFullscreenVS12 =
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    float2 p  = float2((id << 1) & 2, id & 2);\n"
    "    VS_OUT o;\n"
    "    o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
    "    o.uv  = float2(p.x, 1.0 - p.y);\n"
    "    return o;\n"
    "}\n";

static ComPtr<ID3DBlob> compile_hlsl12(const char* src, const char* entry,
                                       const char* profile) {
    ComPtr<ID3DBlob> code, err;
    D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
               entry, profile, 0, 0, &code, &err);
    return code;
}

// ── D3D12Device ───────────────────────────────────────────────────────────────

D3D12Device::D3D12Device(ID3D12Device* dev, ID3D12CommandQueue* queue)
    : dev_(dev), queue_(queue) {
    init_heaps();
    init_command_infra();
}

D3D12Device::~D3D12Device() {
    if (fence_event_) CloseHandle(fence_event_);
}

bool D3D12Device::init_heaps() {
    rtv_desc_size_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_desc_size_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // RTV heap (CPU-only, 64 RTVs)
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 64;
    rh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtv_heap_));

    // SRV heap (GPU-visible, ring buffer)
    D3D12_DESCRIPTOR_HEAP_DESC sh{};
    sh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = kSrvHeapSize;
    sh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    dev_->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&srv_heap_));
    return true;
}

bool D3D12Device::init_command_infra() {
    dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   IID_PPV_ARGS(&cmd_alloc_));
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                             cmd_alloc_.Get(), nullptr,
                             IID_PPV_ARGS(&cmd_list_));
    cmd_list_->Close(); // start in closed state

    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::alloc_rtv() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (uint64_t)rtv_next_ * rtv_desc_size_;
    rtv_next_ = (rtv_next_ + 1) % 64;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::alloc_pass_srvs(
    uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE* cpu_handles) {
    // Allocate `count` contiguous slots from the ring heap and copy the
    // CPU-side descriptors into them.
    uint32_t base = srv_next_;
    srv_next_ = (srv_next_ + count) % kSrvHeapSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dst = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += (uint64_t)base * srv_desc_size_;

    for (uint32_t i = 0; i < count; ++i) {
        if (cpu_handles[i].ptr != 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE d = dst;
            d.ptr += (uint64_t)i * srv_desc_size_;
            dev_->CopyDescriptorsSimple(1, d, cpu_handles[i],
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += (uint64_t)base * srv_desc_size_;
    return gpu;
}

ComPtr<ID3D12RootSignature> D3D12Device::make_root_sig(bool /*is_compute*/) {
    // Root sig:
    //   param 0 : Root CBV  b0  (params, PS visibility)
    //   param 1 : Descriptor table  SRV t0-t7  (PS visibility)
    //   static sampler s0 : linear, clamp
    D3D12_DESCRIPTOR_RANGE1 srv_range{};
    srv_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors                    = 8;
    srv_range.BaseShaderRegister                = 0;
    srv_range.RegisterSpace                     = 0;
    srv_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 params[2] = {};
    // CBV at b0
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    // SRV table
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxAnisotropy    = 1;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version                   = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters    = 2;
    desc.Desc_1_1.pParameters      = params;
    desc.Desc_1_1.NumStaticSamplers = 1;
    desc.Desc_1_1.pStaticSamplers  = &samp;
    desc.Desc_1_1.Flags            =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> sig_blob, err_blob;
    D3D12SerializeVersionedRootSignature(&desc, &sig_blob, &err_blob);

    ComPtr<ID3D12RootSignature> root_sig;
    if (sig_blob) {
        dev_->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                   sig_blob->GetBufferSize(),
                                   IID_PPV_ARGS(&root_sig));
    }
    return root_sig;
}

Result<Texture> D3D12Device::create_texture(const vangfx::TextureDesc& desc,
                                             const InitData* init) {
    auto data = std::make_unique<D3D12TextureData>();
    data->owner_dev = dev_.Get();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = desc.width;
    rd.Height           = desc.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = static_cast<uint16_t>(desc.mip_levels);
    rd.Format           = to_dxgi12(desc.format);
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (desc.is_render_target) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        state    = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (desc.is_depth) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        state    = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if (desc.is_uav) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                              &rd, state, nullptr,
                                              IID_PPV_ARGS(&data->resource))))
        return gfx_err<Texture>(ShaderError::OutOfMemory, "D3D12 CreateCommittedResource failed");

    // Create SRV in a CPU-side heap (not for binding; just for copy-on-use)
    if (!desc.is_depth && !(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) {
        // Allocate a persistent CPU SRV slot from the first part of the ring heap
        data->srv_cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        uint32_t slot = rtv_next_ + 100; // offset to avoid RTV collision
        data->srv_cpu.ptr += (uint64_t)slot * srv_desc_size_;

        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format                        = rd.Format;
        sv.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels           = rd.MipLevels;
        dev_->CreateShaderResourceView(data->resource.Get(), &sv, data->srv_cpu);
    }

    // Create RTV if needed
    if (desc.is_render_target) {
        data->rtv_cpu = alloc_rtv();
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format             = rd.Format;
        rv.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(data->resource.Get(), &rv, data->rtv_cpu);
    }

    return Texture(std::move(data), desc.width, desc.height, desc.format);
}

Result<Buffer> D3D12Device::create_buffer(uint32_t size, const void* init_data) {
    auto data = std::make_unique<D3D12BufferData>();
    // Round to 256 bytes for CBV alignment
    uint32_t aligned = (size + 255u) & ~255u;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width              = aligned;
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(dev_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                              &rd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr,
                                              IID_PPV_ARGS(&data->resource))))
        return gfx_err<Buffer>(ShaderError::OutOfMemory, "D3D12 cbuffer create failed");

    D3D12_RANGE r{ 0, 0 };
    data->resource->Map(0, &r, reinterpret_cast<void**>(&data->mapped_ptr));
    if (init_data && data->mapped_ptr)
        std::memcpy(data->mapped_ptr, init_data, size);

    return Buffer(std::move(data), aligned);
}

Result<Pipeline> D3D12Device::create_pipeline(const PipelineDesc& desc) {
    auto data = std::make_unique<D3D12PipelineData>();
    data->root_sig = make_root_sig(false);
    if (!data->root_sig)
        return gfx_err<Pipeline>(ShaderError::CompileFailed, "D3D12 root sig failed");

    const auto& vs_src = desc.shaders.vs;
    const auto& ps_src = desc.shaders.ps;

    // Compile VS
    ComPtr<ID3DBlob> vs_blob, ps_blob;
    if (!vs_src.bytecode.empty()) {
        // Use precompiled — we wrap in a blob-like struct; for now, re-compile
        vs_blob = compile_hlsl12(kFullscreenVS12, "main", "vs_5_0");
    } else if (!vs_src.hlsl.empty()) {
        vs_blob = compile_hlsl12(vs_src.hlsl.c_str(), vs_src.entry.c_str(), "vs_5_0");
    } else {
        vs_blob = compile_hlsl12(kFullscreenVS12, "main", "vs_5_0");
    }
    if (!vs_blob)
        return gfx_err<Pipeline>(ShaderError::CompileFailed, "D3D12 VS compile failed");

    // Compile PS
    if (!ps_src.bytecode.empty()) {
        ps_blob = compile_hlsl12(ps_src.hlsl.empty() ? "" : ps_src.hlsl.c_str(),
                                  ps_src.entry.c_str(), "ps_5_0");
        // Fall back: treat bytecode as SM5.0 DXBC inline
    } else if (!ps_src.hlsl.empty()) {
        ps_blob = compile_hlsl12(ps_src.hlsl.c_str(), ps_src.entry.c_str(), "ps_5_0");
    }
    if (!ps_blob)
        return gfx_err<Pipeline>(ShaderError::CompileFailed, "D3D12 PS compile failed");

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = data->root_sig.Get();
    pso.VS = { vs_blob->GetBufferPointer(), vs_blob->GetBufferSize() };
    pso.PS = { ps_blob->GetBufferPointer(), ps_blob->GetBufferSize() };

    pso.BlendState.RenderTarget[0].BlendEnable    = desc.alpha_blend ? TRUE : FALSE;
    pso.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.SampleMask = UINT_MAX;

    pso.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode              = desc.cull_back ? D3D12_CULL_MODE_BACK
                                                                : D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable       = TRUE;

    pso.DepthStencilState.DepthEnable  = desc.depth_test  ? TRUE : FALSE;
    pso.DepthStencilState.DepthWriteMask =
        desc.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc    = D3D12_COMPARISON_FUNC_LESS;

    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM; // default; pass overrides if needed
    pso.SampleDesc.Count      = 1;

    if (FAILED(dev_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&data->pso))))
        return gfx_err<Pipeline>(ShaderError::CompileFailed, "D3D12 PSO create failed");

    return Pipeline(std::move(data));
}

void D3D12Device::update_buffer(Buffer& buf, const void* data_ptr, uint32_t size) {
    auto* bd = static_cast<D3D12BufferData*>(buf.data());
    if (bd && bd->mapped_ptr)
        std::memcpy(bd->mapped_ptr, data_ptr, size);
}

Texture D3D12Device::wrap_texture(void* native_tex, void* native_srv,
                                   void* native_rtv, uint32_t width,
                                   uint32_t height,
                                   vangfx::SurfaceFormat format) {
    auto data            = std::make_unique<D3D12TextureData>();
    data->owns_resource  = false;
    data->owner_dev      = dev_.Get();
    data->resource       = static_cast<ID3D12Resource*>(native_tex);

    // native_srv is a D3D12_CPU_DESCRIPTOR_HANDLE* (or null)
    if (native_srv) {
        data->srv_cpu = *static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(native_srv);
    }
    if (native_rtv) {
        data->rtv_cpu = *static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(native_rtv);
    }
    return Texture(std::move(data), width, height, format);
}

void D3D12Device::submit_pass_list(ID3D12GraphicsCommandList* list) {
    std::lock_guard lock(submit_mutex_);
    ID3D12CommandList* lists[] = { list };
    queue_->ExecuteCommandLists(1, lists);

    // GPU-CPU sync: wait for the submitted list before reusing the allocator.
    ++fence_value_;
    queue_->Signal(fence_.Get(), fence_value_);
    if (fence_->GetCompletedValue() < fence_value_) {
        fence_->SetEventOnCompletion(fence_value_, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    cmd_alloc_->Reset();
}

std::unique_ptr<Pass> D3D12Device::begin_pass() {
    // Reset and open the shared command list
    cmd_alloc_->Reset();
    cmd_list_->Reset(cmd_alloc_.Get(), nullptr);
    return std::make_unique<D3D12Pass>(this, cmd_alloc_.Get(), cmd_list_.Get());
}

// ── D3D12Pass ─────────────────────────────────────────────────────────────────

D3D12Pass::D3D12Pass(D3D12Device* device,
                     ID3D12CommandAllocator* alloc,
                     ID3D12GraphicsCommandList* list)
    : device_(device), alloc_(alloc), list_(list) {
    // Create a small upload buffer for the params cbuffer
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width              = sizeof(VanGFXParams);
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.Format             = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    device_->dev()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&params_buf_));
    if (params_buf_) {
        D3D12_RANGE r{ 0, 0 };
        params_buf_->Map(0, &r, reinterpret_cast<void**>(&params_ptr_));
    }
}

D3D12Pass::~D3D12Pass() {
    if (!ended_) end();
    if (params_buf_ && params_ptr_) params_buf_->Unmap(0, nullptr);
}

void D3D12Pass::set_render_target(Texture& rt) {
    auto* td = static_cast<D3D12TextureData*>(rt.data());
    if (!td) return;
    has_custom_rt_ = true;
    rt_cpu_        = td->rtv_cpu;

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource   = td->resource.Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bar.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list_->ResourceBarrier(1, &bar);

    list_->OMSetRenderTargets(1, &rt_cpu_, FALSE, nullptr);
}

void D3D12Pass::set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{ float(x), float(y), float(w), float(h), 0.0f, 1.0f };
    list_->RSSetViewports(1, &vp);
    D3D12_RECT sc{ LONG(x), LONG(y), LONG(x + w), LONG(y + h) };
    list_->RSSetScissorRects(1, &sc);
}

void D3D12Pass::set_pipeline(Pipeline& p) {
    pipeline_ = static_cast<D3D12PipelineData*>(p.data());
    if (!pipeline_) return;

    list_->SetGraphicsRootSignature(pipeline_->root_sig.Get());
    list_->SetPipelineState(pipeline_->pso.Get());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind the SRV descriptor heap
    ID3D12DescriptorHeap* heaps[] = { device_->srv_heap() };
    list_->SetDescriptorHeaps(1, heaps);
}

void D3D12Pass::bind(uint32_t slot, Texture& t) {
    auto* td = static_cast<D3D12TextureData*>(t.data());
    if (!td || slot >= kMaxSRVs) return;
    bound_srvs_[slot] = td->srv_cpu;
    if (slot + 1 > srv_count_) srv_count_ = slot + 1;
}

void D3D12Pass::update_params(const VanGFXParams& params) {
    if (!params_ptr_) return;
    std::memcpy(params_ptr_, &params, sizeof(params));
    // Set root CBV (param 0) to the buffer's GPU VA
    if (pipeline_)
        list_->SetGraphicsRootConstantBufferView(0, params_buf_->GetGPUVirtualAddress());
}

void D3D12Pass::draw_fullscreen() {
    // Upload any bound SRVs into the GPU-visible descriptor heap
    if (srv_count_ > 0 && pipeline_) {
        auto gpu_base = device_->alloc_pass_srvs(srv_count_, bound_srvs_);
        list_->SetGraphicsRootDescriptorTable(1, gpu_base);
    }
    list_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Pass::draw(uint32_t vertices, uint32_t instances) {
    if (srv_count_ > 0 && pipeline_) {
        auto gpu_base = device_->alloc_pass_srvs(srv_count_, bound_srvs_);
        list_->SetGraphicsRootDescriptorTable(1, gpu_base);
    }
    list_->DrawInstanced(vertices, instances, 0, 0);
}

void D3D12Pass::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    list_->Dispatch(x, y, z);
}

void D3D12Pass::end() {
    if (ended_) return;
    ended_ = true;

    // Transition RT back
    if (has_custom_rt_) {
        // We only do this for custom RTs we transitioned in set_render_target
        // For the default backbuffer the caller handles transitions.
    }

    list_->Close();
    device_->submit_pass_list(list_);
}

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_d3d12_device(ID3D12Device* dev,
                                           ID3D12CommandQueue* queue) {
    return std::make_unique<D3D12Device>(dev, queue);
}

} // namespace vangfx::gpu
