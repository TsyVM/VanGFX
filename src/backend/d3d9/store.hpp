#pragma once

/**
 * src/backend/d3d9/store.hpp — the resources a D3D9 context owns on behalf of
 * callbacks (textures, surfaces, compiled shaders), and the game's resources it
 * has been shown, so a callback can read one back by the id an event gave.
 *
 * Separate from d3d9_impl.hpp because D3D9ContextImpl holds one of these by
 * value, and the frame context needs the context: one header would be a cycle.
 */

#include <vangfx/core/error.hpp>
#include <vangfx/core/shader.hpp>
#include <vangfx/resources/buffer.hpp>
#include <vangfx/resources/format.hpp>
#include <vangfx/resources/surface.hpp>
#include <vangfx/resources/texture.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d9 {

class D3D9ResourceStore {
public:
    D3D9ResourceStore() = default;
    ~D3D9ResourceStore() { release_all(); }

    D3D9ResourceStore(const D3D9ResourceStore&)            = delete;
    D3D9ResourceStore& operator=(const D3D9ResourceStore&) = delete;

    void attach(IDirect3DDevice9* device) noexcept { device_ = device; }

    // Every COM object the store holds. The context's destructor.
    void release_all() noexcept;
    // Before a plain device's Reset, which fails while anything in the default
    // pool is alive: the store's render targets, depth buffers and default-pool
    // textures go (their handles stop working), managed textures and shaders
    // stay. The game's surfaces are forgotten; the game releases them itself.
    void release_default_pool() noexcept;

    // ── Textures and surfaces ────────────────────────────────────────────────

    Result<TextureHandle>      create_texture(uint64_t id, const TextureDesc& d);
    Result<RenderTargetHandle> create_render_target(uint64_t id, const RenderTargetDesc& d);
    Result<DepthBufferHandle>  create_depth_buffer(uint64_t id, const DepthBufferDesc& d);

    void destroy(TextureHandle h)      noexcept;
    void destroy(RenderTargetHandle h) noexcept;
    void destroy(DepthBufferHandle h)  noexcept;

    [[nodiscard]] SurfaceFormat format_of(TextureHandle h)      const noexcept;
    [[nodiscard]] SurfaceFormat format_of(RenderTargetHandle h) const noexcept;
    [[nodiscard]] SurfaceFormat format_of(DepthBufferHandle h)  const noexcept;

    [[nodiscard]] IDirect3DTexture9* texture(TextureHandle h)      const noexcept;
    [[nodiscard]] IDirect3DSurface9* surface(RenderTargetHandle h) const noexcept;
    [[nodiscard]] IDirect3DSurface9* surface(DepthBufferHandle h)  const noexcept;

    // A surface the *game* owns, under the id events report for it (bound as a
    // target: rendertarget.cpp). Borrowed: forget() drops it when the game's
    // resource dies.
    void adopt_render_target(uint64_t id, IDirect3DSurface9* surface) noexcept;
    void adopt_depth_buffer (uint64_t id, IDirect3DSurface9* surface) noexcept;

    // ── Readback ─────────────────────────────────────────────────────────────
    //
    // The returned span points into scratch_, which is reused by the next call.
    // That matches what the public header promises: valid until the next call
    // on this FrameContext, or the end of the frame.

    Result<ReadbackData> readback(TextureHandle h);
    Result<ReadbackData> readback(RenderTargetHandle h);
    Result<ReadbackData> readback(DepthBufferHandle h);

    // ── Vertex and index buffers ─────────────────────────────────────────────
    //
    // Registered as the application creates them, so the handle a ResourceEvent
    // reports is one read_buffer() accepts. The usage flags are kept because
    // whether a buffer can be read at all is decided at creation and cannot be
    // worked out later -- see read_buffer.
    void adopt_vertex_buffer(uint64_t id, IDirect3DVertexBuffer9* vb,
                             uint32_t size, uint32_t usage, uint32_t pool) noexcept;
    void adopt_index_buffer (uint64_t id, IDirect3DIndexBuffer9* ib,
                             uint32_t size, uint32_t usage, uint32_t pool) noexcept;
    // The game's resource `id` is gone: drop every borrowed pointer to it.
    void forget(uint64_t id) noexcept;

