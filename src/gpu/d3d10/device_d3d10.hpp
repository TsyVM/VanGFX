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
#include <d3d10_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace vangfx::gpu {

using Microsoft::WRL::ComPtr;

// ── D3D10 Texture data ───────────────────────────────────────────────────────

struct D3D10TextureData final : TextureData {
    ComPtr<ID3D10Texture2D>          tex;
    ComPtr<ID3D10ShaderResourceView> srv_view_;
    ComPtr<ID3D10RenderTargetView>   rtv_view_;
    bool  owns_tex = true;

    void* native() const noexcept override { return tex.Get(); }
    void* srv()    const noexcept override { return srv_view_.Get(); }
    void* rtv()    const noexcept override { return rtv_view_.Get(); }
};

// ── D3D10 Buffer data ────────────────────────────────────────────────────────

struct D3D10BufferData final : BufferData {
    ComPtr<ID3D10Buffer> buf;
    void* native() const noexcept override { return buf.Get(); }
};

// ── D3D10 Pipeline data ──────────────────────────────────────────────────────

struct D3D10PipelineData final : PipelineData {
    ComPtr<ID3D10VertexShader>      vs;
    ComPtr<ID3D10PixelShader>       ps;
    ComPtr<ID3D10BlendState>        blend;
    ComPtr<ID3D10DepthStencilState> depth;
    ComPtr<ID3D10RasterizerState>   raster;
    ComPtr<ID3D10SamplerState>      samp;
};

// ── D3D10 Pass ───────────────────────────────────────────────────────────────

class D3D10Pass final : public Pass {
public:
    explicit D3D10Pass(ID3D10Device* dev);
    ~D3D10Pass() override;

    void set_render_target(Texture& rt)                      override;
    void set_viewport(uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h)                override;
    void set_pipeline(Pipeline& p)                           override;
    void bind(uint32_t slot, Texture& t)                     override;
    void update_params(const VanGFXParams& params)           override;
    void draw_fullscreen()                                    override;
    void draw(uint32_t vertices, uint32_t instances)         override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z)        override;
    void end()                                                override;

private:
    void save_state();
    void restore_state();

    ID3D10Device*       dev_      = nullptr;
    bool                ended_    = false;
    ComPtr<ID3D10Buffer> params_cb_;
    D3D10PipelineData*  pipeline_ = nullptr;
    ComPtr<ID3D10RenderTargetView> custom_rtv_;

    static constexpr int kMaxRTs  = 8;
    static constexpr int kMaxSRVs = 8;

    struct Saved {
        ComPtr<ID3D10RenderTargetView>   rtvs[kMaxRTs];
        ComPtr<ID3D10DepthStencilView>   dsv;
        uint32_t                         rtv_count = 0;

        D3D10_PRIMITIVE_TOPOLOGY         topology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ComPtr<ID3D10InputLayout>        input_layout;
        ComPtr<ID3D10VertexShader>       vs;
        ComPtr<ID3D10PixelShader>        ps;
        ComPtr<ID3D10GeometryShader>     gs;
        ComPtr<ID3D10Buffer>             ps_cb0;
        ComPtr<ID3D10ShaderResourceView> ps_srvs[kMaxSRVs];
        ComPtr<ID3D10SamplerState>       ps_samp0;
        ComPtr<ID3D10BlendState>         blend;
        FLOAT                            blend_factor[4] = {};
        UINT                             blend_mask      = 0;
        ComPtr<ID3D10DepthStencilState>  depth;
        UINT                             stencil_ref     = 0;
        ComPtr<ID3D10RasterizerState>    raster;
        UINT                             viewport_count  = 0;
        D3D10_VIEWPORT viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    } saved_;
};

// ── D3D10 Device ─────────────────────────────────────────────────────────────

class D3D10Device final : public Device {
public:
    explicit D3D10Device(ID3D10Device* dev);

    [[nodiscard]] Api api() const override { return Api::D3D10; }

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

    [[nodiscard]] ID3D10VertexShader* fullscreen_vs();

private:
    ComPtr<ID3D10Device>       dev_;
    ComPtr<ID3D10VertexShader> fullscreen_vs_;
};

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_d3d10_device(ID3D10Device* dev);

} // namespace vangfx::gpu
