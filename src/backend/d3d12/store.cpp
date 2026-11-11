/**
 * src/backend/d3d12/store.cpp — D3D12ResourceStore.
 *
 * Resource creation, descriptor management, readback and constant buffers for
 * the D3D12 backend. Every one of these used to return NotInitialized from a
 * stub in context_impl.hpp marked "not impl".
 *
 * The readback path is the part worth reading twice. D3D12 has no synchronous
 * copy: the sequence is transition the source to COPY_SOURCE, record a
 * CopyTextureRegion into a buffer on a readback heap, transition it back,
 * close, execute on the queue, signal a fence, block until it fires, then map.
 * That last block is a full GPU stall and is why capture is something you turn
 * on for a few frames rather than leave running.
 */

#include "store.hpp"
#include "hub.hpp"
#include <format_map.hpp>

#include <algorithm>
#include <cstring>
#include <string>

namespace vangfx::backend::d3d12 {

namespace {

template <typename T>
void safe_release(T*& p) noexcept
{
    if (p) { p->Release(); p = nullptr; }
}

D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) noexcept
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type                 = type;
    p.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask     = 1;
    p.VisibleNodeMask      = 1;
    return p;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* res,
                                   D3D12_RESOURCE_STATES from,
                                   D3D12_RESOURCE_STATES to) noexcept
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type                    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags                   = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource    = res;
    b.Transition.StateBefore  = from;
    b.Transition.StateAfter   = to;
    b.Transition.Subresource  = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Lifetime
// ─────────────────────────────────────────────────────────────────────────────

bool D3D12ResourceStore::attach(ID3D12Device* device,
                                 ID3D12CommandQueue* queue) noexcept
{
    InternalScope internal_work;   // none of this is the application's
    device_ = device;
    queue_  = queue;
    if (!device_) return false;

    auto make_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count,
                         ID3D12DescriptorHeap** out, uint32_t& stride) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type           = type;
        d.NumDescriptors = count;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device_->CreateDescriptorHeap(&d, IID_PPV_ARGS(out)))) return false;
        stride = device_->GetDescriptorHandleIncrementSize(type);
        return true;
    };

    if (!make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, &srv_heap_, srv_stride_) ||
        !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,         kRtvHeapSize, &rtv_heap_, rtv_stride_) ||
        !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,         kDsvHeapSize, &dsv_heap_, dsv_stride_)) {
        release_all();
        return false;
    }

    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&copy_alloc_))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           copy_alloc_, nullptr,
                                           IID_PPV_ARGS(&copy_list_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        release_all();
        return false;
    }

    // A command list is created in the recording state; close it so the first
    // readback can Reset it like every subsequent one.
    copy_list_->Close();

    fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        release_all();
        return false;
    }

    return true;
}

void D3D12ResourceStore::release_all() noexcept
{
    std::lock_guard lk(mutex_);

    for (auto& [id, rec] : textures_)       if (rec.owned) safe_release(rec.res);
    for (auto& [id, rec] : render_targets_) if (rec.owned) safe_release(rec.res);
    for (auto& [id, rec] : depth_buffers_)  if (rec.owned) safe_release(rec.res);
    for (auto& [id, rec] : cbuffers_) {
        if (rec.res && rec.mapped) rec.res->Unmap(0, nullptr);
        safe_release(rec.res);
    }

    textures_.clear();
    render_targets_.clear();
    depth_buffers_.clear();
    cbuffers_.clear();
    buffers_.clear();
    va_index_.clear();
    uniform_cb_ = {};

    safe_release(readback_buf_);
    readback_capacity_ = 0;

    safe_release(copy_list_);
    safe_release(copy_alloc_);
    safe_release(fence_);
    if (fence_event_) { ::CloseHandle(fence_event_); fence_event_ = nullptr; }

    safe_release(srv_heap_);
    safe_release(rtv_heap_);
    safe_release(dsv_heap_);
    srv_next_ = rtv_next_ = dsv_next_ = 0;

    scratch_.clear();
    scratch_.shrink_to_fit();
}

