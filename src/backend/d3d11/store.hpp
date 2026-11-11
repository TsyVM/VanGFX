#pragma once

/**
 * src/backend/d3d11/store.hpp — the resources a D3D11 context owns on behalf of
 * callbacks: textures with their views, compiled shaders, and constant buffers.
 *
 * Separate from frame_context.hpp because D3D11ContextImpl holds one of these
 * by value and the frame context needs the context; one header would be a
 * cycle.
 *
 * D3D11 is the comfortable one of the three. A texture is a resource plus
 * whichever views it needs, a constant buffer is an actual buffer object, and
 * readback is CopyResource into a staging texture followed by Map. None of the
 * D3D9 pool rules apply and none of the D3D12 descriptor bookkeeping is needed.
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
#include <d3d11.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d11 {

class D3D11ResourceStore {
public:
    D3D11ResourceStore() = default;
    ~D3D11ResourceStore() { release_all(); }

    D3D11ResourceStore(const D3D11ResourceStore&)            = delete;
    D3D11ResourceStore& operator=(const D3D11ResourceStore&) = delete;

    void attach(ID3D11Device* device, ID3D11DeviceContext* ctx) noexcept
    {
        device_ = device;
        ctx_    = ctx;
    }

    void release_all() noexcept;

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

    [[nodiscard]] ID3D11Texture2D*          texture(TextureHandle h)  const noexcept;
    [[nodiscard]] ID3D11ShaderResourceView* srv(TextureHandle h)      const noexcept;
    [[nodiscard]] ID3D11RenderTargetView*   rtv(RenderTargetHandle h) const noexcept;
    [[nodiscard]] ID3D11DepthStencilView*   dsv(DepthBufferHandle h)  const noexcept;

    // Registers a texture the *game* owns under the id events report for it,
    // so a callback can take a handle from an event and read it back or ask
    // its format. Borrowed: forget() drops it when the game's resource dies.
    void adopt_render_target(uint64_t id, ID3D11Resource* res) noexcept;
    void adopt_depth_buffer (uint64_t id, ID3D11Resource* res) noexcept;
    [[nodiscard]] ID3D11Resource* game_resource(uint64_t id) const noexcept;

    // ── Readback ─────────────────────────────────────────────────────────────

    Result<ReadbackData> readback(TextureHandle h);
    Result<ReadbackData> readback(RenderTargetHandle h);
    Result<ReadbackData> readback(DepthBufferHandle h);

    // Every buffer the application creates, registered as resource.cpp sees it
    // go past. One map rather than six: the six buffer handle types are one id
    // space, and a caller holding the handle it was given should not be turned
    // away because CreateBuffer's bind flags put that id in another bucket.
    void adopt_buffer(uint64_t id, ID3D11Buffer* buf, uint32_t size) noexcept;
    void forget(uint64_t id) noexcept;

    // Any of the six buffer handle types. `size` of 0 reads to the end; both
    // offset and size are clamped to the buffer.
    Result<BufferData> read_buffer(uint64_t id, uint64_t offset, uint64_t size);

    // ── Shaders ──────────────────────────────────────────────────────────────

    Result<Shader> register_shader(uint64_t id, const ShaderBuilder& b);
    void           destroy_shader(ShaderHandle h) noexcept;
    [[nodiscard]] bool has_shader(ShaderHandle h) const noexcept;

    [[nodiscard]] ID3D11VertexShader* vertex_shader(ShaderHandle h) const noexcept;
    [[nodiscard]] ID3D11PixelShader*  pixel_shader(ShaderHandle h)  const noexcept;

    // ── Constant buffers ─────────────────────────────────────────────────────

    // Rewrites one of the game's constant buffers, whole: D3D11 cannot update
    // part of one (a dynamic buffer is discarded on Map, a default one takes
    // UpdateSubresource of the full size). `size` must be the buffer's size.
    Result<void> update_constant_buffer(ConstantBufferHandle h,
                                        const void* data, uint32_t size);

private:
    struct TextureRec {
        ID3D11Texture2D*          tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11RenderTargetView*   rtv = nullptr;
        ID3D11DepthStencilView*   dsv = nullptr;
        SurfaceFormat format = SurfaceFormat::Unknown;
        uint32_t      width  = 0;
        uint32_t      height = 0;
    };
    struct ViewRec {
        // `owned` distinguishes a view this store created from a texture
        // adopted off a hook, which belongs to the game (view is null then).
        ID3D11View*   view   = nullptr;
        ID3D11Texture2D* tex = nullptr;
        SurfaceFormat format = SurfaceFormat::Unknown;
        uint32_t      width  = 0;
        uint32_t      height = 0;
        bool          owned  = false;
    };
    struct ShaderRec {
        ID3D11VertexShader*   vs = nullptr;
        ID3D11PixelShader*    ps = nullptr;
        ID3D11GeometryShader* gs = nullptr;
        ID3D11HullShader*     hs = nullptr;
        ID3D11DomainShader*   ds = nullptr;
        ID3D11ComputeShader*  cs = nullptr;
        ShaderStage           stage{};
    };

    // Fills a ViewRec's size and format from the texture behind a view.
    static void describe_from_view(ViewRec& rec) noexcept;

    // The shared half of all three readback overloads: stage `src`, map it,
    // repack into scratch_.
    Result<ReadbackData> read_texture(ID3D11Texture2D* src);

    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* ctx_    = nullptr;

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, TextureRec> textures_;
    std::unordered_map<uint64_t, ViewRec>    render_targets_;
    std::unordered_map<uint64_t, ViewRec>    depth_buffers_;
    std::unordered_map<uint64_t, ShaderRec>  shaders_;

    // Buffers the application owns. Borrowed pointers: never released here, and
    // dropped by forget() when the application's own Release destroys them.
    struct BufferRec { ID3D11Buffer* buf = nullptr; uint32_t size = 0; };
    std::unordered_map<uint64_t, BufferRec> buffers_;

    std::vector<uint8_t> scratch_;
};

}  // namespace vangfx::backend::d3d11
