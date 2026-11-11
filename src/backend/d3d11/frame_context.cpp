/**
 * src/backend/d3d11/frame_context.cpp — D3D11FrameContextImpl.
 *
 * Surfaces and pipeline state are the device context's the callback was raised
 * on (immediate or deferred); the back buffer's size and format come from the
 * swap chain. Readback and resource creation run on the immediate context, so
 * they are refused from a callback raised on a deferred context: that runs on a
 * worker thread while the game may be using the immediate context on another.
 */

#include "d3d11_impl.hpp"
#include <format_map.hpp>

namespace vangfx::backend::d3d11 {

Result<void> D3D11FrameContextImpl::on_immediate() const {
    if (dc_ && dc_ != ctx_.immediate()) {
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D11: readback and resource creation use the immediate context, and this callback was raised "
            "on a deferred one (another thread may be using the immediate context). Do it from an immediate-"
            "context or present callback.");
    }
    return {};
}

FrameSurfaces D3D11FrameContextImpl::surfaces() const noexcept {
    FrameSurfaces out = state_ ? state_->surfaces : FrameSurfaces{};
    if (IDXGISwapChain* sc = ctx_.swap_chain()) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            out.width             = d.BufferDesc.Width;
            out.height            = d.BufferDesc.Height;
            out.backbuffer_format = fmtmap::from_dxgi(d.BufferDesc.Format);
            out.msaa_samples      = d.SampleDesc.Count ? d.SampleDesc.Count : 1u;
        }
        ID3D11Resource* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
            out.backbuffer = { resource_id(bb) };
            bb->Release();
        }
    }
    return out;
}

PipelineState D3D11FrameContextImpl::pipeline() const noexcept {
    return state_ ? state_->pipeline : PipelineState{};
}

SurfaceFormat D3D11FrameContextImpl::format_of(RenderTargetHandle h) const noexcept { return ctx_.store().format_of(h); }
SurfaceFormat D3D11FrameContextImpl::format_of(DepthBufferHandle h)  const noexcept { return ctx_.store().format_of(h); }
SurfaceFormat D3D11FrameContextImpl::format_of(TextureHandle h)      const noexcept { return ctx_.store().format_of(h); }

Result<TextureHandle> D3D11FrameContextImpl::create_texture(const TextureDesc& d) {
    return ctx_.store().create_texture(ContextImpl::alloc_id(), d);
}
Result<RenderTargetHandle> D3D11FrameContextImpl::create_render_target(const RenderTargetDesc& d) {
    return ctx_.store().create_render_target(ContextImpl::alloc_id(), d);
}
Result<DepthBufferHandle> D3D11FrameContextImpl::create_depth_buffer(const DepthBufferDesc& d) {
    return ctx_.store().create_depth_buffer(ContextImpl::alloc_id(), d);
}
void D3D11FrameContextImpl::destroy(TextureHandle h)      noexcept { ctx_.store().destroy(h); }
void D3D11FrameContextImpl::destroy(RenderTargetHandle h) noexcept { ctx_.store().destroy(h); }
void D3D11FrameContextImpl::destroy(DepthBufferHandle h)  noexcept { ctx_.store().destroy(h); }

// The replacement is set past the hooks for the draw being reported, and the
// draw thunk puts the game's shader back after the draw.
Result<void> D3D11FrameContextImpl::bind_effect(const Effect& e) {
    if (!state_ || !dc_) {
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D11: bind_effect swaps the shaders of the draw being reported, so it works only inside an "
            "on_draw callback.");
    }
    if (is_valid(e.gs.handle) || is_valid(e.hs.handle) || is_valid(e.ds.handle) || is_valid(e.cs.handle))
        return gfx_err<void>(ShaderError::Unsupported, "D3D11: only the vertex and pixel stages are swapped.");
    ID3D11VertexShader* vs = is_valid(e.vs.handle) ? ctx_.store().vertex_shader(e.vs.handle) : nullptr;
    ID3D11PixelShader*  ps = is_valid(e.ps.handle) ? ctx_.store().pixel_shader(e.ps.handle) : nullptr;
    if ((is_valid(e.vs.handle) && !vs) || (is_valid(e.ps.handle) && !ps) || (!vs && !ps)) {
        return gfx_err<void>(ShaderError::InvalidHandle,
            "D3D11: the Effect names a shader this context did not issue, or one already destroyed.");
    }
    if (vs) { set_vs_unreported(dc_, vs, nullptr, 0); state_->vs_swapped = true; }
    if (ps) { set_ps_unreported(dc_, ps, nullptr, 0); state_->ps_swapped = true; }
    return {};
}