bool D3D12ResourceStore::wait_idle(unsigned long timeout_ms) noexcept
{
    std::lock_guard copy_lk(copy_mutex_);
    if (!queue_ || !fence_ || !fence_event_) return false;
    InternalScope internal_work;
    const uint64_t target = ++fence_value_;
    if (FAILED(queue_->Signal(fence_, target))) return false;
    if (fence_->GetCompletedValue() >= target) return true;
    if (FAILED(fence_->SetEventOnCompletion(target, fence_event_))) return false;
    return ::WaitForSingleObject(fence_event_, timeout_ms) == WAIT_OBJECT_0;
}

uint32_t D3D12ResourceStore::alloc_slot(D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept
{
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        return (srv_next_ < kSrvHeapSize) ? srv_next_++ : UINT32_MAX;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        return (rtv_next_ < kRtvHeapSize) ? rtv_next_++ : UINT32_MAX;
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        return (dsv_next_ < kDsvHeapSize) ? dsv_next_++ : UINT32_MAX;
    default:
        return UINT32_MAX;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Creation
// ─────────────────────────────────────────────────────────────────────────────

Result<TextureHandle>
D3D12ResourceStore::create_texture(uint64_t id, const TextureDesc& d)
{
    if (!device_) {
        return gfx_err<TextureHandle>(ShaderError::NotInitialized, "D3D12: no device.");
    }
    InternalScope internal_work;
    if (d.width == 0 || d.height == 0) {
        return gfx_err<TextureHandle>(ShaderError::InvalidFormat,
            "D3D12: texture needs a non-zero width and height.");
    }

    const DXGI_FORMAT fmt = fmtmap::to_dxgi(d.format);
    if (fmt == DXGI_FORMAT_UNKNOWN) {
        return gfx_err<TextureHandle>(ShaderError::InvalidFormat,
            "D3D12: no DXGI equivalent for the requested SurfaceFormat.");
    }

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = (d.type == TextureType::Tex1D)
                        ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                        : (d.type == TextureType::TexVolume)
                        ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                        : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment        = d.d3d12_alignment;
    rd.Width            = d.width;
    rd.Height           = d.height;
    rd.DepthOrArraySize = (d.type == TextureType::TexVolume)
                        ? static_cast<UINT16>(std::max(1u, d.depth))
                        : static_cast<UINT16>((d.type == TextureType::TexCube)
                                              ? 6u : std::max(1u, d.array_size));
    rd.MipLevels        = static_cast<UINT16>(std::max(1u, d.mip_levels));
    rd.Format           = fmt;
    rd.SampleDesc.Count = std::max(1u, d.msaa_samples);
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (d.is_render_target) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (d.is_depth)         rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (d.is_uav)           rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (d.d3d12_deny_shader_resource)
        rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    if (d.d3d12_allow_simultaneous_access)
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    const auto heap = heap_props(static_cast<D3D12_HEAP_TYPE>(
        d.d3d12_heap_type == 1 ? D3D12_HEAP_TYPE_UPLOAD :
        d.d3d12_heap_type == 2 ? D3D12_HEAP_TYPE_READBACK :
                                 D3D12_HEAP_TYPE_DEFAULT));

    auto state = static_cast<D3D12_RESOURCE_STATES>(d.d3d12_initial_state);
    // An upload-heap resource must start in GENERIC_READ and a readback one in
    // COPY_DEST; the runtime rejects anything else, with a message about the
    // heap rather than about the state that was actually wrong.
    if (heap.Type == D3D12_HEAP_TYPE_UPLOAD)   state = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (heap.Type == D3D12_HEAP_TYPE_READBACK) state = D3D12_RESOURCE_STATE_COPY_DEST;

    // A depth resource cleared to something other than its clear value costs a
    // decompression on first use, so the optimised clear value is supplied.
    D3D12_CLEAR_VALUE clear{};
    const D3D12_CLEAR_VALUE* clear_ptr = nullptr;
    if (d.is_depth) {
        clear.Format               = fmtmap::dsv_format_for(fmt);
        clear.DepthStencil.Depth   = 1.0f;
        clear.DepthStencil.Stencil = 0;
        clear_ptr = &clear;
    } else if (d.is_render_target) {
        clear.Format = fmt;
        clear_ptr = &clear;
    }

    ResourceRec rec;
    rec.format = d.format;
    rec.width  = d.width;
    rec.height = d.height;
    rec.state  = state;
    rec.owned  = true;

    const HRESULT hr = device_->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &rd, state, clear_ptr, IID_PPV_ARGS(&rec.res));
    if (FAILED(hr) || !rec.res) {
        return gfx_err<TextureHandle>(ShaderError::OutOfMemory,
            "D3D12: CreateCommittedResource failed. Check the format supports "
            "the requested flags with CheckFeatureSupport(FORMAT_SUPPORT).");
    }

    std::lock_guard lk(mutex_);

    // Views for whatever it was asked to be.
    if (!d.d3d12_deny_shader_resource && !d.is_depth) {
        rec.srv_slot = alloc_slot(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (rec.srv_slot != UINT32_MAX) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format                  = fmtmap::srv_format_for(fmt);
            sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels     = rd.MipLevels;
            auto h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(rec.srv_slot) * srv_stride_;
            device_->CreateShaderResourceView(rec.res, &sd, h);
        }
    }
    if (d.is_render_target) {
        rec.rtv_slot = alloc_slot(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (rec.rtv_slot != UINT32_MAX) {
            auto h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(rec.rtv_slot) * rtv_stride_;
            device_->CreateRenderTargetView(rec.res, nullptr, h);
        }
    }
    if (d.is_depth) {
        rec.dsv_slot = alloc_slot(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        if (rec.dsv_slot != UINT32_MAX) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
            dd.Format        = fmtmap::dsv_format_for(fmt);
            dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            auto h = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(rec.dsv_slot) * dsv_stride_;
            device_->CreateDepthStencilView(rec.res, &dd, h);
        }
    }

    textures_[id] = rec;
    return TextureHandle{ id };
}

Result<RenderTargetHandle>
D3D12ResourceStore::create_render_target(uint64_t id, const RenderTargetDesc& d)
{
    TextureDesc td;
    td.format             = d.format;
    td.width              = d.width;
    td.height             = d.height;
    td.msaa_samples       = d.msaa_samples;
    td.mip_levels         = 1;
    td.is_render_target   = true;
    td.d3d12_initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

    auto tex = create_texture(id, td);
    if (!tex) return gfx_err<RenderTargetHandle>(tex.error().code, tex.error().message);

    std::lock_guard lk(mutex_);
    ResourceRec rec = textures_[id];
    rec.owned = false;                  // the TextureRec owns the resource
    render_targets_[id] = rec;
    return RenderTargetHandle{ id };
}

Result<DepthBufferHandle>
D3D12ResourceStore::create_depth_buffer(uint64_t id, const DepthBufferDesc& d)
{
    TextureDesc td;
    td.format              = d.format;
    td.width               = d.width;
    td.height              = d.height;
    td.msaa_samples        = d.msaa_samples;
    td.mip_levels          = 1;
    td.is_depth            = true;
    td.d3d12_initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    auto tex = create_texture(id, td);
    if (!tex) return gfx_err<DepthBufferHandle>(tex.error().code, tex.error().message);

    std::lock_guard lk(mutex_);
    ResourceRec rec = textures_[id];
    rec.owned = false;
    depth_buffers_[id] = rec;
    return DepthBufferHandle{ id };
}

// ─────────────────────────────────────────────────────────────────────────────
//  Adoption
// ─────────────────────────────────────────────────────────────────────────────

void D3D12ResourceStore::adopt_render_target(uint64_t id, ID3D12Resource* res,
                                              D3D12_RESOURCE_STATES state) noexcept
{
    if (!res) return;
    std::lock_guard lk(mutex_);
    if (render_targets_.contains(id)) return;

    ResourceRec rec;
    rec.res   = res;
    rec.state = state;
    rec.owned = false;      // belongs to the game

    const auto d = res->GetDesc();
    rec.width  = static_cast<uint32_t>(d.Width);
    rec.height = d.Height;
    rec.format = fmtmap::from_dxgi(d.Format);

    render_targets_[id] = rec;
}

void D3D12ResourceStore::adopt_depth_buffer(uint64_t id, ID3D12Resource* res,
                                             D3D12_RESOURCE_STATES state) noexcept
{
    if (!res) return;
    std::lock_guard lk(mutex_);
    if (depth_buffers_.contains(id)) return;

    ResourceRec rec;
    rec.res   = res;
    rec.state = state;
    rec.owned = false;

    const auto d = res->GetDesc();
    rec.width  = static_cast<uint32_t>(d.Width);
    rec.height = d.Height;
    rec.format = fmtmap::from_dxgi(d.Format);

    depth_buffers_[id] = rec;
}

void D3D12ResourceStore::adopt_buffer(uint64_t id, ID3D12Resource* res,
                                       uint64_t size, uint64_t gpu_va) noexcept
{
    if (!res || size == 0) return;
    std::lock_guard lk(mutex_);
    if (buffers_.contains(id)) return;

    BufferRec rec;
    rec.res    = res;
    rec.size   = size;
    rec.gpu_va = gpu_va;
    buffers_[id] = rec;

    if (gpu_va) va_index_[gpu_va] = id;
}

void D3D12ResourceStore::forget(uint64_t id) noexcept
{
    std::lock_guard lk(mutex_);
    // Targets adopted from the application are borrowed pointers under the
    // resource's id; VanGFX's own (owned) ones are never forgotten this way.
    if (auto t = render_targets_.find(id); t != render_targets_.end() && !t->second.owned && !textures_.contains(id))
        render_targets_.erase(t);
    if (auto t = depth_buffers_.find(id); t != depth_buffers_.end() && !t->second.owned && !textures_.contains(id))
        depth_buffers_.erase(t);

    const auto it = buffers_.find(id);
    if (it == buffers_.end()) return;

    // Erase by the recorded address rather than by scanning for the id: a
    // released buffer's address is reused by the next allocation, and the index
    // entry has to go before that happens or a later lookup resolves to a
    // buffer that no longer exists. Guarded because that reuse may already have
    // overwritten the slot with a live buffer, which must not be erased.
    if (it->second.gpu_va) {
        const auto vit = va_index_.find(it->second.gpu_va);
        if (vit != va_index_.end() && vit->second == id) va_index_.erase(vit);
    }
    buffers_.erase(it);
}

BufferRef D3D12ResourceStore::locate(uint64_t gpu_va) const noexcept
{
    if (!gpu_va) return {};
    std::lock_guard lk(mutex_);

    // The last range starting at or before the address; then check it actually
    // reaches. upper_bound gives the first range starting strictly after, so
    // the one before it is the only candidate -- ranges do not overlap.
    auto it = va_index_.upper_bound(gpu_va);
    if (it == va_index_.begin()) return {};
    --it;

    const auto bit = buffers_.find(it->second);
    if (bit == buffers_.end()) return {};

    const uint64_t base = it->first;
    const uint64_t off  = gpu_va - base;
    if (off >= bit->second.size) return {};   // past the end: a different buffer

    BufferRef ref;
    ref.id     = it->second;
    ref.offset = off;
    ref.size   = bit->second.size;
    return ref;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Destruction and lookup
// ─────────────────────────────────────────────────────────────────────────────

void D3D12ResourceStore::destroy(TextureHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = textures_.find(h.id); it != textures_.end()) {
        render_targets_.erase(h.id);
        depth_buffers_.erase(h.id);
        if (it->second.owned) safe_release(it->second.res);
        textures_.erase(it);
    }
}

void D3D12ResourceStore::destroy(RenderTargetHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = render_targets_.find(h.id); it != render_targets_.end()) {
        if (it->second.owned) safe_release(it->second.res);
        render_targets_.erase(it);
    }
}

void D3D12ResourceStore::destroy(DepthBufferHandle h) noexcept
{
    std::lock_guard lk(mutex_);
    if (auto it = depth_buffers_.find(h.id); it != depth_buffers_.end()) {
        if (it->second.owned) safe_release(it->second.res);
        depth_buffers_.erase(it);
    }
}

SurfaceFormat D3D12ResourceStore::format_of(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    return (it != textures_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

SurfaceFormat D3D12ResourceStore::format_of(RenderTargetHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = render_targets_.find(h.id);
    return (it != render_targets_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

SurfaceFormat D3D12ResourceStore::format_of(DepthBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = depth_buffers_.find(h.id);
    return (it != depth_buffers_.end()) ? it->second.format : SurfaceFormat::Unknown;
}

ID3D12Resource* D3D12ResourceStore::resource(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    return (it != textures_.end()) ? it->second.res : nullptr;
}

ID3D12Resource* D3D12ResourceStore::resource(RenderTargetHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = render_targets_.find(h.id);
    return (it != render_targets_.end()) ? it->second.res : nullptr;
}

ID3D12Resource* D3D12ResourceStore::resource(DepthBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = depth_buffers_.find(h.id);
    return (it != depth_buffers_.end()) ? it->second.res : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE
D3D12ResourceStore::srv_of(TextureHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = textures_.find(h.id);
    if (it == textures_.end() || it->second.srv_slot == UINT32_MAX || !srv_heap_)
        return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    auto cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(it->second.srv_slot) * srv_stride_;
    return cpu;
}

D3D12_CPU_DESCRIPTOR_HANDLE
D3D12ResourceStore::rtv_of(RenderTargetHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = render_targets_.find(h.id);
    if (it == render_targets_.end() || it->second.rtv_slot == UINT32_MAX || !rtv_heap_)
        return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    auto cpu = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(it->second.rtv_slot) * rtv_stride_;
    return cpu;
}

D3D12_CPU_DESCRIPTOR_HANDLE
D3D12ResourceStore::dsv_of(DepthBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = depth_buffers_.find(h.id);
    if (it == depth_buffers_.end() || it->second.dsv_slot == UINT32_MAX || !dsv_heap_)
        return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    auto cpu = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(it->second.dsv_slot) * dsv_stride_;
    return cpu;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Readback
// ─────────────────────────────────────────────────────────────────────────────

Result<ReadbackData> D3D12ResourceStore::read_resource(const ResourceRec& rec)
{
    // One readback at a time: they share the copy list, the readback buffer and
    // the fence, and callbacks run on every thread recording a command list.
    std::lock_guard copy_lk(copy_mutex_);
    InternalScope internal_work;

    if (!rec.res || !device_ || !queue_ || !copy_list_ || !fence_) {
        return gfx_err<ReadbackData>(ShaderError::NotInitialized,
            "D3D12: the context has no copy machinery; readback is unavailable.");
    }

    const auto desc = rec.res->GetDesc();

    // The runtime decides the layout of the destination -- alignment and row
    // pitch are not the caller's to choose, and getting them wrong produces a
    // copy that succeeds and returns skewed pixels.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT   rows = 0;
    UINT64 row_bytes = 0, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows,
                                   &row_bytes, &total);
    if (total == 0) {
        return gfx_err<ReadbackData>(ShaderError::InvalidFormat,
            "D3D12: the resource has no copyable footprint -- a planar or "
            "video format this path does not handle.");
    }

    // One readback buffer, grown as needed and kept for next time.
    if (total > readback_capacity_) {
        safe_release(readback_buf_);

        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = total;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const auto heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback_buf_)))) {
            readback_capacity_ = 0;
            return gfx_err<ReadbackData>(ShaderError::OutOfMemory,
                "D3D12: could not create the readback buffer.");
        }
        readback_capacity_ = total;
    }

    // Record: transition to COPY_SOURCE, copy, transition back.
    if (FAILED(copy_alloc_->Reset()) ||
        FAILED(copy_list_->Reset(copy_alloc_, nullptr))) {
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
            "D3D12: could not reset the copy command list. A previous readback "
            "may still be in flight.");
    }

    // An application's resource is in whatever state the queue has reached
    // (hub.hpp); one of VanGFX's own is where the store leaves it.
    D3D12_RESOURCE_STATES state = rec.state;
    if (!rec.owned) {
        const TrackedState t = state_of(rec.res);
        if (t.known && t.layout) {
            copy_list_->Close();
            return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
                "D3D12: the resource is moved by enhanced barriers, which readback does not record; a capture "
                "(CaptureSession) copies it.");
        }
        if (t.known) state = static_cast<D3D12_RESOURCE_STATES>(t.value);
    }
    const bool needs_transition = (state & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    if (needs_transition) {
        auto b = transition(rec.res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        b.Transition.Subresource = 0;
        copy_list_->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = readback_buf_;
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = rec.res;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    copy_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (needs_transition) {
        auto b = transition(rec.res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        b.Transition.Subresource = 0;
        copy_list_->ResourceBarrier(1, &b);
    }

    if (FAILED(copy_list_->Close())) {
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
            "D3D12: the copy command list would not close.");
    }

    ID3D12CommandList* lists[] = { copy_list_ };
    queue_->ExecuteCommandLists(1, lists);

    // Block until the copy has actually happened. This is a full stall, which
    // is why capture is meant to run for a handful of frames rather than
    // continuously.
    const uint64_t target = ++fence_value_;
    if (FAILED(queue_->Signal(fence_, target))) {
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
            "D3D12: could not signal the readback fence.");
    }
    if (fence_->GetCompletedValue() < target) {
        if (FAILED(fence_->SetEventOnCompletion(target, fence_event_))) {
            return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
                "D3D12: could not wait on the readback fence.");
        }
        ::WaitForSingleObject(fence_event_, INFINITE);
    }

    // Map and repack to a tight pitch. The footprint's is aligned to 256 bytes.
    void* mapped = nullptr;
    D3D12_RANGE range{ 0, static_cast<SIZE_T>(total) };
    if (FAILED(readback_buf_->Map(0, &range, &mapped)) || !mapped) {
        return gfx_err<ReadbackData>(ShaderError::ReadbackFailed,
            "D3D12: Map failed on the readback buffer.");
    }

    const uint32_t tight = static_cast<uint32_t>(row_bytes);
    scratch_.resize(static_cast<size_t>(tight) * rows);
    const auto* p = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < rows; ++y) {
        std::memcpy(scratch_.data() + static_cast<size_t>(y) * tight,
                    p + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    tight);
    }

    const D3D12_RANGE written{ 0, 0 };   // nothing was written back
    readback_buf_->Unmap(0, &written);

    ReadbackData out;
    out.pixels    = std::span<const uint8_t>(scratch_.data(), scratch_.size());
    out.width     = rec.width  ? rec.width  : static_cast<uint32_t>(desc.Width);
    out.height    = rec.height ? rec.height : desc.Height;
    out.format    = fmtmap::from_dxgi(desc.Format);
    out.row_pitch = tight;
    return out;
}

