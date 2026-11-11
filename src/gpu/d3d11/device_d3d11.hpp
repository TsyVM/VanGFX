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
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace vangfx::gpu {

using Microsoft::WRL::ComPtr;

// ── D3D11 Texture data ───────────────────────────────────────────────────────

struct D3D11TextureData final : TextureData {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv_view_;
    ComPtr<ID3D11RenderTargetView>   rtv_view_;
    bool  owns_tex = true; // false = wrapped externally

    void* native() const noexcept override { return tex.Get(); }
    void* srv()    const noexcept override { return srv_view_.Get(); }
    void* rtv()    const noexcept override { return rtv_view_.Get(); }
};

// ── D3D11 Buffer data ────────────────────────────────────────────────────────

struct D3D11BufferData final : BufferData {
    ComPtr<ID3D11Buffer> buf;
    void* native() const noexcept override { return buf.Get(); }
};

// ── D3D11 Pipeline data ──────────────────────────────────────────────────────

struct D3D11PipelineData final : PipelineData {
    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   ps;
    ComPtr<ID3D11BlendState>    blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState>   raster;
    ComPtr<ID3D11SamplerState>  samp; // linear clamp
    bool is_compute = false;
    ComPtr<ID3D11ComputeShader> cs;
};

// ── D3D11 Pass ───────────────────────────────────────────────────────────────

class D3D11Pass final : public Pass {
public:
    D3D11Pass(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    ~D3D11Pass() override;

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
    void save_state();
    void restore_state();

    ID3D11Device*        dev_  = nullptr;
    ID3D11DeviceContext* ctx_  = nullptr;
    bool                 ended_ = false;

    // Params cbuffer (created lazily, one per pass for simplicity)
    ComPtr<ID3D11Buffer> params_cb_;

    // Fullscreen VS (compiled once, cached in device)
    ComPtr<ID3D11VertexShader> fullscreen_vs_;

    // Active pipeline
    D3D11PipelineData* pipeline_ = nullptr;
    bool  pipeline_set_ = false;

    // Active custom RT (null = use saved state's RT)
    ComPtr<ID3D11RenderTargetView> custom_rtv_;

    // Saved state
    struct SavedState {
        static constexpr int kMaxRTs = 8;
        static constexpr int kMaxSRVs = 4;

        ComPtr<ID3D11RenderTargetView>   rtvs[kMaxRTs];
        ComPtr<ID3D11DepthStencilView>   dsv;
        uint32_t                         rtv_count = 0;

        D3D11_PRIMITIVE_TOPOLOGY         topology  = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ComPtr<ID3D11InputLayout>        input_layout;

        ComPtr<ID3D11VertexShader>       vs;
        ComPtr<ID3D11PixelShader>        ps;

        ComPtr<ID3D11Buffer>             ps_cb0;
        ComPtr<ID3D11ShaderResourceView> ps_srvs[kMaxSRVs];
        ComPtr<ID3D11SamplerState>       ps_samp0;

        ComPtr<ID3D11BlendState>         blend;
        FLOAT                            blend_factor[4] = {};
        UINT                             blend_mask      = 0;

        ComPtr<ID3D11DepthStencilState>  depth;
        UINT                             stencil_ref = 0;

        ComPtr<ID3D11RasterizerState>    raster;

        UINT                             viewport_count = 0;
        D3D11_VIEWPORT                   viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

        UINT                             scissor_count = 0;
        D3D11_RECT                       scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    } saved_;
};

// ── D3D11 Device ─────────────────────────────────────────────────────────────

class D3D11Device final : public Device {
public:
    D3D11Device(ID3D11Device* dev, ID3D11DeviceContext* ctx);

    [[nodiscard]] Api api() const override { return Api::D3D11; }

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

    // Shared fullscreen VS so all passes use the same compiled blob.
    [[nodiscard]] ID3D11VertexShader* fullscreen_vs();

private:
    ComPtr<ID3D11Device>        dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VertexShader>  fullscreen_vs_;
};

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_d3d11_device(ID3D11Device* dev,
                                           ID3D11DeviceContext* ctx);

} // namespace vangfx::gpu
