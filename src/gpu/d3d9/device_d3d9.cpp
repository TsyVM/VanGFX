#include "device_d3d9.hpp"
#include <vangfx/resources/format.hpp>
#include <vangfx/core/error.hpp>
#include <cstring>

namespace vangfx::gpu {

// ── Helpers ───────────────────────────────────────────────────────────────────

static D3DFORMAT to_d3d9(vangfx::SurfaceFormat f) {
    switch (f) {
    case vangfx::SurfaceFormat::RGBA8:         return D3DFMT_A8B8G8R8;
    case vangfx::SurfaceFormat::BGRA8:
    case vangfx::SurfaceFormat::A8R8G8B8:      return D3DFMT_A8R8G8B8;
    case vangfx::SurfaceFormat::RGBA16F:
    case vangfx::SurfaceFormat::A16B16G16R16F: return D3DFMT_A16B16G16R16F;
    case vangfx::SurfaceFormat::RGBA32F:
    case vangfx::SurfaceFormat::A32B32G32R32F: return D3DFMT_A32B32G32R32F;
    case vangfx::SurfaceFormat::R16F:          return D3DFMT_R16F;
    case vangfx::SurfaceFormat::R32F:          return D3DFMT_R32F;
    default:                                   return D3DFMT_A8R8G8B8;
    }
}

// HLSL for the fullscreen quad VS (D3D9 uses XYZRHW + TEXCOORD, so no matrix).
// Vertex format: { x, y, z, w, u, v }.  We pass through as-is.
static constexpr const char* kFullscreenVS9 =
    "struct VS_IN  { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(VS_IN i) { VS_OUT o; o.pos = i.pos; o.uv = i.uv; return o; }\n";

static ComPtr<ID3DBlob> compile_hlsl9(const char* src, const char* entry,
                                      const char* profile) {
    ComPtr<ID3DBlob> code, err;
    D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
               entry, profile, 0, 0, &code, &err);
    return code;
}

// ── D3D9Device ────────────────────────────────────────────────────────────────

D3D9Device::D3D9Device(IDirect3DDevice9* dev) : dev_(dev) {}

bool D3D9Device::ensure_quad_vb() {
    if (quad_vb_) return true;

    // 4-vertex fullscreen quad: two triangles as TRIANGLESTRIP.
    // D3D9 clip space: x in [-1,1], y in [-1,1], z=0, w=1.
    // With XYZRHW the values are pre-transformed screen-space pixels, BUT
    // using POSITION (not POSITIONT) we stay in clip space and use vs_3_0.
    static const D3D9QuadVertex kVerts[4] = {
        { -1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f },
    };

    if (FAILED(dev_->CreateVertexBuffer(sizeof(kVerts), D3DUSAGE_WRITEONLY,
                                         0, D3DPOOL_DEFAULT, &quad_vb_, nullptr)))
        return false;

    void* ptr = nullptr;
    if (SUCCEEDED(quad_vb_->Lock(0, sizeof(kVerts), &ptr, 0))) {
        std::memcpy(ptr, kVerts, sizeof(kVerts));
        quad_vb_->Unlock();
    }
    return true;
}

