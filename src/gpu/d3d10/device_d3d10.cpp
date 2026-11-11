#include "device_d3d10.hpp"
#include <vangfx/resources/format.hpp>
#include <vangfx/core/error.hpp>
#include <algorithm>
#include <cstring>

namespace vangfx::gpu {

// ── Helpers ───────────────────────────────────────────────────────────────────

static DXGI_FORMAT to_dxgi(vangfx::SurfaceFormat f) {
    switch (f) {
    case vangfx::SurfaceFormat::RGBA8:        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case vangfx::SurfaceFormat::BGRA8:        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case vangfx::SurfaceFormat::RGBA16F:      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case vangfx::SurfaceFormat::RGBA32F:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case vangfx::SurfaceFormat::R11G11B10F:   return DXGI_FORMAT_R11G11B10_FLOAT;
    case vangfx::SurfaceFormat::R10G10B10A2:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case vangfx::SurfaceFormat::R8:           return DXGI_FORMAT_R8_UNORM;
    case vangfx::SurfaceFormat::RG8:          return DXGI_FORMAT_R8G8_UNORM;
    case vangfx::SurfaceFormat::R16F:         return DXGI_FORMAT_R16_FLOAT;
    case vangfx::SurfaceFormat::R32F:         return DXGI_FORMAT_R32_FLOAT;
    case vangfx::SurfaceFormat::RG16F:        return DXGI_FORMAT_R16G16_FLOAT;
    case vangfx::SurfaceFormat::RG32F:        return DXGI_FORMAT_R32G32_FLOAT;
    default:                                  return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static constexpr const char* kFullscreenVS =
    "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
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
    return code;
}

// ── D3D10Device ───────────────────────────────────────────────────────────────

D3D10Device::D3D10Device(ID3D10Device* dev) : dev_(dev) {}

ID3D10VertexShader* D3D10Device::fullscreen_vs() {
    if (fullscreen_vs_) return fullscreen_vs_.Get();
    auto blob = compile_hlsl(kFullscreenVS, "main", "vs_4_0");
    if (!blob) return nullptr;
    dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                              &fullscreen_vs_);
    return fullscreen_vs_.Get();
}

Result<Texture> D3D10Device::create_texture(const vangfx::TextureDesc& desc,
                                              const InitData* init) {
    auto data = std::make_unique<D3D10TextureData>();

    D3D10_TEXTURE2D_DESC d{};
    d.Width             = desc.width;
    d.Height            = desc.height;
    d.MipLevels         = desc.mip_levels ? desc.mip_levels : 1;
    d.ArraySize         = 1;
    d.Format            = to_dxgi(desc.format);
    d.SampleDesc.Count  = 1;
    d.Usage             = D3D10_USAGE_DEFAULT;
    d.BindFlags         = D3D10_BIND_SHADER_RESOURCE;

    if (desc.is_render_target) d.BindFlags |= D3D10_BIND_RENDER_TARGET;
    if (desc.is_depth)         d.BindFlags  = D3D10_BIND_DEPTH_STENCIL | D3D10_BIND_SHADER_RESOURCE;
    if (desc.is_dynamic) {
        d.Usage          = D3D10_USAGE_DYNAMIC;
        d.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    }

    D3D10_SUBRESOURCE_DATA sd{};
    if (init && init->data) {
        sd.pSysMem          = init->data;
        sd.SysMemPitch      = init->row_pitch ? init->row_pitch : desc.width * 4;
        sd.SysMemSlicePitch = init->slice_pitch;
    }

    if (FAILED(dev_->CreateTexture2D(&d, (init && init->data) ? &sd : nullptr,
                                      &data->tex)))
        return gfx_err<Texture>(ShaderError::OutOfMemory,
                                "ID3D10Device::CreateTexture2D failed");

    if (!desc.is_depth) {
        D3D10_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format                    = d.Format;
        sv.ViewDimension             = D3D10_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels       = d.MipLevels;
        sv.Texture2D.MostDetailedMip = 0;
        dev_->CreateShaderResourceView(data->tex.Get(), &sv, &data->srv_view_);
    }
    if (desc.is_render_target) {
        D3D10_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format             = d.Format;
        rv.ViewDimension      = D3D10_RTV_DIMENSION_TEXTURE2D;
        rv.Texture2D.MipSlice = 0;
        dev_->CreateRenderTargetView(data->tex.Get(), &rv, &data->rtv_view_);
    }

    return Texture(std::move(data), desc.width, desc.height, desc.format);
}

Result<Buffer> D3D10Device::create_buffer(uint32_t size, const void* init) {
    auto data    = std::make_unique<D3D10BufferData>();
    uint32_t aligned = (size + 15u) & ~15u;

    D3D10_BUFFER_DESC bd{};
    bd.ByteWidth      = aligned;
    bd.Usage          = D3D10_USAGE_DYNAMIC;
    bd.BindFlags      = D3D10_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

    D3D10_SUBRESOURCE_DATA sd{};
    if (init) sd.pSysMem = init;

    if (FAILED(dev_->CreateBuffer(&bd, init ? &sd : nullptr, &data->buf)))
        return gfx_err<Buffer>(ShaderError::OutOfMemory,
                               "ID3D10Device::CreateBuffer failed");

    return Buffer(std::move(data), aligned);
}

Result<Pipeline> D3D10Device::create_pipeline(const PipelineDesc& desc) {
    auto data = std::make_unique<D3D10PipelineData>();

    const auto& vs_src = desc.shaders.vs;
    const auto& ps_src = desc.shaders.ps;

    // VS
    if (vs_src.hlsl.empty() && vs_src.bytecode.empty()) {
        auto* fsVS = fullscreen_vs();
        if (!fsVS)
            return gfx_err<Pipeline>(ShaderError::CompileFailed,
                                     "fullscreen VS compile failed");
        data->vs = fsVS;
    } else if (!vs_src.bytecode.empty()) {
        dev_->CreateVertexShader(vs_src.bytecode.data(), vs_src.bytecode.size(),
                                  &data->vs);
    } else {
        auto blob = compile_hlsl(vs_src.hlsl.c_str(),
                                  vs_src.entry.empty() ? "main" : vs_src.entry.c_str(),
                                  "vs_4_0");
        if (!blob)
            return gfx_err<Pipeline>(ShaderError::CompileFailed, "VS compile failed");
        dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                  &data->vs);
    }

