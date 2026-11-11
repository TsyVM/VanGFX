#pragma once

/**
 * src/backend/d3d12/store.hpp — the resources a D3D12 context owns on behalf of
 * callbacks.
 *
 * D3D12 asks for more than the other two backends, and all of it for the same
 * reason: nothing is implicit.
 *
 *   Descriptors are not part of a resource. A texture is an ID3D12Resource; the
 *   SRV, RTV or DSV that lets anything use it is a slot in a heap the
 *   application owns. So the store carries three heaps and hands out slots.
 *
 *   Copies are recorded, not called. There is no CopyResource on the device --
 *   readback means recording a CopyTextureRegion onto a command list, executing
 *   it on a queue, and waiting on a fence. The store owns an allocator, a list
 *   and a fence for exactly that.
 *
 *   Upload is manual. A constant buffer is a committed resource on an upload
 *   heap that stays mapped for its lifetime, which is what this does.
 *
 * Separate from the context header because D3D12ContextImpl holds one by value.
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
#include <d3d12.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d12 {

class D3D12ResourceStore {
public:
    D3D12ResourceStore() = default;
    ~D3D12ResourceStore() { release_all(); }

    D3D12ResourceStore(const D3D12ResourceStore&)            = delete;
    D3D12ResourceStore& operator=(const D3D12ResourceStore&) = delete;

    // Creates the descriptor heaps and the copy machinery. Returns false and
    // leaves the store inert if the device refuses any of it; every operation
    // then reports why rather than crashing.
    bool attach(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept;

    void release_all() noexcept;

    // Blocks until the queue has finished everything submitted to it so far,
    // or `timeout_ms` passes. False on timeout or when there is no queue.
    bool wait_idle(unsigned long timeout_ms) noexcept;

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

    [[nodiscard]] ID3D12Resource* resource(TextureHandle h)      const noexcept;
    [[nodiscard]] ID3D12Resource* resource(RenderTargetHandle h) const noexcept;
    [[nodiscard]] ID3D12Resource* resource(DepthBufferHandle h)  const noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE srv_of(TextureHandle h)      const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv_of(RenderTargetHandle h) const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE dsv_of(DepthBufferHandle h)  const noexcept;

    // Registers a resource the *game* owns under a handle an event reports, so
    // a callback can read back what is actually on screen rather than only what
    // VanGFX created. `state` is the state the resource is normally left in,
    // which readback needs in order to transition it and put it back.
    void adopt_render_target(uint64_t id, ID3D12Resource* res,
                             D3D12_RESOURCE_STATES state) noexcept;
    void adopt_depth_buffer (uint64_t id, ID3D12Resource* res,
                             D3D12_RESOURCE_STATES state) noexcept;

    // Every buffer the application creates, registered as resource.cpp sees it
    // go past. One map rather than six, because the six handle types are one id
    // space and D3D12 cannot tell a vertex buffer from a constant buffer at
    // creation anyway -- the flags do not say, so resource.cpp has to guess a
    // kind, and a caller holding the handle it was actually given should not be
    // turned away because that guess put the id in a different bucket.
    //
    // `gpu_va` is the buffer's GPU virtual address, or 0 when it has none
    // (a reserved/tiled buffer). It builds the index buffer_at() searches, so
    // a buffer registered with 0 can still be read by handle but will never be
    // found from a draw's vertex or index buffer view.
    void adopt_buffer(uint64_t id, ID3D12Resource* res, uint64_t size,
                      uint64_t gpu_va) noexcept;
    void forget(uint64_t id) noexcept;

    // The buffer whose address range covers `gpu_va`, or an empty ref.
    [[nodiscard]] BufferRef locate(uint64_t gpu_va) const noexcept;

    // ── Readback ─────────────────────────────────────────────────────────────

    Result<ReadbackData> readback(TextureHandle h);
    Result<ReadbackData> readback(RenderTargetHandle h);
    Result<ReadbackData> readback(DepthBufferHandle h);

    // Any of the six buffer handle types; they differ only in what the caller
    // knows the bytes mean. `size` of 0 reads to the end; both are clamped.
    Result<BufferData> read_buffer(uint64_t id, uint64_t offset, uint64_t size);

    // ── Constant buffers ─────────────────────────────────────────────────────

    [[nodiscard]] ConstantBufferHandle uniform_buffer(uint64_t id_if_new);
    Result<void> update_constant_buffer(ConstantBufferHandle h,
                                        const void* data, uint32_t size);

    // The GPU address of a constant buffer, for a caller binding it into a root
    // signature themselves.
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS cbuffer_address(ConstantBufferHandle h) const noexcept;

private:
    struct ResourceRec {
        ID3D12Resource*       res    = nullptr;
        SurfaceFormat         format = SurfaceFormat::Unknown;
        uint32_t              width  = 0;
        uint32_t              height = 0;
        D3D12_RESOURCE_STATES state  = D3D12_RESOURCE_STATE_COMMON;
        uint32_t              srv_slot = UINT32_MAX;
        uint32_t              rtv_slot = UINT32_MAX;
        uint32_t              dsv_slot = UINT32_MAX;
        bool                  owned  = false;
    };
    struct CBufferRec {
        ID3D12Resource* res     = nullptr;
        void*           mapped  = nullptr;
        uint32_t        size    = 0;
    };
    // A buffer the application owns. No state is recorded because none is
    // needed: see read_buffer's note on promotion and decay.
    struct BufferRec {
        ID3D12Resource* res    = nullptr;
        uint64_t        size   = 0;
        uint64_t        gpu_va = 0;   // 0 = not addressable (reserved buffer)
    };

    // Copies `src` into the readback buffer and maps it. Shared by all three
    // readback overloads.
    Result<ReadbackData> read_resource(const ResourceRec& rec);

    [[nodiscard]] uint32_t alloc_slot(D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept;

    ID3D12Device*       device_ = nullptr;
    ID3D12CommandQueue* queue_  = nullptr;

    // Descriptor heaps. Non-shader-visible: these exist so views can be created
    // at all, and a caller that wants one on the GPU copies it into their own
    // shader-visible heap with CopyDescriptorsSimple.
    ID3D12DescriptorHeap* srv_heap_ = nullptr;
    ID3D12DescriptorHeap* rtv_heap_ = nullptr;
    ID3D12DescriptorHeap* dsv_heap_ = nullptr;
    uint32_t srv_stride_ = 0, rtv_stride_ = 0, dsv_stride_ = 0;
    uint32_t srv_next_   = 0, rtv_next_   = 0, dsv_next_   = 0;

    static constexpr uint32_t kSrvHeapSize = 256;
    static constexpr uint32_t kRtvHeapSize = 64;
    static constexpr uint32_t kDsvHeapSize = 64;

    // Copy machinery for readback.
    ID3D12CommandAllocator*    copy_alloc_ = nullptr;
    ID3D12GraphicsCommandList* copy_list_  = nullptr;
    ID3D12Fence*               fence_      = nullptr;
    HANDLE                     fence_event_ = nullptr;
    uint64_t                   fence_value_ = 0;

    ID3D12Resource* readback_buf_      = nullptr;
    uint64_t        readback_capacity_ = 0;

    mutable std::mutex mutex_;
    std::mutex         copy_mutex_;   // one readback at a time (shared copy machinery)
    std::unordered_map<uint64_t, ResourceRec> textures_;
    std::unordered_map<uint64_t, ResourceRec> render_targets_;
    std::unordered_map<uint64_t, ResourceRec> depth_buffers_;
    std::unordered_map<uint64_t, CBufferRec>  cbuffers_;
    std::unordered_map<uint64_t, BufferRec>   buffers_;

    // GPU address range -> buffer id, keyed by the range's first address.
    // Ordered rather than hashed because the question it answers is "which
    // range contains this address", which needs a predecessor search: a game
    // suballocates its meshes out of a few large buffers, so a draw's vertex
    // buffer view almost never points at a buffer's first byte.
    std::map<uint64_t, uint64_t> va_index_;

    std::vector<uint8_t> scratch_;

    ConstantBufferHandle uniform_cb_{};
};

}  // namespace vangfx::backend::d3d12
