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
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>

namespace vangfx::gpu {

using Microsoft::WRL::ComPtr;

// ── D3D12 Texture data ───────────────────────────────────────────────────────

struct D3D12TextureData final : TextureData {
    ComPtr<ID3D12Resource> resource;
    // CPU descriptor handles stored here; GPU-visible views are allocated
    // per-pass from a ring buffer in D3D12Device.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = {};   // null if not an RT
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = {};   // null if no SRV heap
    // The device that owns the descriptor heaps (weak ref)
    ID3D12Device* owner_dev = nullptr;
    bool owns_resource = true;

    void* native()  const noexcept override { return resource.Get(); }
    uint64_t gpu_va() const noexcept override {
        return resource ? resource->GetGPUVirtualAddress() : 0;
    }
};

// ── D3D12 Buffer data ────────────────────────────────────────────────────────

struct D3D12BufferData final : BufferData {
    ComPtr<ID3D12Resource> resource;         // upload heap (CPU-writable)
    uint8_t* mapped_ptr = nullptr;           // persistently mapped

    ~D3D12BufferData() override {
        if (resource && mapped_ptr) resource->Unmap(0, nullptr);
    }

    void* native()  const noexcept override { return resource.Get(); }
    uint64_t gpu_va() const noexcept override {
        return resource ? resource->GetGPUVirtualAddress() : 0;
    }
};

// ── D3D12 Pipeline data ──────────────────────────────────────────────────────

struct D3D12PipelineData final : PipelineData {
    ComPtr<ID3D12PipelineState>  pso;
    ComPtr<ID3D12RootSignature>  root_sig;
    bool is_compute = false;
};

// ── D3D12 Device ─────────────────────────────────────────────────────────────

class D3D12Pass;

class D3D12Device final : public Device {
public:
    D3D12Device(ID3D12Device* dev, ID3D12CommandQueue* queue);
    ~D3D12Device() override;

    [[nodiscard]] Api api() const override { return Api::D3D12; }

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

    // Called by D3D12Pass::end() to submit the command list.
    void submit_pass_list(ID3D12GraphicsCommandList* list);

    // Allocate an RTV descriptor (from a CPU-only heap).
    D3D12_CPU_DESCRIPTOR_HANDLE alloc_rtv();

    // Allocate SRV descriptors from the GPU-visible heap for one pass.
    // Returns the first GPU descriptor handle.
    D3D12_GPU_DESCRIPTOR_HANDLE alloc_pass_srvs(uint32_t count,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE* cpu_handles);

    ID3D12Device*       dev()   const { return dev_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    ID3D12DescriptorHeap* srv_heap() const { return srv_heap_.Get(); }

    [[nodiscard]] ComPtr<ID3D12RootSignature> make_root_sig(bool is_compute);

private:
    bool init_heaps();
    bool init_command_infra();

    ComPtr<ID3D12Device>        dev_;
    ComPtr<ID3D12CommandQueue>  queue_;

    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;     // CPU-only
    uint32_t rtv_next_       = 0;
    uint32_t rtv_desc_size_  = 0;

    ComPtr<ID3D12DescriptorHeap> srv_heap_;     // GPU-visible (CBV/SRV/UAV)
    uint32_t srv_next_       = 0;
    uint32_t srv_desc_size_  = 0;
    static constexpr uint32_t kSrvHeapSize = 1024;

    // Command infrastructure
    ComPtr<ID3D12CommandAllocator>    cmd_alloc_;
    ComPtr<ID3D12GraphicsCommandList> cmd_list_;
    ComPtr<ID3D12Fence>               fence_;
    uint64_t fence_value_ = 0;
    HANDLE   fence_event_ = nullptr;

    std::mutex submit_mutex_;
};

// ── D3D12 Pass ───────────────────────────────────────────────────────────────

class D3D12Pass final : public Pass {
public:
    D3D12Pass(D3D12Device* device,
              ID3D12CommandAllocator* alloc,
              ID3D12GraphicsCommandList* list);
    ~D3D12Pass() override;

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
    D3D12Device*                   device_   = nullptr;
    ID3D12CommandAllocator*        alloc_    = nullptr;
    ID3D12GraphicsCommandList*     list_     = nullptr;
    bool                           ended_    = false;

    // Params cbuffer (upload buffer, re-used per pass)
    ComPtr<ID3D12Resource>         params_buf_;
    uint8_t*                       params_ptr_ = nullptr;

    D3D12PipelineData*             pipeline_ = nullptr;

    // SRV staging: slots 0-3 filled via bind()
    static constexpr uint32_t kMaxSRVs = 8;
    D3D12_CPU_DESCRIPTOR_HANDLE    bound_srvs_[kMaxSRVs] = {};
    uint32_t                       srv_count_ = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE    rt_cpu_ = {};
    bool                           has_custom_rt_ = false;
};

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Device> make_d3d12_device(ID3D12Device* dev,
                                           ID3D12CommandQueue* queue);

} // namespace vangfx::gpu