    // PS
    if (!ps_src.bytecode.empty()) {
        dev_->CreatePixelShader(ps_src.bytecode.data(), ps_src.bytecode.size(),
                                 &data->ps);
    } else if (!ps_src.hlsl.empty()) {
        auto blob = compile_hlsl(ps_src.hlsl.c_str(),
                                  ps_src.entry.empty() ? "main" : ps_src.entry.c_str(),
                                  "ps_4_0");
        if (!blob)
            return gfx_err<Pipeline>(ShaderError::CompileFailed, "PS compile failed");
        dev_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                 &data->ps);
    }

    // Blend
    D3D10_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable  = FALSE;
    for (int i = 0; i < 8; ++i) {
        bd.BlendEnable[i]           = desc.alpha_blend ? TRUE : FALSE;
        bd.RenderTargetWriteMask[i] = D3D10_COLOR_WRITE_ENABLE_ALL;
    }
    bd.SrcBlend      = D3D10_BLEND_SRC_ALPHA;
    bd.DestBlend     = D3D10_BLEND_INV_SRC_ALPHA;
    bd.BlendOp       = D3D10_BLEND_OP_ADD;
    bd.SrcBlendAlpha = D3D10_BLEND_ONE;
    bd.DestBlendAlpha= D3D10_BLEND_ZERO;
    bd.BlendOpAlpha  = D3D10_BLEND_OP_ADD;
    dev_->CreateBlendState(&bd, &data->blend);

    // Depth
    D3D10_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable    = desc.depth_test  ? TRUE : FALSE;
    ds.DepthWriteMask = desc.depth_write ? D3D10_DEPTH_WRITE_MASK_ALL
                                         : D3D10_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc      = D3D10_COMPARISON_LESS;
    dev_->CreateDepthStencilState(&ds, &data->depth);

    // Rasterizer
    D3D10_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D10_FILL_SOLID;
    rd.CullMode        = desc.cull_back ? D3D10_CULL_BACK : D3D10_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev_->CreateRasterizerState(&rd, &data->raster);

    // Sampler
    D3D10_SAMPLER_DESC sd2{};
    sd2.Filter         = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sd2.AddressU       = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd2.AddressV       = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd2.AddressW       = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd2.MaxAnisotropy  = 1;
    sd2.ComparisonFunc = D3D10_COMPARISON_NEVER;
    sd2.MaxLOD         = D3D10_FLOAT32_MAX;
    dev_->CreateSamplerState(&sd2, &data->samp);

    return Pipeline(std::move(data));
}

