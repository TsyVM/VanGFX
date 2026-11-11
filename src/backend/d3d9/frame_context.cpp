/**
 * src/backend/d3d9/frame_context.cpp — D3D9FrameContextImpl.
 *
 * Surfaces and pipeline state are the device's as the hooks have followed
 * them; the back buffer's size and format come from the swap chain presenting
 * (or the device's own). Resources are the context's store's, and the game's
 * own resources by the ids their events gave.
 *
 * bind_effect swaps the shaders of the draw being reported, and chooses each
 * replacement's shader model to fit what it is paired with: vs_3_0 runs only
 * with ps_3_0, anything below 3 only with below 3 or the fixed-function
 * pipeline. A replacement pixel shader for a DirectX 9.0c game drawing with
 * vs_2_0 (or no vertex shader) is recompiled as shader model 2 the first time.
 */

#include "d3d9_impl.hpp"
#include <format_map.hpp>

namespace vangfx::backend::d3d9 {

namespace {

SurfaceFormat format_of_resource(IDirect3DResource9* res) noexcept {
    if (!res) return SurfaceFormat::Unknown;
    D3DSURFACE_DESC d{};
    switch (res->GetType()) {
    case D3DRTYPE_SURFACE:
        if (SUCCEEDED(static_cast<IDirect3DSurface9*>(res)->GetDesc(&d))) return fmtmap::from_d3d9(d.Format);
        break;
    case D3DRTYPE_TEXTURE:
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(res)->GetLevelDesc(0, &d))) return fmtmap::from_d3d9(d.Format);
        break;
    case D3DRTYPE_CUBETEXTURE:
        if (SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(res)->GetLevelDesc(0, &d))) return fmtmap::from_d3d9(d.Format);
        break;
    case D3DRTYPE_VOLUMETEXTURE: {
        D3DVOLUME_DESC v{};
        if (SUCCEEDED(static_cast<IDirect3DVolumeTexture9*>(res)->GetLevelDesc(0, &v))) return fmtmap::from_d3d9(v.Format);
        break;
    }
    default: break;
    }
    return SurfaceFormat::Unknown;
}

// A target by id: the surface itself, or the top level of a texture reported
// under the texture's id. Weak: the texture keeps its levels alive.
void* target_surface(uint64_t id) noexcept {
    IDirect3DResource9* res = resource_from_id(id);
    if (!res || res->GetType() != D3DRTYPE_TEXTURE) return res;
    IDirect3DSurface9* top = nullptr;
    if (FAILED(static_cast<IDirect3DTexture9*>(res)->GetSurfaceLevel(0, &top)) || !top) return nullptr;
    top->Release();
    return top;
}

} // namespace

FrameSurfaces D3D9FrameContextImpl::surfaces() const noexcept {
    FrameSurfaces out = state_ ? state_->surfaces : FrameSurfaces{};
    IDirect3DSwapChain9* chain = swap_chain_;
    if (!chain && (FAILED(ctx_.device()->GetSwapChain(0, &chain)) || !chain)) return out;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(bb->GetDesc(&d))) {
            out.width             = d.Width;
            out.height            = d.Height;
            out.backbuffer_format = fmtmap::from_d3d9(d.Format);
            out.msaa_samples      = d.MultiSampleType == D3DMULTISAMPLE_NONE ? 1u : static_cast<uint32_t>(d.MultiSampleType);
        }
        out.backbuffer = { resource_id(bb) };
        bb->Release();
    }
    if (!swap_chain_) chain->Release();
    return out;
}

PipelineState D3D9FrameContextImpl::pipeline() const noexcept {
    return state_ ? state_->pipeline : PipelineState{};
}

SurfaceFormat D3D9FrameContextImpl::format_of(RenderTargetHandle h) const noexcept {
    const SurfaceFormat f = ctx_.store().format_of(h);
    return f != SurfaceFormat::Unknown ? f : format_of_resource(resource_from_id(h.id));
}
SurfaceFormat D3D9FrameContextImpl::format_of(DepthBufferHandle h) const noexcept {
    const SurfaceFormat f = ctx_.store().format_of(h);
    return f != SurfaceFormat::Unknown ? f : format_of_resource(resource_from_id(h.id));
}
SurfaceFormat D3D9FrameContextImpl::format_of(TextureHandle h) const noexcept {
    const SurfaceFormat f = ctx_.store().format_of(h);
    return f != SurfaceFormat::Unknown ? f : format_of_resource(resource_from_id(h.id));
}

Result<TextureHandle> D3D9FrameContextImpl::create_texture(const TextureDesc& d) {
    return ctx_.store().create_texture(ContextImpl::alloc_id(), d);
}
Result<RenderTargetHandle> D3D9FrameContextImpl::create_render_target(const RenderTargetDesc& d) {
    return ctx_.store().create_render_target(ContextImpl::alloc_id(), d);
}
Result<DepthBufferHandle> D3D9FrameContextImpl::create_depth_buffer(const DepthBufferDesc& d) {
    return ctx_.store().create_depth_buffer(ContextImpl::alloc_id(), d);
}
void D3D9FrameContextImpl::destroy(TextureHandle h)      noexcept { ctx_.store().destroy(h); }
void D3D9FrameContextImpl::destroy(RenderTargetHandle h) noexcept { ctx_.store().destroy(h); }
void D3D9FrameContextImpl::destroy(DepthBufferHandle h)  noexcept { ctx_.store().destroy(h); }