Result<Texture> D3D9Device::create_texture(const vangfx::TextureDesc& desc,
                                            const InitData* init) {
    auto data = std::make_unique<D3D9TextureData>();
    D3DFORMAT fmt = to_d3d9(desc.format);
    DWORD usage = D3DUSAGE_DYNAMIC;
    D3DPOOL pool = D3DPOOL_DEFAULT;

    if (desc.is_render_target) {
        usage = D3DUSAGE_RENDERTARGET;
    } else if (!desc.is_dynamic) {
        usage = 0;
        pool  = D3DPOOL_MANAGED;
    }

    if (FAILED(dev_->CreateTexture(desc.width, desc.height,
                                    desc.mip_levels, usage, fmt, pool,
                                    &data->tex, nullptr)))
        return gfx_err<Texture>(ShaderError::OutOfMemory, "D3D9 CreateTexture failed");

    if (init && init->data && pool == D3DPOOL_MANAGED) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(data->tex->LockRect(0, &lr, nullptr, 0))) {
            const uint8_t* src = static_cast<const uint8_t*>(init->data);
            uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
            uint32_t pitch = init->row_pitch ? init->row_pitch : desc.width * 4;
            for (uint32_t row = 0; row < desc.height; ++row)
                std::memcpy(dst + row * lr.Pitch, src + row * pitch, pitch);
            data->tex->UnlockRect(0);
        }
    }

    if (desc.is_render_target) {
        data->tex->GetSurfaceLevel(0, &data->rt_surf);
    }

    return Texture(std::move(data), desc.width, desc.height, desc.format);
}

Result<Buffer> D3D9Device::create_buffer(uint32_t size, const void* init) {
    auto data = std::make_unique<D3D9BufferData>();
    uint32_t floats = (size + 3u) / 4u;
    data->cpu_data.resize(floats, 0.0f);
    data->size = size;
    if (init) std::memcpy(data->cpu_data.data(), init, size);
    return Buffer(std::move(data), size);
}

Result<Pipeline> D3D9Device::create_pipeline(const PipelineDesc& desc) {
    auto data = std::make_unique<D3D9PipelineData>();
    data->alpha_blend = desc.alpha_blend;
    data->depth_test  = desc.depth_test;

    // VS: use built-in fullscreen VS if no source provided
    const auto& vs_src = desc.shaders.vs;
    if (vs_src.hlsl.empty() && vs_src.bytecode.empty()) {
        auto blob = compile_hlsl9(kFullscreenVS9, "main", "vs_3_0");
        if (!blob)
            return gfx_err<Pipeline>(ShaderError::CompileFailed, "D3D9 fullscreen VS compile failed");
        dev_->CreateVertexShader(static_cast<const DWORD*>(blob->GetBufferPointer()),
                                  &data->vs);
    } else if (!vs_src.bytecode.empty()) {
        dev_->CreateVertexShader(reinterpret_cast<const DWORD*>(vs_src.bytecode.data()),
                                  &data->vs);
    } else {
        auto blob = compile_hlsl9(vs_src.hlsl.c_str(), vs_src.entry.c_str(), "vs_3_0");
        if (!blob)
            return gfx_err<Pipeline>(ShaderError::CompileFailed, "D3D9 VS compile failed");
        dev_->CreateVertexShader(static_cast<const DWORD*>(blob->GetBufferPointer()),
                                  &data->vs);
    }

    // PS
    const auto& ps_src = desc.shaders.ps;
    if (!ps_src.bytecode.empty()) {
        dev_->CreatePixelShader(reinterpret_cast<const DWORD*>(ps_src.bytecode.data()),
                                 &data->ps);
    } else if (!ps_src.hlsl.empty()) {
        auto blob = compile_hlsl9(ps_src.hlsl.c_str(), ps_src.entry.c_str(), "ps_3_0");
        if (!blob)
            return gfx_err<Pipeline>(ShaderError::CompileFailed, "D3D9 PS compile failed");
        dev_->CreatePixelShader(static_cast<const DWORD*>(blob->GetBufferPointer()),
                                 &data->ps);
    }

    return Pipeline(std::move(data));
}

void D3D9Device::update_buffer(Buffer& buf, const void* data_ptr, uint32_t size) {
    auto* bd = static_cast<D3D9BufferData*>(buf.data());
    if (!bd) return;
    std::memcpy(bd->cpu_data.data(), data_ptr, size > bd->size ? bd->size : size);
}