void D3D10Device::update_buffer(Buffer& buf, const void* data_ptr, uint32_t size) {
    auto* bd = static_cast<D3D10BufferData*>(buf.data());
    if (!bd) return;
    void* mapped = nullptr;
    if (SUCCEEDED(bd->buf->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped, data_ptr, size);
        bd->buf->Unmap();
    }
}

Texture D3D10Device::wrap_texture(void* native_tex, void* native_srv,
                                    void* native_rtv, uint32_t width,
                                    uint32_t height,
                                    vangfx::SurfaceFormat format) {
    auto data       = std::make_unique<D3D10TextureData>();
    data->owns_tex  = false;
    data->tex       = static_cast<ID3D10Texture2D*>(native_tex);
    data->srv_view_ = static_cast<ID3D10ShaderResourceView*>(native_srv);
    data->rtv_view_ = static_cast<ID3D10RenderTargetView*>(native_rtv);
    return Texture(std::move(data), width, height, format);
}

std::unique_ptr<Pass> D3D10Device::begin_pass() {
    return std::make_unique<D3D10Pass>(dev_.Get());
}

// ── D3D10Pass ─────────────────────────────────────────────────────────────────

D3D10Pass::D3D10Pass(ID3D10Device* dev) : dev_(dev) {
    save_state();

    D3D10_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(VanGFXParams);
    bd.Usage          = D3D10_USAGE_DYNAMIC;
    bd.BindFlags      = D3D10_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
    dev_->CreateBuffer(&bd, nullptr, &params_cb_);
}

D3D10Pass::~D3D10Pass() {
    if (!ended_) end();
}

void D3D10Pass::save_state() {
    {
        ID3D10RenderTargetView* rtvs[kMaxRTs]{};
        ID3D10DepthStencilView* dsv = nullptr;
        dev_->OMGetRenderTargets(kMaxRTs, rtvs, &dsv);
        for (int i = 0; i < kMaxRTs; ++i) saved_.rtvs[i].Attach(rtvs[i]);
        saved_.dsv.Attach(dsv);
    }
    for (saved_.rtv_count = 0; saved_.rtv_count < (uint32_t)kMaxRTs; ++saved_.rtv_count)
        if (!saved_.rtvs[saved_.rtv_count]) break;

    dev_->IAGetPrimitiveTopology(&saved_.topology);
    dev_->IAGetInputLayout(&saved_.input_layout);
    dev_->VSGetShader(&saved_.vs);
    dev_->PSGetShader(&saved_.ps);
    dev_->GSGetShader(&saved_.gs);
    dev_->PSGetConstantBuffers(0, 1, &saved_.ps_cb0);
    {
        ID3D10ShaderResourceView* srvs[kMaxSRVs]{};
        dev_->PSGetShaderResources(0, kMaxSRVs, srvs);
        for (int i = 0; i < kMaxSRVs; ++i) saved_.ps_srvs[i].Attach(srvs[i]);
    }
    dev_->PSGetSamplers(0, 1, &saved_.ps_samp0);

    FLOAT bf[4]; UINT sm;
    dev_->OMGetBlendState(&saved_.blend, bf, &sm);
    std::copy(bf, bf + 4, saved_.blend_factor);
    saved_.blend_mask = sm;

    dev_->OMGetDepthStencilState(&saved_.depth, &saved_.stencil_ref);
    dev_->RSGetState(&saved_.raster);
    saved_.viewport_count = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    dev_->RSGetViewports(&saved_.viewport_count, saved_.viewports);
}

