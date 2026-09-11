#pragma once

/**
 * vangfx/d3d12/resource.hpp — D3D12 resource state machine, barrier types,
 * and placed / committed / reserved resource descriptors.
 *
 * D3D12 introduced explicit resource state tracking.  Every resource lives
 * in a D3D12_RESOURCE_STATE at any moment and must be transitioned with a
 * ResourceBarrier before a new usage is legal.  VanGFX surfaces this so
 * mods can inject barriers correctly without reaching into raw D3D12.
 *
 * Allocation model:
 *   Committed  — most common; device allocates a private heap for you.
 *   Placed     — you own the heap; resource is placed at an explicit offset.
 *   Reserved   — sparse / tiled resources; no physical memory initially.
 */

#include <cstdint>

namespace vangfx::d3d12 {

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceState — D3D12_RESOURCE_STATES bit field
//
//  Mirrors the D3D12 enum exactly so values can be cast directly.
// ─────────────────────────────────────────────────────────────────────────────

enum class ResourceState : uint32_t {
    Common                     = 0,
    VertexAndConstantBuffer    = 0x0001,
    IndexBuffer                = 0x0002,
    RenderTarget               = 0x0004,
    UnorderedAccess            = 0x0008,
    DepthWrite                 = 0x0010,
    DepthRead                  = 0x0020,
    NonPixelShaderResource     = 0x0040,
    PixelShaderResource        = 0x0080,
    StreamOut                  = 0x0100,
    IndirectArgument           = 0x0200,
    CopyDest                   = 0x0400,
    CopySource                 = 0x0800,
    ResolveDest                = 0x1000,
    ResolveSource              = 0x2000,
    RaytracingAccelStructure   = 0x400000,
    ShadingRateSource          = 0x1000000,
    GenericRead                = VertexAndConstantBuffer | IndexBuffer
                               | NonPixelShaderResource | PixelShaderResource
                               | IndirectArgument | CopySource,
    AllShaderResource          = NonPixelShaderResource | PixelShaderResource,
    Present                    = 0,              // alias for Common on swap chains
    Predication                = IndirectArgument,
    VideoDecodeRead            = 0x00010000,
    VideoDecodeWrite           = 0x00020000,
    VideoProcessRead           = 0x00040000,
    VideoProcessWrite          = 0x00080000,
    VideoEncodeRead            = 0x00200000,
    VideoEncodeWrite           = 0x00800000,
};

[[nodiscard]] inline ResourceState operator|(ResourceState a, ResourceState b) noexcept {
    return static_cast<ResourceState>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] inline ResourceState operator&(ResourceState a, ResourceState b) noexcept {
    return static_cast<ResourceState>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
[[nodiscard]] inline bool any(ResourceState s) noexcept {
    return static_cast<uint32_t>(s) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  HeapType — D3D12_HEAP_TYPE
// ─────────────────────────────────────────────────────────────────────────────

enum class HeapType : uint8_t {
    Default,    // GPU-only; fastest GPU access, no CPU access
    Upload,     // CPU-write / GPU-read; for constant / vertex upload buffers
    Readback,   // GPU-write / CPU-read; for readback operations
    Custom,     // explicit memory pool (requires L0/L1 page properties)
};

// ─────────────────────────────────────────────────────────────────────────────
//  HeapFlags — D3D12_HEAP_FLAGS (subset relevant to VanGFX interception)
// ─────────────────────────────────────────────────────────────────────────────

enum class HeapFlags : uint32_t {
    None                    = 0,
    Shared                  = 0x0001,  // cross-process/adapter sharing
    DenyBuffers             = 0x0004,
    AllowDisplay            = 0x0008,  // swap-chain back buffers
    SharedCrossAdapter      = 0x0020,
    DenyRTDSTextures        = 0x0040,
    DenyNonRTDSTextures     = 0x0080,
    AllowSimultaneousAccess = 0x0100,  // multi-queue without explicit barriers
    CreateNotResident       = 0x0800,
    CreateNotZeroed         = 0x1000,
    AllowAllBuffersAndTextures = 0,    // default: no restrictions
};

[[nodiscard]] inline HeapFlags operator|(HeapFlags a, HeapFlags b) noexcept {
    return static_cast<HeapFlags>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] inline bool operator&(HeapFlags a, HeapFlags b) noexcept {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceDimension — D3D12_RESOURCE_DIMENSION
// ─────────────────────────────────────────────────────────────────────────────

enum class ResourceDimension : uint8_t {
    Unknown,
    Buffer,
    Texture1D,
    Texture2D,
    Texture3D,
};

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceFlags — D3D12_RESOURCE_FLAGS
// ─────────────────────────────────────────────────────────────────────────────

enum class ResourceFlags : uint16_t {
    None                        = 0,
    AllowRenderTarget           = 0x0001,
    AllowDepthStencil           = 0x0002,
    AllowUnorderedAccess        = 0x0004,
    DenyShaderResource          = 0x0008,
    AllowCrossAdapter           = 0x0010,
    AllowSimultaneousAccess     = 0x0020,
    VideoDecodeReferenceOnly    = 0x0040,
    VideoEncodeReferenceOnly    = 0x0080,
    RaytracingAccelStructure    = 0x0100,
};

[[nodiscard]] inline ResourceFlags operator|(ResourceFlags a, ResourceFlags b) noexcept {
    return static_cast<ResourceFlags>(
        static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
[[nodiscard]] inline bool operator&(ResourceFlags a, ResourceFlags b) noexcept {
    return (static_cast<uint16_t>(a) & static_cast<uint16_t>(b)) != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceAllocationKind — how the resource was allocated
// ─────────────────────────────────────────────────────────────────────────────

enum class ResourceAllocationKind : uint8_t {
    Committed,   // CreateCommittedResource — private implicit heap
    Placed,      // CreatePlacedResource    — explicit heap + offset
    Reserved,    // CreateReservedResource  — sparse/tiled, no backing memory
};

// ─────────────────────────────────────────────────────────────────────────────
//  D3D12ResourceDesc — extends vangfx TextureDesc / BufferDesc with DX12 fields
//
//  Observed by ResourceEvent callbacks when backend == D3D12.
// ─────────────────────────────────────────────────────────────────────────────

struct D3D12ResourceDesc {
    ResourceDimension      dimension        = ResourceDimension::Unknown;
    ResourceFlags          flags            = ResourceFlags::None;
    HeapType               heap_type        = HeapType::Default;
    HeapFlags              heap_flags       = HeapFlags::None;
    ResourceAllocationKind allocation_kind  = ResourceAllocationKind::Committed;
    ResourceState          initial_state    = ResourceState::Common;

    // For placed resources: the heap handle and offset within it.
    uint64_t               heap_id          = 0;   // vangfx heap opaque id
    uint64_t               heap_offset      = 0;   // bytes from heap start

    // D3D12_RESOURCE_DESC fields not in the base TextureDesc:
    uint64_t               alignment        = 0;   // 0 = default; 64KB/4MB
    uint16_t               layout           = 0;   // D3D12_TEXTURE_LAYOUT

    // GPU virtual address assigned by the runtime (valid after resource creation).
    uint64_t               gpu_virtual_address = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  BarrierType — D3D12_RESOURCE_BARRIER_TYPE
// ─────────────────────────────────────────────────────────────────────────────

enum class BarrierType : uint8_t {
    Transition,     // state transition (before/after states)
    Aliasing,       // two placed resources sharing heap memory
    Uav,            // UAV access hazard flush (no state change)
};

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceBarrierFlags — D3D12_RESOURCE_BARRIER_FLAGS
// ─────────────────────────────────────────────────────────────────────────────

enum class ResourceBarrierFlags : uint8_t {
    None           = 0,
    BeginOnly      = 0x01,   // split barrier begin
    EndOnly        = 0x02,   // split barrier end
};

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceBarrier — a single D3D12_RESOURCE_BARRIER record
//
//  Observed in ResourceBarrierEvent::barriers[].
// ─────────────────────────────────────────────────────────────────────────────

struct ResourceBarrier {
    BarrierType          type           = BarrierType::Transition;
    ResourceBarrierFlags flags          = ResourceBarrierFlags::None;
    uint64_t             resource_id    = 0;   // vangfx resource opaque id

    union {
        struct {
            ResourceState before;        // state before the barrier
            ResourceState after;         // state after the barrier
            uint32_t      subresource;   // D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = 0xFFFFFFFF
        } transition;

        struct {
            uint64_t before_resource_id; // resource being aliased away (0 = none)
            uint64_t after_resource_id;  // resource being aliased in  (0 = none)
        } aliasing;

        struct {
            // UAV barrier has no additional data; just marks a UAV hazard.
            // No initialiser: a default member initialiser on a variant member
            // of an anonymous union deletes the enclosing class's default
            // constructor, which made `ResourceBarrier arr[N];` illegal.
            uint8_t _pad;
        } uav;
    };
};

} // namespace vangfx::d3d12