Result<ReadbackData> D3D12ResourceStore::readback(TextureHandle h)
{
    ResourceRec rec;
    {
        std::lock_guard lk(mutex_);
        const auto it = textures_.find(h.id);
        if (it == textures_.end()) {
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D12: readback of a texture this context does not know.");
        }
        rec = it->second;
    }
    return read_resource(rec);
}

Result<ReadbackData> D3D12ResourceStore::readback(RenderTargetHandle h)
{
    ResourceRec rec;
    {
        std::lock_guard lk(mutex_);
        const auto it = render_targets_.find(h.id);
        if (it == render_targets_.end()) {
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D12: readback of a render target this context does not know. "
                "Targets are registered as they are bound, so this usually means "
                "the handle came from a different context.");
        }
        rec = it->second;
    }
    return read_resource(rec);
}

Result<ReadbackData> D3D12ResourceStore::readback(DepthBufferHandle h)
{
    ResourceRec rec;
    {
        std::lock_guard lk(mutex_);
        const auto it = depth_buffers_.find(h.id);
        if (it == depth_buffers_.end()) {
            return gfx_err<ReadbackData>(ShaderError::InvalidHandle,
                "D3D12: readback of a depth buffer this context does not know.");
        }
        rec = it->second;
    }
    return read_resource(rec);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constant buffers
// ─────────────────────────────────────────────────────────────────────────────

ConstantBufferHandle D3D12ResourceStore::uniform_buffer(uint64_t id_if_new)
{
    std::lock_guard lk(mutex_);
    if (is_valid(uniform_cb_)) return uniform_cb_;
    if (!device_) return {};
    InternalScope internal_work;

    // 256 bytes, which is both the size of the CbufferData Uniforms uploads and
    // the alignment D3D12 requires of a constant buffer view.
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = 256;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const auto heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);

    CBufferRec rec;
    rec.size = 256;
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&rec.res))) || !rec.res) {
        return {};
    }

    // Left mapped for its lifetime. An upload heap is CPU-visible and there is
    // no cost to keeping the pointer; unmapping between writes only adds work.
    const D3D12_RANGE none{ 0, 0 };
    if (FAILED(rec.res->Map(0, &none, &rec.mapped))) {
        rec.res->Release();
        return {};
    }
    std::memset(rec.mapped, 0, rec.size);

    cbuffers_[id_if_new] = rec;
    uniform_cb_ = ConstantBufferHandle{ id_if_new };
    return uniform_cb_;
}