void D3D10Pass::restore_state() {
    ID3D10RenderTargetView* rtvs[kMaxRTs];
    for (int i = 0; i < kMaxRTs; ++i) rtvs[i] = saved_.rtvs[i].Get();
    dev_->OMSetRenderTargets(saved_.rtv_count, rtvs, saved_.dsv.Get());

    dev_->IASetPrimitiveTopology(saved_.topology);
    dev_->IASetInputLayout(saved_.input_layout.Get());
    dev_->VSSetShader(saved_.vs.Get());
    dev_->PSSetShader(saved_.ps.Get());
    dev_->GSSetShader(saved_.gs.Get());
    dev_->PSSetConstantBuffers(0, 1, &saved_.ps_cb0);

    ID3D10ShaderResourceView* srvs[kMaxSRVs];
    for (int i = 0; i < kMaxSRVs; ++i) srvs[i] = saved_.ps_srvs[i].Get();
    dev_->PSSetShaderResources(0, kMaxSRVs, srvs);

    ID3D10SamplerState* samp = saved_.ps_samp0.Get();
    dev_->PSSetSamplers(0, 1, &samp);
    dev_->OMSetBlendState(saved_.blend.Get(), saved_.blend_factor, saved_.blend_mask);
    dev_->OMSetDepthStencilState(saved_.depth.Get(), saved_.stencil_ref);
    dev_->RSSetState(saved_.raster.Get());
    if (saved_.viewport_count)
        dev_->RSSetViewports(saved_.viewport_count, saved_.viewports);
}

void D3D10Pass::set_render_target(Texture& rt) {
    auto* td = static_cast<D3D10TextureData*>(rt.data());
    if (!td) return;
    custom_rtv_ = td->rtv_view_;
    ID3D10RenderTargetView* rtv = custom_rtv_.Get();
    dev_->OMSetRenderTargets(1, &rtv, nullptr);
}

void D3D10Pass::set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    D3D10_VIEWPORT vp{ (INT)x, (INT)y, (UINT)w, (UINT)h, 0.0f, 1.0f };
    dev_->RSSetViewports(1, &vp);
}

void D3D10Pass::set_pipeline(Pipeline& p) {
    pipeline_ = static_cast<D3D10PipelineData*>(p.data());
    if (!pipeline_) return;

    dev_->VSSetShader(pipeline_->vs.Get());
    dev_->PSSetShader(pipeline_->ps.Get());
    dev_->GSSetShader(nullptr);
    dev_->IASetInputLayout(nullptr);
    dev_->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->OMSetBlendState(pipeline_->blend.Get(), nullptr, 0xFFFFFFFF);
    dev_->OMSetDepthStencilState(pipeline_->depth.Get(), 0);
    dev_->RSSetState(pipeline_->raster.Get());
    ID3D10SamplerState* samp = pipeline_->samp.Get();
    dev_->PSSetSamplers(0, 1, &samp);
}

void D3D10Pass::bind(uint32_t slot, Texture& t) {
    auto* td = static_cast<D3D10TextureData*>(t.data());
    if (!td) return;
    ID3D10ShaderResourceView* srv = td->srv_view_.Get();
    dev_->PSSetShaderResources(slot, 1, &srv);
}

void D3D10Pass::update_params(const VanGFXParams& params) {
    if (!params_cb_) return;
    void* mapped = nullptr;
    if (SUCCEEDED(params_cb_->Map(D3D10_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped, &params, sizeof(VanGFXParams));
        params_cb_->Unmap();
    }
    ID3D10Buffer* cb = params_cb_.Get();
    dev_->PSSetConstantBuffers(0, 1, &cb);
    dev_->VSSetConstantBuffers(0, 1, &cb);
}

void D3D10Pass::draw_fullscreen() {
    dev_->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    dev_->IASetInputLayout(nullptr);
    ID3D10Buffer* nullBuf = nullptr;
    UINT stride = 0, offset = 0;
    dev_->IASetVertexBuffers(0, 1, &nullBuf, &stride, &offset);
    dev_->Draw(3, 0);
}

void D3D10Pass::draw(uint32_t vertices, uint32_t instances) {
    dev_->DrawInstanced(vertices, instances, 0, 0);
}

void D3D10Pass::dispatch(uint32_t, uint32_t, uint32_t) {
    // D3D10 has no compute shaders
}

void D3D10Pass::end() {
    if (ended_) return;
    ended_ = true;
    restore_state();
}

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_d3d10_device(ID3D10Device* dev) {
    return std::make_unique<D3D10Device>(dev);
}

} // namespace vangfx::gpu
