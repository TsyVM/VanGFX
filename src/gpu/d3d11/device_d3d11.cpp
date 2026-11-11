#include "device_d3d11.hpp"
#include <vangfx/resources/format.hpp>
#include <vangfx/core/error.hpp>
#include <cstring>
#include <string>

namespace vangfx::gpu {

// ── Helpers ───────────────────────────────────────────────────────────────────

static DXGI_FORMAT to_dxgi(vangfx::SurfaceFormat f) {
    switch (f) {
    case vangfx::SurfaceFormat::RGBA8:         return DXGI_FORMAT_R8G8B8A8_UNORM;
    case vangfx::SurfaceFormat::BGRA8:         return DXGI_FORMAT_B8G8R8A8_UNORM;
    case vangfx::SurfaceFormat::RGBA16F:       return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vangfx::SurfaceFormat::RGBA32F:       return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case vangfx::SurfaceFormat::R11G11B10F:    return DXGI_FORMAT_R11G11B10_FLOAT;
    case vangfx::SurfaceFormat::R10G10B10A2:   return DXGI_FORMAT_R10G10B10A2_UNORM;
    case vangfx::SurfaceFormat::R8:            return DXGI_FORMAT_R8_UNORM;
    case vangfx::SurfaceFormat::RG8:           return DXGI_FORMAT_R8G8_UNORM;
    case vangfx::SurfaceFormat::R16F:          return DXGI_FORMAT_R16_FLOAT;
    case vangfx::SurfaceFormat::R32F:          return DXGI_FORMAT_R32_FLOAT;
    case vangfx::SurfaceFormat::RG16F:         return DXGI_FORMAT_R16G16_FLOAT;
    case vangfx::SurfaceFormat::RG32F:         return DXGI_FORMAT_R32G32_FLOAT;
    case vangfx::SurfaceFormat::A8R8G8B8:      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case vangfx::SurfaceFormat::A16B16G16R16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vangfx::SurfaceFormat::A32B32G32R32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                                   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static vangfx::SurfaceFormat from_dxgi(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return vangfx::SurfaceFormat::RGBA8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return vangfx::SurfaceFormat::BGRA8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return vangfx::SurfaceFormat::RGBA16F;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return vangfx::SurfaceFormat::RGBA32F;
    case DXGI_FORMAT_R11G11B10_FLOAT:       return vangfx::SurfaceFormat::R11G11B10F;
    default:                                return vangfx::SurfaceFormat::RGBA8;
    }
}

// HLSL for the shared fullscreen triangle VS (vertex-ID pull, no VB).
static constexpr const char* kFullscreenVS =
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    float2 p  = float2((id << 1) & 2, id & 2);\n"
    "    VS_OUT o;\n"
    "    o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
    "    o.uv  = float2(p.x, 1.0 - p.y);\n"
    "    return o;\n"
    "}\n";

static ComPtr<ID3DBlob> compile_hlsl(const char* src, const char* entry,
                                     const char* profile) {
    ComPtr<ID3DBlob> code, err;
    D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
               entry, profile, 0, 0, &code, &err);
    return code; // null on failure; caller checks
}

// ── D3D11Device ──────────────────────────────────────────────────────────────

D3D11Device::D3D11Device(ID3D11Device* dev, ID3D11DeviceContext* ctx)
    : dev_(dev), ctx_(ctx) {}

ID3D11VertexShader* D3D11Device::fullscreen_vs() {
    if (fullscreen_vs_) return fullscreen_vs_.Get();
    auto blob = compile_hlsl(kFullscreenVS, "main", "vs_5_0");
    if (!blob) return nullptr;
    dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                              nullptr, &fullscreen_vs_);
    return fullscreen_vs_.Get();
}