// The replacement is set past the hooks for the draw being reported, and the
// draw thunk puts the game's shader back after the draw.
Result<void> D3D9FrameContextImpl::bind_effect(const Effect& e) {
    if (!state_) {
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D9: bind_effect swaps the shaders of the draw being reported, so it works only inside an on_draw "
            "callback.");
    }
    if (is_valid(e.gs.handle) || is_valid(e.hs.handle) || is_valid(e.ds.handle) || is_valid(e.cs.handle))
        return gfx_err<void>(ShaderError::Unsupported, "D3D9: only the vertex and pixel stages exist on this API.");
    const bool has_vs = is_valid(e.vs.handle), has_ps = is_valid(e.ps.handle);
    if (!has_vs && !has_ps) return gfx_err<void>(ShaderError::InvalidHandle, "D3D9: the Effect has no shader.");

    auto& store = ctx_.store();
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9*  ps = nullptr;
    std::string why;
    bool first_failure = false;
    if (has_vs && has_ps) {
        // Both replaced: the pair is the caller's, as compiled.
        vs = store.vertex_shader(e.vs.handle);
        ps = store.pixel_shader(e.ps.handle);
        if (!vs || !ps) why = "the Effect names a shader this context did not issue, or one already destroyed";
    } else if (has_ps) {
        ps = store.pixel_shader_for(e.ps.handle, state_->vs_major >= 3, why, first_failure);
    } else {
        vs = store.vertex_shader_for(e.vs.handle, state_->ps_major >= 3, why, first_failure);
    }
    if ((has_vs && !vs) || (has_ps && !ps)) {
        const std::string msg = "D3D9: the replacement cannot run with this draw's shaders: " + why + ".";
        if (first_failure) ctx_.log(LogLevel::Error, msg);
        return gfx_err<void>(ShaderError::Unsupported, msg);
    }
    IDirect3DDevice9* dev = ctx_.device();
    if (vs) { set_vs_unreported(dev, vs); state_->vs_swapped = true; }
    if (ps) { set_ps_unreported(dev, ps); state_->ps_swapped = true; }
    return {};
}

void D3D9FrameContextImpl::unbind_effect() noexcept {
    if (state_) restore_shaders(ctx_.device(), *state_);
}

// Direct3D 9 has no constant buffer objects: constants are numbered registers
// set on the device, and no ConstantBufferHandle is ever issued for one.
Result<void> D3D9FrameContextImpl::update_constant_buffer(ConstantBufferHandle, const void*, uint32_t) {
    return gfx_err<void>(ShaderError::Unsupported,
        "D3D9: there are no constant buffers on this API; shader constants are registers. Set them with "
        "SetVertexShaderConstantF / SetPixelShaderConstantF on native_device().");
}

Result<ReadbackData> D3D9FrameContextImpl::readback(TextureHandle h)      { return ctx_.store().readback(h); }
Result<ReadbackData> D3D9FrameContextImpl::readback(RenderTargetHandle h) { return ctx_.store().readback(h); }
Result<ReadbackData> D3D9FrameContextImpl::readback(DepthBufferHandle h)  { return ctx_.store().readback(h); }

// All six land on one store call. Only vertex and index buffers exist on D3D9,
// so the other four handle types are never issued here.
Result<BufferData> D3D9FrameContextImpl::readback(VertexBufferHandle h, uint64_t o, uint64_t n)     { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D9FrameContextImpl::readback(IndexBufferHandle h, uint64_t o, uint64_t n)      { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D9FrameContextImpl::readback(ConstantBufferHandle h, uint64_t o, uint64_t n)   { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D9FrameContextImpl::readback(StructuredBufferHandle h, uint64_t o, uint64_t n) { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D9FrameContextImpl::readback(RawBufferHandle h, uint64_t o, uint64_t n)        { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D9FrameContextImpl::readback(UAVHandle h, uint64_t o, uint64_t n)              { return ctx_.store().read_buffer(h.id, o, n); }

Result<BufferData> D3D9FrameContextImpl::readback_at(uint64_t, uint64_t) {
    return gfx_err<BufferData>(ShaderError::Unsupported,
        "D3D9: buffers have no GPU virtual address on this API. Use the handle from a ResourceEvent with readback().");
}

bool D3D9FrameContextImpl::capture_active() const noexcept { return ctx_.capture_active(); }

Result<GBufferCapture> D3D9FrameContextImpl::latest_capture() {
    return ctx_.latest_gbuffer();
}

void* D3D9FrameContextImpl::native_device()  const noexcept { return ctx_.device(); }
void* D3D9FrameContextImpl::native_context() const noexcept { return ctx_.device(); }

void* D3D9FrameContextImpl::native_resource(RenderTargetHandle h) const noexcept {
    if (IDirect3DSurface9* s = ctx_.store().surface(h)) return s;
    return target_surface(h.id);
}
void* D3D9FrameContextImpl::native_resource(DepthBufferHandle h) const noexcept {
    if (IDirect3DSurface9* s = ctx_.store().surface(h)) return s;
    return target_surface(h.id);
}
void* D3D9FrameContextImpl::native_resource(TextureHandle h) const noexcept {
    if (IDirect3DTexture9* t = ctx_.store().texture(h)) return t;
    return resource_from_id(h.id);
}
void* D3D9FrameContextImpl::native_buffer(uint64_t id) const noexcept { return resource_from_id(id); }

}  // namespace vangfx::backend::d3d9
