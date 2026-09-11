#pragma once

/**
 * vangfx/d3d12/heap.hpp — Descriptor heap handles, CPU/GPU descriptor handles,
 * and root parameter binding for D3D12.
 *
 * D3D12 moved all resource views into explicitly managed descriptor heaps.
 * VanGFX wraps these with opaque handles and provides binding helpers that
 * mirror the D3D12 root signature model.
 *
 * Heap types correspond directly to D3D12_DESCRIPTOR_HEAP_TYPE:
 *   CbvSrvUav  → CBV / SRV / UAV  (shader-visible or CPU-only)
 *   Sampler    → sampler states    (shader-visible or CPU-only)
 *   Rtv        → render target views  (CPU-only)
 *   Dsv        → depth/stencil views  (CPU-only)
 */

#include <cstdint>

namespace vangfx::d3d12 {

// ─────────────────────────────────────────────────────────────────────────────
//  DescriptorHeapType
// ─────────────────────────────────────────────────────────────────────────────

enum class DescriptorHeapType : uint8_t {
    CbvSrvUav,   // D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
    Sampler,     // D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
    Rtv,         // D3D12_DESCRIPTOR_HEAP_TYPE_RTV
    Dsv,         // D3D12_DESCRIPTOR_HEAP_TYPE_DSV
};

// ─────────────────────────────────────────────────────────────────────────────
//  DescriptorHeapFlags
// ─────────────────────────────────────────────────────────────────────────────

enum class DescriptorHeapFlags : uint8_t {
    None          = 0,
    ShaderVisible = 1 << 0,  // D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                              // Required for CbvSrvUav and Sampler heaps
                              // that shaders read directly via GPU handle.
                              // Rtv and Dsv heaps cannot be shader-visible.
};

[[nodiscard]] inline DescriptorHeapFlags operator|(DescriptorHeapFlags a,
                                                   DescriptorHeapFlags b) noexcept {
    return static_cast<DescriptorHeapFlags>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
[[nodiscard]] inline bool operator&(DescriptorHeapFlags a,
                                    DescriptorHeapFlags b) noexcept {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opaque handles
// ─────────────────────────────────────────────────────────────────────────────

struct DescriptorHeapHandle { uint64_t id = 0; };

[[nodiscard]] inline bool is_valid(DescriptorHeapHandle h) noexcept {
    return h.id != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CpuDescriptorHandle — CPU-side descriptor address
//
//  Wraps D3D12_CPU_DESCRIPTOR_HANDLE.  Used to write descriptors via
//  ID3D12Device::Create*View() and as source/dest for CopyDescriptors().
// ─────────────────────────────────────────────────────────────────────────────

struct CpuDescriptorHandle {
    uint64_t ptr = 0;   // D3D12_CPU_DESCRIPTOR_HANDLE::ptr

    [[nodiscard]] bool valid() const noexcept { return ptr != 0; }

    // Advance to the Nth descriptor in a heap.
    // increment is ID3D12Device::GetDescriptorHandleIncrementSize() for the
    // appropriate heap type.
    [[nodiscard]] CpuDescriptorHandle offset(uint32_t index,
                                             uint32_t increment) const noexcept {
        return { ptr + static_cast<uint64_t>(index) * increment };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  GpuDescriptorHandle — GPU-side descriptor address (shader-visible heaps only)
//
//  Wraps D3D12_GPU_DESCRIPTOR_HANDLE.  Valid only for shader-visible heaps.
//  Pass to SetGraphicsRootDescriptorTable / SetComputeRootDescriptorTable.
// ─────────────────────────────────────────────────────────────────────────────

struct GpuDescriptorHandle {
    uint64_t ptr = 0;   // D3D12_GPU_DESCRIPTOR_HANDLE::ptr

    [[nodiscard]] bool valid() const noexcept { return ptr != 0; }

    [[nodiscard]] GpuDescriptorHandle offset(uint32_t index,
                                             uint32_t increment) const noexcept {
        return { ptr + static_cast<uint64_t>(index) * increment };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DescriptorHeapDesc
// ─────────────────────────────────────────────────────────────────────────────

struct DescriptorHeapDesc {
    DescriptorHeapType  type              = DescriptorHeapType::CbvSrvUav;
    DescriptorHeapFlags flags             = DescriptorHeapFlags::None;
    uint32_t            num_descriptors   = 0;
    uint32_t            node_mask         = 0;   // multi-GPU node mask; 0 = single GPU
};

// ─────────────────────────────────────────────────────────────────────────────
//  HeapRange — a contiguous range of descriptors in a heap
// ─────────────────────────────────────────────────────────────────────────────

struct HeapRange {
    DescriptorHeapHandle heap;
    uint32_t             base_index  = 0;  // first descriptor index
    uint32_t             count       = 0;  // number of descriptors in range
};

// ─────────────────────────────────────────────────────────────────────────────
//  RootParameterType — corresponds to D3D12_ROOT_PARAMETER_TYPE
// ─────────────────────────────────────────────────────────────────────────────

enum class RootParameterType : uint8_t {
    DescriptorTable,    // table of heap ranges (most flexible)
    Constants32,        // inline 32-bit constants (fastest — no indirection)
    Cbv,                // inline CBV descriptor (no heap, buffer GPU address)
    Srv,                // inline SRV descriptor
    Uav,                // inline UAV descriptor
};

// ─────────────────────────────────────────────────────────────────────────────
//  RootBinding — a single root signature slot binding observed at draw time
//
//  Filled in DrawEvent::root_bindings[] for each bound root parameter.
// ─────────────────────────────────────────────────────────────────────────────

struct RootBinding {
    uint32_t           parameter_index = 0;   // root parameter slot
    RootParameterType  type            = RootParameterType::DescriptorTable;

    union {
        GpuDescriptorHandle descriptor_table; // RootParameterType::DescriptorTable
        uint64_t            gpu_virtual_addr; // Cbv / Srv / Uav — raw VA
        uint32_t            constants[64];    // Constants32 — inline data
    };

    bool is_compute = false;  // false = graphics root, true = compute root
};

// ─────────────────────────────────────────────────────────────────────────────
//  RootSignatureHandle — opaque reference to a compiled root signature
// ─────────────────────────────────────────────────────────────────────────────

struct RootSignatureHandle { uint64_t id = 0; };

[[nodiscard]] inline bool is_valid(RootSignatureHandle h) noexcept {
    return h.id != 0;
}

} // namespace vangfx::d3d12