void D3D11FrameContextImpl::unbind_effect() noexcept {
    if (state_ && dc_) restore_shaders(dc_, *state_);
}

Result<void> D3D11FrameContextImpl::update_constant_buffer(ConstantBufferHandle h, const void* data, uint32_t size) {
    if (auto ok = on_immediate(); !ok) return ok;
    return ctx_.store().update_constant_buffer(h, data, size);
}

#define VANGFX_ON_IMMEDIATE(T) if (auto ok = on_immediate(); !ok) return std::unexpected(ok.error())

Result<ReadbackData> D3D11FrameContextImpl::readback(TextureHandle h)      { VANGFX_ON_IMMEDIATE(ReadbackData); return ctx_.store().readback(h); }
Result<ReadbackData> D3D11FrameContextImpl::readback(RenderTargetHandle h) { VANGFX_ON_IMMEDIATE(ReadbackData); return ctx_.store().readback(h); }
Result<ReadbackData> D3D11FrameContextImpl::readback(DepthBufferHandle h)  { VANGFX_ON_IMMEDIATE(ReadbackData); return ctx_.store().readback(h); }

Result<BufferData> D3D11FrameContextImpl::readback(VertexBufferHandle h, uint64_t o, uint64_t n)     { VANGFX_ON_IMMEDIATE(BufferData); return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D11FrameContextImpl::readback(IndexBufferHandle h, uint64_t o, uint64_t n)      { VANGFX_ON_IMMEDIATE(BufferData); return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D11FrameContextImpl::readback(ConstantBufferHandle h, uint64_t o, uint64_t n)   { VANGFX_ON_IMMEDIATE(BufferData); return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D11FrameContextImpl::readback(StructuredBufferHandle h, uint64_t o, uint64_t n) { VANGFX_ON_IMMEDIATE(BufferData); return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D11FrameContextImpl::readback(RawBufferHandle h, uint64_t o, uint64_t n)        { VANGFX_ON_IMMEDIATE(BufferData); return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D11FrameContextImpl::readback(UAVHandle h, uint64_t o, uint64_t n)              { VANGFX_ON_IMMEDIATE(BufferData); return ctx_.store().read_buffer(h.id, o, n); }

#undef VANGFX_ON_IMMEDIATE

Result<BufferData> D3D11FrameContextImpl::readback_at(uint64_t, uint64_t) {
    return gfx_err<BufferData>(ShaderError::Unsupported, "D3D11 has no GPU virtual addresses; read a buffer by its handle.");
}

bool D3D11FrameContextImpl::capture_active() const noexcept { return ctx_.capture_active(); }

Result<GBufferCapture> D3D11FrameContextImpl::latest_capture() {
    return ctx_.latest_gbuffer();
}

void* D3D11FrameContextImpl::native_device()  const noexcept { return ctx_.device(); }
void* D3D11FrameContextImpl::native_context() const noexcept { return dc_ ? dc_ : ctx_.immediate(); }

void* D3D11FrameContextImpl::native_resource(RenderTargetHandle h) const noexcept {
    if (ID3D11Texture2D* t = ctx_.store().texture(TextureHandle{ h.id })) return t;
    return resource_from_id(h.id);
}
void* D3D11FrameContextImpl::native_resource(DepthBufferHandle h) const noexcept {
    if (ID3D11Texture2D* t = ctx_.store().texture(TextureHandle{ h.id })) return t;
    return resource_from_id(h.id);
}
void* D3D11FrameContextImpl::native_resource(TextureHandle h) const noexcept {
    if (ID3D11Texture2D* t = ctx_.store().texture(h)) return t;
    return resource_from_id(h.id);
}
void* D3D11FrameContextImpl::native_buffer(uint64_t id) const noexcept { return resource_from_id(id); }

}  // namespace vangfx::backend::d3d11
