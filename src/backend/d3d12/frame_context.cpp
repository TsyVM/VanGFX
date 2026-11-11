/**
 * src/backend/d3d12/frame_context.cpp — D3D12FrameContextImpl.
 *
 * Resource work forwards to the context's D3D12ResourceStore. Surfaces and
 * pipeline state come from the command list the callback was raised on — D3D12
 * records lists on many threads at once, so "the current render target" means
 * something only per list — and, for events with no list (present, queue,
 * creation), from the last render-target bind on any list.
 */

#include "d3d12_impl.hpp"
#include "effect_bridge.hpp"

namespace vangfx::backend::d3d12 {

FrameSurfaces D3D12FrameContextImpl::surfaces() const noexcept {
    return state_ ? state_->surfaces : ctx_.last_surfaces();
}

PipelineState D3D12FrameContextImpl::pipeline() const noexcept {
    return state_ ? state_->pipeline : PipelineState{};
}

SurfaceFormat D3D12FrameContextImpl::format_of(RenderTargetHandle h) const noexcept { return ctx_.store().format_of(h); }
SurfaceFormat D3D12FrameContextImpl::format_of(DepthBufferHandle h)  const noexcept { return ctx_.store().format_of(h); }
SurfaceFormat D3D12FrameContextImpl::format_of(TextureHandle h)      const noexcept { return ctx_.store().format_of(h); }

Result<TextureHandle> D3D12FrameContextImpl::create_texture(const TextureDesc& d) {
    return ctx_.store().create_texture(ContextImpl::alloc_id(), d);
}
Result<RenderTargetHandle> D3D12FrameContextImpl::create_render_target(const RenderTargetDesc& d) {
    return ctx_.store().create_render_target(ContextImpl::alloc_id(), d);
}
Result<DepthBufferHandle> D3D12FrameContextImpl::create_depth_buffer(const DepthBufferDesc& d) {
    return ctx_.store().create_depth_buffer(ContextImpl::alloc_id(), d);
}

void D3D12FrameContextImpl::destroy(TextureHandle h)      noexcept { ctx_.store().destroy(h); }
void D3D12FrameContextImpl::destroy(RenderTargetHandle h) noexcept { ctx_.store().destroy(h); }
void D3D12FrameContextImpl::destroy(DepthBufferHandle h)  noexcept { ctx_.store().destroy(h); }

// The pipeline bound for this draw is swapped for one running the Effect's
// shaders, and put back after the draw (effect_bridge.hpp).
Result<void> D3D12FrameContextImpl::bind_effect(const Effect& e) { return effect_bridge::bind(ctx_, *this, e); }
void         D3D12FrameContextImpl::unbind_effect() noexcept    { effect_bridge::unbind(*this); }

Result<void> D3D12FrameContextImpl::update_constant_buffer(ConstantBufferHandle h, const void* data, uint32_t size) {
    return ctx_.store().update_constant_buffer(h, data, size);
}

Result<ReadbackData> D3D12FrameContextImpl::readback(TextureHandle h)      { return ctx_.store().readback(h); }
Result<ReadbackData> D3D12FrameContextImpl::readback(RenderTargetHandle h) { return ctx_.store().readback(h); }
Result<ReadbackData> D3D12FrameContextImpl::readback(DepthBufferHandle h)  { return ctx_.store().readback(h); }

// The six buffer handle types are one id space; they stay separate in the
// interface so a caller never has to launder one type into another.
Result<BufferData> D3D12FrameContextImpl::readback(VertexBufferHandle h, uint64_t o, uint64_t n)     { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D12FrameContextImpl::readback(IndexBufferHandle h, uint64_t o, uint64_t n)      { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D12FrameContextImpl::readback(ConstantBufferHandle h, uint64_t o, uint64_t n)   { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D12FrameContextImpl::readback(StructuredBufferHandle h, uint64_t o, uint64_t n) { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D12FrameContextImpl::readback(RawBufferHandle h, uint64_t o, uint64_t n)        { return ctx_.store().read_buffer(h.id, o, n); }
Result<BufferData> D3D12FrameContextImpl::readback(UAVHandle h, uint64_t o, uint64_t n)              { return ctx_.store().read_buffer(h.id, o, n); }

BufferRef D3D12FrameContextImpl::buffer_at(uint64_t gpu_virtual_address) const noexcept {
    return ctx_.store().locate(gpu_virtual_address);
}

Result<BufferData> D3D12FrameContextImpl::readback_at(uint64_t gpu_virtual_address, uint64_t size) {
    const BufferRef ref = ctx_.store().locate(gpu_virtual_address);
    if (!ref) {
        return gfx_err<BufferData>(ShaderError::InvalidHandle,
            "D3D12: no registered buffer covers that GPU virtual address. It may belong to memory "
            "the application allocated before VanGFX attached, or to a resource since released.");
    }
    return ctx_.store().read_buffer(ref.id, ref.offset, size);
}

bool D3D12FrameContextImpl::capture_active() const noexcept { return ctx_.capture_active(); }

Result<GBufferCapture> D3D12FrameContextImpl::latest_capture() {
    return ctx_.latest_gbuffer();
}

void* D3D12FrameContextImpl::native_device() const noexcept { return ctx_.device(); }

void* D3D12FrameContextImpl::native_resource(RenderTargetHandle h) const noexcept {
    if (ID3D12Resource* r = ctx_.store().resource(h)) return r;
    return resource_from_id(h.id);
}
void* D3D12FrameContextImpl::native_resource(DepthBufferHandle h) const noexcept {
    if (ID3D12Resource* r = ctx_.store().resource(h)) return r;
    return resource_from_id(h.id);
}
void* D3D12FrameContextImpl::native_resource(TextureHandle h) const noexcept {
    if (ID3D12Resource* r = ctx_.store().resource(h)) return r;
    return resource_from_id(h.id);
}
void* D3D12FrameContextImpl::native_buffer(uint64_t id) const noexcept { return resource_from_id(id); }

}  // namespace vangfx::backend::d3d12