Result<void>
D3D12ResourceStore::update_constant_buffer(ConstantBufferHandle h,
                                            const void* data, uint32_t size)
{
    if (!data || size == 0) {
        return gfx_err<void>(ShaderError::InvalidFormat,
            "D3D12: constant buffer update with no data.");
    }

    std::lock_guard lk(mutex_);
    const auto it = cbuffers_.find(h.id);
    if (it == cbuffers_.end() || !it->second.mapped) {
        return gfx_err<void>(ShaderError::InvalidHandle,
            "D3D12: no such constant buffer. Obtain one from "
            "Context::uniform_buffer().");
    }
    if (size > it->second.size) {
        return gfx_err<void>(ShaderError::InvalidFormat,
            "D3D12: the update is " + std::to_string(size) + " bytes and the "
            "buffer holds " + std::to_string(it->second.size) + ".");
    }

    std::memcpy(it->second.mapped, data, size);
    return {};
}

D3D12_GPU_VIRTUAL_ADDRESS
D3D12ResourceStore::cbuffer_address(ConstantBufferHandle h) const noexcept
{
    std::lock_guard lk(mutex_);
    const auto it = cbuffers_.find(h.id);
    return (it != cbuffers_.end() && it->second.res)
         ? it->second.res->GetGPUVirtualAddress()
         : D3D12_GPU_VIRTUAL_ADDRESS{ 0 };
}