    Result<BufferData> read_buffer(uint64_t id, uint64_t offset, uint64_t size);

    // ── Shaders ──────────────────────────────────────────────────────────────
    //
    // A vs_3_0 runs only with a ps_3_0 and the other way round, and neither
    // with the fixed-function pipeline; a DirectX 9.0c game may use any shader
    // model from 1_1 to 3_0, or none. So a replacement is kept with its source,
    // and the form a draw needs -- shader model 3, or 2 -- is made the first
    // time a draw needs it.

    Result<Shader> register_shader(uint64_t id, const ShaderBuilder& b);
    void           destroy_shader(ShaderHandle h) noexcept;
    [[nodiscard]] bool has_shader(ShaderHandle h) const noexcept;

    // The shader as compiled.
    [[nodiscard]] IDirect3DVertexShader9* vertex_shader(ShaderHandle h) const noexcept;
    [[nodiscard]] IDirect3DPixelShader9*  pixel_shader (ShaderHandle h) const noexcept;
    // The shader in shader model 3 (`sm3`) or below it. Null with `why` set
    // when it cannot be had; `first_failure` says this is the first time.
    IDirect3DVertexShader9* vertex_shader_for(ShaderHandle h, bool sm3, std::string& why, bool& first_failure);
    IDirect3DPixelShader9*  pixel_shader_for (ShaderHandle h, bool sm3, std::string& why, bool& first_failure);

private:
    struct TextureRec {
        IDirect3DTexture9* tex    = nullptr;
        SurfaceFormat      format = SurfaceFormat::Unknown;
        uint32_t           width  = 0;
        uint32_t           height = 0;
        D3DPOOL            pool   = D3DPOOL_DEFAULT;
    };
    struct SurfaceRec {
        IDirect3DSurface9* surf   = nullptr;
        SurfaceFormat      format = SurfaceFormat::Unknown;
        uint32_t           width  = 0;
        uint32_t           height = 0;
        bool               owned  = true;   // false: the game's, borrowed
    };
    struct ShaderRec {
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9*  ps = nullptr;
        ShaderStage             stage{};
        uint8_t                 major = 0;       // as compiled
        ShaderBuilder           source;          // for the other shader model
        IDirect3DVertexShader9* other_vs = nullptr;
        IDirect3DPixelShader9*  other_ps = nullptr;
        bool                    other_failed = false;
        std::string             other_why;
    };
    // One record for both buffer kinds: Lock has the same shape on each, and
    // exactly one of the two pointers is set.
    struct BufferRec {
        IDirect3DVertexBuffer9* vb    = nullptr;
        IDirect3DIndexBuffer9*  ib    = nullptr;
        uint32_t                size  = 0;
        uint32_t                usage = 0;
        uint32_t                pool  = 0;
    };

    // Copies a surface into scratch_ and describes it. Shared by the readback
    // overloads, which differ only in how they get the surface.
    Result<ReadbackData> read_surface(IDirect3DSurface9* src, bool src_is_lockable);
    void adopt(std::unordered_map<uint64_t, SurfaceRec>& map, uint64_t id, IDirect3DSurface9* surface) noexcept;
    template <typename S>
    S* shader_for(ShaderHandle h, bool sm3, std::string& why, bool& first_failure);

    IDirect3DDevice9* device_ = nullptr;

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, TextureRec> textures_;
    std::unordered_map<uint64_t, SurfaceRec> render_targets_;
    std::unordered_map<uint64_t, SurfaceRec> depth_buffers_;
    std::unordered_map<uint64_t, ShaderRec>  shaders_;
    std::unordered_map<uint64_t, BufferRec>  buffers_;

    std::vector<uint8_t> scratch_;
};

}  // namespace vangfx::backend::d3d9
