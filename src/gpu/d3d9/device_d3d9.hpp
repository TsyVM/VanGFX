#pragma once

#include <vangfx/gpu/device.hpp>
#include <vangfx/gpu/pass.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace vangfx::gpu {

using Microsoft::WRL::ComPtr;

// ── D3D9 Texture data ────────────────────────────────────────────────────────

struct D3D9TextureData final : TextureData {
    ComPtr<IDirect3DTexture9> tex;
    ComPtr<IDirect3DSurface9> rt_surf;   // GetSurfaceLevel(0) for RT
    bool  owns_tex = true;

    void* native() const noexcept override { return tex.Get(); }
    void* rtv()    const noexcept override { return rt_surf.Get(); }
};

// ── D3D9 Buffer data ─────────────────────────────────────────────────────────

struct D3D9BufferData final : BufferData {
    // D3D9 has no cbuffer concept; we use a vertex shader float constant array
    // and upload via SetVertexShaderConstantF.
    std::vector<float> cpu_data;
    uint32_t           size = 0;

    void* native() const noexcept override {
        return const_cast<float*>(cpu_data.data());
    }
};

// ── D3D9 Pipeline data ───────────────────────────────────────────────────────

struct D3D9PipelineData final : PipelineData {
    ComPtr<IDirect3DVertexShader9> vs;
    ComPtr<IDirect3DPixelShader9>  ps;
    bool alpha_blend = false;
    bool depth_test  = false;
};

// Fullscreen quad vertex format for D3D9.
struct D3D9QuadVertex {
    float x, y, z, w;
    float u, v;
};
static constexpr DWORD kFVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

// ── D3D9 Pass ────────────────────────────────────────────────────────────────

class D3D9Pass final : public Pass {
public:
    D3D9Pass(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* quad_vb);
    ~D3D9Pass() override;

    void set_render_target(Texture& rt) override;
    void set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
    void set_pipeline(Pipeline& p) override;
    void bind(uint32_t slot, Texture& t) override;
    void update_params(const VanGFXParams& params) override;
    void draw_fullscreen() override;
    void draw(uint32_t vertices, uint32_t instances) override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override;
    void end() override;

private:
    IDirect3DDevice9*          dev_     = nullptr;
    IDirect3DVertexBuffer9*    quad_vb_ = nullptr;
    bool                       ended_   = false;
    ComPtr<IDirect3DStateBlock9> state_block_;
    D3D9PipelineData*          pipeline_ = nullptr;
};

// ── D3D9 Device ──────────────────────────────────────────────────────────────

class D3D9Device final : public Device {
public:
    explicit D3D9Device(IDirect3DDevice9* dev);

    [[nodiscard]] Api api() const override { return Api::D3D9; }

    [[nodiscard]] Result<Texture>  create_texture(const vangfx::TextureDesc& desc,
                                                   const InitData* init) override;
    [[nodiscard]] Result<Buffer>   create_buffer(uint32_t size,
                                                  const void* init) override;
    [[nodiscard]] Result<Pipeline> create_pipeline(const PipelineDesc& desc) override;
    void update_buffer(Buffer& buf, const void* data, uint32_t size) override;

    [[nodiscard]] std::unique_ptr<Pass> begin_pass() override;
    [[nodiscard]] void* native() const override { return dev_.Get(); }

    [[nodiscard]] Texture wrap_texture(void* native_tex, void* native_srv,
                                        void* native_rtv, uint32_t width,
                                        uint32_t height,
                                        vangfx::SurfaceFormat format) override;

private:
    bool ensure_quad_vb();

    ComPtr<IDirect3DDevice9>       dev_;
    ComPtr<IDirect3DVertexBuffer9> quad_vb_;
};

std::unique_ptr<Device> make_d3d9_device(IDirect3DDevice9* dev);

} // namespace vangfx::gpu