// ─────────────────────────────────────────────────────────────────────────────
//  Buffer readback
//
//  Simpler than the texture path, and deliberately so. A buffer has no
//  footprint to ask the runtime about, no row pitch to unpick and no subresource
//  to name -- it is a range of bytes, so this is CopyBufferRegion and a map.
//
//  NO BARRIER, which is the part that looks wrong and is not. D3D12 promotes a
//  buffer out of COMMON into whatever state its first use in a command list
//  needs, and decays it back to COMMON when the list is executed. Buffers always
//  decay -- the rule that keeps some textures in their promoted state does not
//  apply to them -- so at the top of a fresh command list every buffer the
//  application owns is in COMMON, and the copy below promotes it to COPY_SOURCE
//  by itself. Issuing a transition barrier here would mean naming the state it
//  is in, which is exactly the thing that cannot be known from outside.
//
//  The exception is a buffer the application explicitly barriered into a state
//  and left there, which does not decay; a UAV held in UNORDERED_ACCESS across
//  frames is the realistic case. Reading one of those trips the debug layer.
//  Vertex, index and constant buffers -- what anyone is here to read -- decay.
// ─────────────────────────────────────────────────────────────────────────────

Result<BufferData> D3D12ResourceStore::read_buffer(uint64_t id,
                                                    uint64_t offset,
                                                    uint64_t size)
{
    std::lock_guard copy_lk(copy_mutex_);
    InternalScope internal_work;
    ID3D12Resource* res      = nullptr;
    uint64_t        buf_size = 0;
    {
        std::lock_guard lk(mutex_);
        const auto it = buffers_.find(id);
        if (it == buffers_.end() || !it->second.res) {
            return gfx_err<BufferData>(ShaderError::InvalidHandle,
                "D3D12: readback of a buffer this context does not know. "
                "Buffers are registered as the application creates them, so a "
                "handle from before VanGFX attached is not one of them.");
        }
        res      = it->second.res;
        buf_size = it->second.size;
    }

    // Clamped rather than rejected: a caller asking for the first 256 bytes of
    // every buffer it sees should not have to special-case the short ones.
    if (offset >= buf_size) {
        return gfx_err<BufferData>(ShaderError::InvalidFormat,
            "D3D12: offset " + std::to_string(offset) + " is past the end of a "
            + std::to_string(buf_size) + "-byte buffer.");
    }
    const uint64_t avail = buf_size - offset;
    if (size == 0 || size > avail) size = avail;

    // A buffer on an UPLOAD or READBACK heap is already CPU-visible, so the
    // whole copy-and-stall is pointless -- map it where it lies. This is the
    // common case for a constant buffer, which is written every frame and
    // therefore lives on an upload heap.
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS      hf{};
    if (SUCCEEDED(res->GetHeapProperties(&hp, &hf)) &&
        (hp.Type == D3D12_HEAP_TYPE_UPLOAD || hp.Type == D3D12_HEAP_TYPE_READBACK)) {

        void*        mapped = nullptr;
        D3D12_RANGE  range{ static_cast<SIZE_T>(offset),
                            static_cast<SIZE_T>(offset + size) };
        if (FAILED(res->Map(0, &range, &mapped)) || !mapped) {
            return gfx_err<BufferData>(ShaderError::ReadbackFailed,
                "D3D12: Map failed on a CPU-visible buffer.");
        }
        scratch_.resize(static_cast<size_t>(size));
        std::memcpy(scratch_.data(),
                    static_cast<const uint8_t*>(mapped) + offset,
                    static_cast<size_t>(size));

        const D3D12_RANGE written{ 0, 0 };
        res->Unmap(0, &written);

        BufferData out;
        out.bytes       = std::span<const uint8_t>(scratch_.data(), scratch_.size());
        out.offset      = offset;
        out.buffer_size = buf_size;
        return out;
    }

    // Default heap: copy to the readback buffer and wait for it.
    if (!device_ || !queue_ || !copy_list_ || !fence_) {
        return gfx_err<BufferData>(ShaderError::NotInitialized,
            "D3D12: the context has no copy machinery; readback is unavailable.");
    }

    if (size > readback_capacity_) {
        safe_release(readback_buf_);

        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = size;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        const auto heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(device_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback_buf_)))) {
            readback_capacity_ = 0;
            return gfx_err<BufferData>(ShaderError::OutOfMemory,
                "D3D12: could not create the readback buffer.");
        }
        readback_capacity_ = size;
    }

    if (FAILED(copy_alloc_->Reset()) ||
        FAILED(copy_list_->Reset(copy_alloc_, nullptr))) {
        return gfx_err<BufferData>(ShaderError::ReadbackFailed,
            "D3D12: could not reset the copy command list. A previous readback "
            "may still be in flight.");
    }

    copy_list_->CopyBufferRegion(readback_buf_, 0, res, offset, size);

    if (FAILED(copy_list_->Close())) {
        return gfx_err<BufferData>(ShaderError::ReadbackFailed,
            "D3D12: the copy command list would not close.");
    }

    ID3D12CommandList* lists[] = { copy_list_ };
    queue_->ExecuteCommandLists(1, lists);

    const uint64_t target = ++fence_value_;
    if (FAILED(queue_->Signal(fence_, target))) {
        return gfx_err<BufferData>(ShaderError::ReadbackFailed,
            "D3D12: could not signal the readback fence.");
    }
    if (fence_->GetCompletedValue() < target) {
        if (FAILED(fence_->SetEventOnCompletion(target, fence_event_))) {
            return gfx_err<BufferData>(ShaderError::ReadbackFailed,
                "D3D12: could not wait on the readback fence.");
        }
        ::WaitForSingleObject(fence_event_, INFINITE);
    }

    void*       mapped = nullptr;
    D3D12_RANGE range{ 0, static_cast<SIZE_T>(size) };
    if (FAILED(readback_buf_->Map(0, &range, &mapped)) || !mapped) {
        return gfx_err<BufferData>(ShaderError::ReadbackFailed,
            "D3D12: Map failed on the readback buffer.");
    }

    scratch_.resize(static_cast<size_t>(size));
    std::memcpy(scratch_.data(), mapped, static_cast<size_t>(size));

    const D3D12_RANGE written{ 0, 0 };
    readback_buf_->Unmap(0, &written);

    BufferData out;
    out.bytes       = std::span<const uint8_t>(scratch_.data(), scratch_.size());
    out.offset      = offset;
    out.buffer_size = buf_size;
    return out;
}

}  // namespace vangfx::backend::d3d12