Texture D3D9Device::wrap_texture(void* native_tex, void* /*native_srv*/,
                                   void* native_rtv, uint32_t width,
                                   uint32_t height,
                                   vangfx::SurfaceFormat format) {
    auto data      = std::make_unique<D3D9TextureData>();
    data->owns_tex = false;
    data->tex      = static_cast<IDirect3DTexture9*>(native_tex);
    data->rt_surf  = static_cast<IDirect3DSurface9*>(native_rtv);
    return Texture(std::move(data), width, height, format);
}

std::unique_ptr<Pass> D3D9Device::begin_pass() {
    ensure_quad_vb();
    return std::make_unique<D3D9Pass>(dev_.Get(), quad_vb_.Get());
}

// ── D3D9Pass ──────────────────────────────────────────────────────────────────

D3D9Pass::D3D9Pass(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* quad_vb)
    : dev_(dev), quad_vb_(quad_vb) {
    // Capture ALL render state into a state block
    dev_->CreateStateBlock(D3DSBT_ALL, &state_block_);
    if (state_block_) state_block_->Capture();
}

D3D9Pass::~D3D9Pass() {
    if (!ended_) end();
}

void D3D9Pass::set_render_target(Texture& rt) {
    auto* surf = static_cast<IDirect3DSurface9*>(rt.rtv());
    dev_->SetRenderTarget(0, surf);
}

void D3D9Pass::set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    D3DVIEWPORT9 vp{ x, y, w, h, 0.0f, 1.0f };
    dev_->SetViewport(&vp);
}

void D3D9Pass::set_pipeline(Pipeline& p) {
    pipeline_ = static_cast<D3D9PipelineData*>(p.data());
    if (!pipeline_) return;

    dev_->SetVertexShader(pipeline_->vs.Get());
    dev_->SetPixelShader(pipeline_->ps.Get());

    // Render states
    dev_->SetRenderState(D3DRS_ALPHABLENDENABLE,
                          pipeline_->alpha_blend ? TRUE : FALSE);
    if (pipeline_->alpha_blend) {
        dev_->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        dev_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    dev_->SetRenderState(D3DRS_ZENABLE,
                          pipeline_->depth_test ? D3DZB_TRUE : D3DZB_FALSE);
    dev_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev_->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);

    // Linear clamp sampler on stage 0
    dev_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
}

void D3D9Pass::bind(uint32_t slot, Texture& t) {
    dev_->SetTexture(slot, static_cast<IDirect3DTexture9*>(t.native()));
}

void D3D9Pass::update_params(const VanGFXParams& params) {
    // Upload float4[0..8] to VS/PS constant registers c0..c8.
    // c0-c7: vangfx_float4[0-7]
    // c8.xy: vangfx_time, vangfx_delta
    // c9.xy: vangfx_resolution
    dev_->SetPixelShaderConstantF(0, &params.float4[0][0], 8);
    float timeVec[4] = { params.time, params.delta, 0.0f, 0.0f };
    dev_->SetPixelShaderConstantF(8, timeVec, 1);
    float resVec[4] = { params.resolution[0], params.resolution[1], 0.0f, 0.0f };
    dev_->SetPixelShaderConstantF(9, resVec, 1);
}

void D3D9Pass::draw_fullscreen() {
    if (!quad_vb_) return;
    // FVF: POSITION | TEX1 (compatible with our VS that expects POSITION)
    dev_->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    dev_->SetStreamSource(0, quad_vb_, 0, sizeof(D3D9QuadVertex));
    dev_->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
}

void D3D9Pass::draw(uint32_t vertices, uint32_t /*instances*/) {
    dev_->DrawPrimitive(D3DPT_TRIANGLELIST, 0, vertices / 3);
}

void D3D9Pass::dispatch(uint32_t, uint32_t, uint32_t) {
    // D3D9 has no compute
}

void D3D9Pass::end() {
    if (ended_) return;
    ended_ = true;
    if (state_block_) state_block_->Apply();
}

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_d3d9_device(IDirect3DDevice9* dev) {
    return std::make_unique<D3D9Device>(dev);
}

} // namespace vangfx::gpu
