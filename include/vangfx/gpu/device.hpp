#pragma once

/**
 * vangfx/gpu/device.hpp — gpu::Device: a borrowed-device wrapper.
 *
 * Device wraps the game's native API device and provides a unified interface
 * for allocating GPU resources and opening passes.  It does NOT create a
 * device: the front-end backend hands it one.
 *
 * Factory functions live in backend-specific headers so that including this
 * header does not pull in D3D or Vulkan declarations.
 *
 *   D3D9  — #include <vangfx/gpu/d3d9_device.hpp>
 *   D3D11 — #include <vangfx/gpu/d3d11_device.hpp>
 *   D3D12 — #include <vangfx/gpu/d3d12_device.hpp>
 */

#include <vangfx/gpu/api.hpp>
#include <vangfx/gpu/init_data.hpp>
#include <vangfx/gpu/pipeline_desc.hpp>
#include <vangfx/gpu/resources.hpp>
#include <vangfx/gpu/pass.hpp>
#include <vangfx/resources/texture.hpp>  // vangfx::TextureDesc
#include <vangfx/core/error.hpp>         // vangfx::Result<T>
#include <memory>

namespace vangfx::gpu {

class Device {
public:
    virtual ~Device() = default;

    // Which graphics API this device wraps.
    [[nodiscard]] virtual Api api() const = 0;

    // ── Resource creation ─────────────────────────────────────────────────────

    // Create a 2-D texture.  init may be null for a render target or an empty
    // texture that will be filled later.
    [[nodiscard]] virtual Result<Texture>  create_texture(
        const vangfx::TextureDesc& desc,
        const InitData*            init = nullptr) = 0;

    // Create a constant buffer of `size` bytes.  init may be null.
    [[nodiscard]] virtual Result<Buffer>   create_buffer(
        uint32_t    size,
        const void* init = nullptr) = 0;

    // Create a pipeline.  Compiles shaders from HLSL if bytecode is absent.
    [[nodiscard]] virtual Result<Pipeline> create_pipeline(
        const PipelineDesc& desc) = 0;

    // Update the contents of a constant buffer (upload data, size <= buffer).
    virtual void update_buffer(Buffer& buf, const void* data, uint32_t size) = 0;

    // ── Frame work ────────────────────────────────────────────────────────────

    // Open a pass.  Saves game state (D3D9/D3D11) or starts recording into the
    // mod's command list (D3D12).  Call pass->end() when done.
    [[nodiscard]] virtual std::unique_ptr<Pass> begin_pass() = 0;

    // ── Backend access ────────────────────────────────────────────────────────

    // The underlying API device pointer (ID3D11Device*, IDirect3DDevice9*, …).
    [[nodiscard]] virtual void* native() const = 0;

    // Wrap an existing API texture as a gpu::Texture (adds no ref count).
    // For D3D11: native_tex is ID3D11Texture2D*, native_srv is ID3D11ShaderResourceView*,
    //            native_rtv is ID3D11RenderTargetView*.
    // For D3D9:  native_tex is IDirect3DTexture9*.
    // For D3D12: native_tex is ID3D12Resource*, gpu_va_or_rtv_cpu is the
    //            D3D12_CPU_DESCRIPTOR_HANDLE.ptr for an RTV, or 0 for SRV-only.
    [[nodiscard]] virtual Texture wrap_texture(void* native_tex, void* native_srv,
                                               void* native_rtv_or_dsv,
                                               uint32_t width, uint32_t height,
                                               vangfx::SurfaceFormat format) = 0;
};

} // namespace vangfx::gpu