Result<Texture> D3D11Device::create_texture(const vangfx::TextureDesc& desc,
                                             const InitData* init) {
    auto data = std::make_unique<D3D11TextureData>();

    D3D11_TEXTURE2D_DESC d{};
    d.Width     = desc.width;
    d.Height    = desc.height;
    d.MipLevels = desc.mip_levels;
    d.ArraySize = 1;
    d.Format    = to_dxgi(desc.format);
    d.SampleDesc.Count = 1;
    d.Usage     = D3D11_USAGE_DEFAULT;

    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (desc.is_render_target) d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (desc.is_depth)         d.BindFlags  = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (desc.is_uav)           d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    if (desc.is_dynamic) {
        d.Usage          = D3D11_USAGE_DYNAMIC;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }

    D3D11_SUBRESOURCE_DATA sd{};
    if (init && init->data) {
        sd.pSysMem          = init->data;
        sd.SysMemPitch      = init->row_pitch ? init->row_pitch
                                              : desc.width * 4; // assume RGBA8
        sd.SysMemSlicePitch = init->slice_pitch;
    }

    if (FAILED(dev_->CreateTexture2D(&d, (init && init->data) ? &sd : nullptr,
                                      &data->tex)))
        return gfx_err<Texture>(ShaderError::OutOfMemory, "ID3D11Device::CreateTexture2D failed");

    // SRV
    if (!desc.is_depth) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format               = d.Format;
        sv.ViewDimension        = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels  = d.MipLevels;
        dev_->CreateShaderResourceView(data->tex.Get(), &sv, &data->srv_view_);
    }

    // RTV
    if (desc.is_render_target) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format             = d.Format;
        rv.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(data->tex.Get(), &rv, &data->rtv_view_);
    }

    return Texture(std::move(data), desc.width, desc.height, desc.format);
}

Result<Buffer> D3D11Device::create_buffer(uint32_t size, const void* init) {
    auto data = std::make_unique<D3D11BufferData>();
    // Round up to 16-byte boundary (D3D11 cbuffer requirement)
    uint32_t aligned = (size + 15u) & ~15u;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth           = aligned;
    bd.Usage               = D3D11_USAGE_DYNAMIC;
    bd.BindFlags           = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;

    D3D11_SUBRESOURCE_DATA sd{};
    if (init) { sd.pSysMem = init; }

    if (FAILED(dev_->CreateBuffer(&bd, init ? &sd : nullptr, &data->buf)))
        return gfx_err<Buffer>(ShaderError::OutOfMemory, "ID3D11Device::CreateBuffer failed");

    return Buffer(std::move(data), aligned);
}

Result<Pipeline> D3D11Device::create_pipeline(const PipelineDesc& desc) {
    auto data = std::make_unique<D3D11PipelineData>();

    if (!desc.shaders.cs.hlsl.empty() || !desc.shaders.cs.bytecode.empty()) {
        // Compute pipeline
        data->is_compute = true;
        const auto& cs = desc.shaders.cs;
        ComPtr<ID3DBlob> blob;
        if (!cs.bytecode.empty()) {
            // Use pre-compiled bytecode
            if (FAILED(dev_->CreateComputeShader(cs.bytecode.data(),
                                                  cs.bytecode.size(), nullptr,
                                                  &data->cs)))
                return gfx_err<Pipeline>(ShaderError::CompileFailed, "CS CreateComputeShader failed");
        } else {
            blob = compile_hlsl(cs.hlsl.c_str(), cs.entry.c_str(), "cs_5_0");
            if (!blob) return gfx_err<Pipeline>(ShaderError::CompileFailed, "CS compile failed");
            dev_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                       nullptr, &data->cs);
        }
    } else {
        // Graphics pipeline
        const auto& vs_src = desc.shaders.vs;
        const auto& ps_src = desc.shaders.ps;

        // VS: use built-in fullscreen VS if no source provided
        if (vs_src.hlsl.empty() && vs_src.bytecode.empty()) {
            auto* fsVS = fullscreen_vs();
            if (!fsVS)
                return gfx_err<Pipeline>(ShaderError::CompileFailed, "fullscreen VS compile failed");
            data->vs = fsVS;
        } else if (!vs_src.bytecode.empty()) {
            dev_->CreateVertexShader(vs_src.bytecode.data(), vs_src.bytecode.size(),
                                      nullptr, &data->vs);
        } else {
            auto blob = compile_hlsl(vs_src.hlsl.c_str(), vs_src.entry.c_str(), "vs_5_0");
            if (!blob) return gfx_err<Pipeline>(ShaderError::CompileFailed, "VS compile failed");
            dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                      nullptr, &data->vs);
        }

        // PS
        if (!ps_src.bytecode.empty()) {
            dev_->CreatePixelShader(ps_src.bytecode.data(), ps_src.bytecode.size(),
                                     nullptr, &data->ps);
        } else if (!ps_src.hlsl.empty()) {
            auto blob = compile_hlsl(ps_src.hlsl.c_str(), ps_src.entry.c_str(), "ps_5_0");
            if (!blob) return gfx_err<Pipeline>(ShaderError::CompileFailed, "PS compile failed");
            dev_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                     nullptr, &data->ps);
        }

        // Blend state
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable    = desc.alpha_blend ? TRUE : FALSE;
        bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev_->CreateBlendState(&bd, &data->blend);

        // Depth stencil (always off for effect passes)
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable  = desc.depth_test  ? TRUE : FALSE;
        ds.DepthWriteMask = desc.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                              : D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc    = D3D11_COMPARISON_LESS;
        dev_->CreateDepthStencilState(&ds, &data->depth);

        // Rasterizer
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode              = D3D11_FILL_SOLID;
        rd.CullMode              = desc.cull_back ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        rd.DepthClipEnable       = TRUE;
        dev_->CreateRasterizerState(&rd, &data->raster);

        // Linear clamp sampler
        D3D11_SAMPLER_DESC sd{};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy  = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&sd, &data->samp);
    }

    return Pipeline(std::move(data));
}

void D3D11Device::update_buffer(Buffer& buf, const void* data_ptr, uint32_t size) {
    auto* bd = static_cast<D3D11BufferData*>(buf.data());
    if (!bd) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(bd->buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, data_ptr, size);
        ctx_->Unmap(bd->buf.Get(), 0);
    }
}

Texture D3D11Device::wrap_texture(void* native_tex, void* native_srv,
                                   void* native_rtv, uint32_t width,
                                   uint32_t height,
                                   vangfx::SurfaceFormat format) {
    auto data        = std::make_unique<D3D11TextureData>();
    data->owns_tex   = false;
    data->tex        = static_cast<ID3D11Texture2D*>(native_tex);
    data->srv_view_  = static_cast<ID3D11ShaderResourceView*>(native_srv);
    data->rtv_view_  = static_cast<ID3D11RenderTargetView*>(native_rtv);
    return Texture(std::move(data), width, height, format);
}

std::unique_ptr<Pass> D3D11Device::begin_pass() {
    return std::make_unique<D3D11Pass>(dev_.Get(), ctx_.Get());
}

// ── D3D11Pass ─────────────────────────────────────────────────────────────────

D3D11Pass::D3D11Pass(ID3D11Device* dev, ID3D11DeviceContext* ctx)
    : dev_(dev), ctx_(ctx) {
    save_state();
    // Create the params cbuffer
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(VanGFXParams);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev_->CreateBuffer(&bd, nullptr, &params_cb_);
}

D3D11Pass::~D3D11Pass() {
    if (!ended_) end();
}

void D3D11Pass::save_state() {
    // RTVs
    ctx_->OMGetRenderTargets(SavedState::kMaxRTs,
        reinterpret_cast<ID3D11RenderTargetView**>(saved_.rtvs),
        saved_.dsv.GetAddressOf());
    for (int i = 0; i < SavedState::kMaxRTs; ++i)
        if (saved_.rtvs[i]) saved_.rtv_count = static_cast<uint32_t>(i + 1);

    // Input assembly
    ctx_->IAGetPrimitiveTopology(&saved_.topology);
    ctx_->IAGetInputLayout(saved_.input_layout.GetAddressOf());

    // Shaders
    ctx_->VSGetShader(saved_.vs.GetAddressOf(), nullptr, nullptr);
    ctx_->PSGetShader(saved_.ps.GetAddressOf(), nullptr, nullptr);

    // PS resources
    ID3D11Buffer* cbs[1] = {};
    ctx_->PSGetConstantBuffers(0, 1, cbs);
    saved_.ps_cb0.Attach(cbs[0]);

    ID3D11ShaderResourceView* srvs[SavedState::kMaxSRVs] = {};
    ctx_->PSGetShaderResources(0, SavedState::kMaxSRVs, srvs);
    for (int i = 0; i < SavedState::kMaxSRVs; ++i)
        saved_.ps_srvs[i].Attach(srvs[i]);

    ID3D11SamplerState* samps[1] = {};
    ctx_->PSGetSamplers(0, 1, samps);
    saved_.ps_samp0.Attach(samps[0]);

    // OM state
    FLOAT bf[4]{};
    ctx_->OMGetBlendState(saved_.blend.GetAddressOf(), bf, &saved_.blend_mask);
    std::memcpy(saved_.blend_factor, bf, sizeof(bf));
    ctx_->OMGetDepthStencilState(saved_.depth.GetAddressOf(), &saved_.stencil_ref);

    // RS state
    ctx_->RSGetState(saved_.raster.GetAddressOf());
    saved_.viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx_->RSGetViewports(&saved_.viewport_count, saved_.viewports);
    saved_.scissor_count  = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx_->RSGetScissorRects(&saved_.scissor_count, saved_.scissors);
}

void D3D11Pass::restore_state() {
    // RTVs
    ID3D11RenderTargetView* raw_rtvs[SavedState::kMaxRTs] = {};
    for (uint32_t i = 0; i < saved_.rtv_count; ++i)
        raw_rtvs[i] = saved_.rtvs[i].Get();
    ctx_->OMSetRenderTargets(saved_.rtv_count, raw_rtvs, saved_.dsv.Get());

    ctx_->IASetPrimitiveTopology(saved_.topology);
    ctx_->IASetInputLayout(saved_.input_layout.Get());

    ctx_->VSSetShader(saved_.vs.Get(), nullptr, 0);
    ctx_->PSSetShader(saved_.ps.Get(), nullptr, 0);

    ID3D11Buffer* cbs[1] = { saved_.ps_cb0.Get() };
    ctx_->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[SavedState::kMaxSRVs] = {};
    for (int i = 0; i < SavedState::kMaxSRVs; ++i)
        srvs[i] = saved_.ps_srvs[i].Get();
    ctx_->PSSetShaderResources(0, SavedState::kMaxSRVs, srvs);

    ID3D11SamplerState* samps[1] = { saved_.ps_samp0.Get() };
    ctx_->PSSetSamplers(0, 1, samps);

    ctx_->OMSetBlendState(saved_.blend.Get(), saved_.blend_factor,
                           saved_.blend_mask);
    ctx_->OMSetDepthStencilState(saved_.depth.Get(), saved_.stencil_ref);
    ctx_->RSSetState(saved_.raster.Get());
    ctx_->RSSetViewports(saved_.viewport_count, saved_.viewports);
    ctx_->RSSetScissorRects(saved_.scissor_count, saved_.scissors);
}

void D3D11Pass::set_render_target(Texture& rt) {
    custom_rtv_ = static_cast<ID3D11RenderTargetView*>(rt.rtv());
    // Bind immediately so subsequent draws go here
    ID3D11RenderTargetView* rtvs[1] = { custom_rtv_.Get() };
    ctx_->OMSetRenderTargets(1, rtvs, nullptr);
}

void D3D11Pass::set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    D3D11_VIEWPORT vp{ float(x), float(y), float(w), float(h), 0.0f, 1.0f };
    ctx_->RSSetViewports(1, &vp);
}

void D3D11Pass::set_pipeline(Pipeline& p) {
    pipeline_ = static_cast<D3D11PipelineData*>(p.data());
    if (!pipeline_) return;
    pipeline_set_ = true;

    if (pipeline_->is_compute) {
        ctx_->CSSetShader(pipeline_->cs.Get(), nullptr, 0);
        return;
    }

    ctx_->VSSetShader(pipeline_->vs.Get(), nullptr, 0);
    ctx_->PSSetShader(pipeline_->ps.Get(), nullptr, 0);
    ctx_->IASetInputLayout(nullptr);     // fullscreen triangle needs no layout
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (pipeline_->blend)  ctx_->OMSetBlendState(pipeline_->blend.Get(), nullptr, 0xFFFFFFFF);
    if (pipeline_->depth)  ctx_->OMSetDepthStencilState(pipeline_->depth.Get(), 0);
    if (pipeline_->raster) ctx_->RSSetState(pipeline_->raster.Get());

    if (pipeline_->samp) {
        ID3D11SamplerState* s = pipeline_->samp.Get();
        ctx_->PSSetSamplers(0, 1, &s);
    }
}

void D3D11Pass::bind(uint32_t slot, Texture& t) {
    auto* srv = static_cast<ID3D11ShaderResourceView*>(t.srv());
    if (!srv) return;
    ctx_->PSSetShaderResources(slot, 1, &srv);
}

void D3D11Pass::update_params(const VanGFXParams& params) {
    if (!params_cb_) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(params_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &params, sizeof(params));
        ctx_->Unmap(params_cb_.Get(), 0);
    }
    ID3D11Buffer* cbs[1] = { params_cb_.Get() };
    ctx_->PSSetConstantBuffers(0, 1, cbs);
}

void D3D11Pass::draw_fullscreen() {
    ctx_->Draw(3, 0); // fullscreen triangle via SV_VertexID
}

void D3D11Pass::draw(uint32_t vertices, uint32_t instances) {
    if (instances > 1) ctx_->DrawInstanced(vertices, instances, 0, 0);
    else               ctx_->Draw(vertices, 0);
}

void D3D11Pass::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    ctx_->Dispatch(x, y, z);
}

void D3D11Pass::end() {
    if (ended_) return;
    ended_ = true;
    restore_state();
}

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_d3d11_device(ID3D11Device* dev,
                                           ID3D11DeviceContext* ctx) {
    return std::make_unique<D3D11Device>(dev, ctx);
}

} // namespace vangfx::gpu
