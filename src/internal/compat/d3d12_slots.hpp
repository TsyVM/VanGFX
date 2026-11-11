#pragma once

/**
 * vangfx/compat/d3d12_slots.hpp — vtable slot indices for D3D12 COM interfaces.
 *
 * Covers the interfaces VanGFX hooks on the D3D12 backend:
 *   • ID3D12Device              — resource and PSO creation
 *   • ID3D12CommandQueue        — ExecuteCommandLists, Signal, Wait
 *   • ID3D12GraphicsCommandList — draw / compute / barrier recording
 *   • IDXGISwapChain3           — Present (DXGI 1.4 flip-model)
 *
 * Inheritance chain (simplified):
 *   IUnknown(0-2) → ID3D12Object(3-6) → ID3D12DeviceChild(7)
 *   → ID3D12Pageable(—) → ID3D12CommandList(8) → ID3D12GraphicsCommandList(9+)
 *
 * Source: d3d12.h in Windows SDK 10.0.20348.0.
 *
 * NOTE: D3D12 introduced versioned device interfaces (ID3D12Device1 through
 * ID3D12Device14 as of SDK 10.0.26100).  Each version appends new methods to
 * the base vtable.  The slots in `device` below cover ID3D12Device (the base).
 * Additional slots for ID3D12Device1–9 are listed in `device_extended`.
 */

#include <cstdint>

namespace vangfx::d3d12_slots {

// ─────────────────────────────────────────────────────────────────────────────
//  Shared IUnknown + ID3D12Object slots (all D3D12 interfaces)
// ─────────────────────────────────────────────────────────────────────────────

namespace common {
    // IUnknown
    inline constexpr uint32_t QueryInterface     = 0;
    inline constexpr uint32_t AddRef             = 1;
    inline constexpr uint32_t Release            = 2;

    // ID3D12Object
    inline constexpr uint32_t GetPrivateData     = 3;
    inline constexpr uint32_t SetPrivateData     = 4;
    inline constexpr uint32_t SetPrivateDataInterface = 5;
    inline constexpr uint32_t SetName            = 6;
} // namespace common

// ─────────────────────────────────────────────────────────────────────────────
//  IDXGISwapChain3 (DXGI 1.4 — used by D3D12 games for flip-model)
//  Extends IDXGISwapChain2 which extends IDXGISwapChain1 (see d3d11_slots.hpp)
//
//  IDXGISwapChain  base: slots 0-17
//  IDXGISwapChain1:      slots 18-28
//  IDXGISwapChain2:      slots 29-36
//  IDXGISwapChain3:      slots 37-41
// ─────────────────────────────────────────────────────────────────────────────

namespace swap_chain3 {
    // IDXGISwapChain2 extensions (after IDXGISwapChain1's 29 slots)
    inline constexpr uint32_t SetSourceSize            = 29;
    inline constexpr uint32_t GetSourceSize            = 30;
    inline constexpr uint32_t SetMaximumFrameLatency   = 31;
    inline constexpr uint32_t GetMaximumFrameLatency   = 32;
    inline constexpr uint32_t GetFrameLatencyWaitableObject = 33;
    inline constexpr uint32_t SetMatrixTransform       = 34;
    inline constexpr uint32_t GetMatrixTransform       = 35;

    // IDXGISwapChain3 extensions
    inline constexpr uint32_t GetCurrentBackBufferIndex = 36; // ← needed for frame tracking
    inline constexpr uint32_t CheckColorSpaceSupport   = 37;
    inline constexpr uint32_t SetColorSpace1           = 38;
    inline constexpr uint32_t ResizeBuffers1           = 39;

    // Present is inherited from IDXGISwapChain slot 8; use that.
    // Present1 is inherited from IDXGISwapChain1 slot 22 — hook that for D3D12.
    inline constexpr uint32_t Present                  =  8;  // alias
    inline constexpr uint32_t Present1                 = 22;  // ← VanGFX primary D3D12 hook

    inline constexpr uint32_t kSlotCount               = 40;
} // namespace swap_chain3

// ─────────────────────────────────────────────────────────────────────────────
//  ID3D12CommandQueue
//  IUnknown(0-2) → ID3D12Object(3-6) → ID3D12DeviceChild(7) → ID3D12Pageable(8)
//  → ID3D12CommandQueue(9+)
// ─────────────────────────────────────────────────────────────────────────────

namespace command_queue {
    // Inherited
    inline constexpr uint32_t QueryInterface     = 0;
    inline constexpr uint32_t AddRef             = 1;
    inline constexpr uint32_t Release            = 2;
    inline constexpr uint32_t GetPrivateData     = 3;
    inline constexpr uint32_t SetPrivateData     = 4;
    inline constexpr uint32_t SetPrivateDataInterface = 5;
    inline constexpr uint32_t SetName            = 6;
    inline constexpr uint32_t GetDevice          = 7;

    // ID3D12CommandQueue (ID3D12Pageable adds no new methods)
    inline constexpr uint32_t UpdateTileMappings          =  8;
    inline constexpr uint32_t CopyTileMappings            =  9;
    inline constexpr uint32_t ExecuteCommandLists         = 10;  // ← VanGFX submit hook
    inline constexpr uint32_t SetMarker                   = 11;
    inline constexpr uint32_t BeginEvent                  = 12;
    inline constexpr uint32_t EndEvent                    = 13;
    inline constexpr uint32_t Signal                      = 14;  // ← fence hook
    inline constexpr uint32_t Wait                        = 15;  // ← fence hook
    inline constexpr uint32_t GetTimestampFrequency       = 16;
    inline constexpr uint32_t GetClockCalibration         = 17;
    inline constexpr uint32_t GetDesc                     = 18;

    inline constexpr uint32_t kSlotCount                  = 19;
} // namespace command_queue

// ─────────────────────────────────────────────────────────────────────────────
//  ID3D12GraphicsCommandList
//  IUnknown(0-2) → ID3D12Object(3-6) → ID3D12DeviceChild(7)
//  → ID3D12CommandList(8) → ID3D12GraphicsCommandList(9+)
// ─────────────────────────────────────────────────────────────────────────────

namespace graphics_command_list {
    // Inherited
    inline constexpr uint32_t QueryInterface     = 0;
    inline constexpr uint32_t AddRef             = 1;
    inline constexpr uint32_t Release            = 2;
    inline constexpr uint32_t GetPrivateData     = 3;
    inline constexpr uint32_t SetPrivateData     = 4;
    inline constexpr uint32_t SetPrivateDataInterface = 5;
    inline constexpr uint32_t SetName            = 6;
    inline constexpr uint32_t GetDevice          = 7;

    // ID3D12CommandList
    inline constexpr uint32_t GetType            = 8;

    // ID3D12GraphicsCommandList — declaration order from d3d12.h
    inline constexpr uint32_t Close                               =  9;  // ← hook
    inline constexpr uint32_t Reset                               = 10;  // ← hook
    inline constexpr uint32_t ClearState                          = 11;
    inline constexpr uint32_t DrawInstanced                       = 12;  // ← draw hook
    inline constexpr uint32_t DrawIndexedInstanced                = 13;  // ← draw hook
    inline constexpr uint32_t Dispatch                            = 14;  // ← compute hook
    inline constexpr uint32_t CopyBufferRegion                    = 15;
    inline constexpr uint32_t CopyTextureRegion                   = 16;
    inline constexpr uint32_t CopyResource                        = 17;
    inline constexpr uint32_t CopyTiles                           = 18;
    inline constexpr uint32_t ResolveSubresource                  = 19;
    inline constexpr uint32_t IASetPrimitiveTopology              = 20;
    inline constexpr uint32_t RSSetViewports                      = 21;
    inline constexpr uint32_t RSSetScissorRects                   = 22;
    inline constexpr uint32_t OMSetBlendFactor                    = 23;
    inline constexpr uint32_t OMSetStencilRef                     = 24;
    inline constexpr uint32_t SetPipelineState                    = 25;  // ← PSO bind hook
    inline constexpr uint32_t ResourceBarrier                     = 26;  // ← barrier hook
    inline constexpr uint32_t ExecuteBundle                       = 27;
    inline constexpr uint32_t SetDescriptorHeaps                  = 28;
    inline constexpr uint32_t SetComputeRootSignature             = 29;
    inline constexpr uint32_t SetGraphicsRootSignature            = 30;
    inline constexpr uint32_t SetComputeRootDescriptorTable       = 31;
    inline constexpr uint32_t SetGraphicsRootDescriptorTable      = 32;
    inline constexpr uint32_t SetComputeRoot32BitConstant         = 33;
    inline constexpr uint32_t SetGraphicsRoot32BitConstant        = 34;
    inline constexpr uint32_t SetComputeRoot32BitConstants        = 35;
    inline constexpr uint32_t SetGraphicsRoot32BitConstants       = 36;
    inline constexpr uint32_t SetComputeRootConstantBufferView    = 37;
    inline constexpr uint32_t SetGraphicsRootConstantBufferView   = 38;
    inline constexpr uint32_t SetComputeRootShaderResourceView    = 39;
    inline constexpr uint32_t SetGraphicsRootShaderResourceView   = 40;
    inline constexpr uint32_t SetComputeRootUnorderedAccessView   = 41;
    inline constexpr uint32_t SetGraphicsRootUnorderedAccessView  = 42;
    inline constexpr uint32_t IASetIndexBuffer                    = 43;
    inline constexpr uint32_t IASetVertexBuffers                  = 44;
    inline constexpr uint32_t SOSetTargets                        = 45;
    inline constexpr uint32_t OMSetRenderTargets                  = 46;  // ← RT bind hook
    inline constexpr uint32_t ClearDepthStencilView               = 47;
    inline constexpr uint32_t ClearRenderTargetView               = 48;
    inline constexpr uint32_t ClearUnorderedAccessViewUint        = 49;
    inline constexpr uint32_t ClearUnorderedAccessViewFloat       = 50;
    inline constexpr uint32_t DiscardResource                     = 51;
    inline constexpr uint32_t BeginQuery                          = 52;
    inline constexpr uint32_t EndQuery                            = 53;
    inline constexpr uint32_t ResolveQueryData                    = 54;
    inline constexpr uint32_t SetPredication                      = 55;
    inline constexpr uint32_t SetMarker                           = 56;
    inline constexpr uint32_t BeginEvent                          = 57;
    inline constexpr uint32_t EndEvent                            = 58;
    inline constexpr uint32_t ExecuteIndirect                     = 59;  // ← indirect draw hook

    inline constexpr uint32_t kSlotCount                          = 60;

    // ── ID3D12GraphicsCommandList1 extensions (slot 60+) ─────────────────────
    // Added in Windows 10 1703 / SDK 10.0.15063
    inline constexpr uint32_t AtomicCopyBufferUINT                = 60;
    inline constexpr uint32_t AtomicCopyBufferUINT64              = 61;
    inline constexpr uint32_t OMSetDepthBounds                    = 62;
    inline constexpr uint32_t SetSamplePositions                  = 63;
    inline constexpr uint32_t ResolveSubresourceRegion            = 64;
    inline constexpr uint32_t SetViewInstanceMask                 = 65;

    // ── ID3D12GraphicsCommandList2 extensions (slot 66+) ─────────────────────
    inline constexpr uint32_t WriteBufferImmediate                = 66;

    // ── ID3D12GraphicsCommandList3 extensions (slot 67+) ─────────────────────
    inline constexpr uint32_t SetProtectedResourceSession         = 67;

    // ── ID3D12GraphicsCommandList4 extensions (slot 68+) — DXR / mesh shaders
    inline constexpr uint32_t BeginRenderPass                     = 68;
    inline constexpr uint32_t EndRenderPass                       = 69;
    inline constexpr uint32_t InitializeMetaCommand               = 70;
    inline constexpr uint32_t ExecuteMetaCommand                  = 71;
    inline constexpr uint32_t BuildRaytracingAccelerationStructure = 72;
    inline constexpr uint32_t EmitRaytracingAccelerationStructurePostbuildInfo = 73;
    inline constexpr uint32_t CopyRaytracingAccelerationStructure = 74;
    inline constexpr uint32_t SetPipelineState1                   = 75;  // ← DXR PSO hook
    inline constexpr uint32_t DispatchRays                        = 76;  // ← DXR dispatch hook

    // ── ID3D12GraphicsCommandList5 (slot 77+) — ShadingRate / VRS ────────────
    inline constexpr uint32_t RSSetShadingRate                    = 77;
    inline constexpr uint32_t RSSetShadingRateImage               = 78;

    // ── ID3D12GraphicsCommandList6 (slot 79+) — mesh shaders ─────────────────
    inline constexpr uint32_t DispatchMesh                        = 79;  // ← mesh shader hook

    // ── ID3D12GraphicsCommandList7 (slot 80+) — SDK 10.0.22621 ───────────────
    inline constexpr uint32_t Barrier                             = 80;  // Enhanced barriers
} // namespace graphics_command_list

// ─────────────────────────────────────────────────────────────────────────────
//  ID3D12Device  (base — SDK 10.0.20348, Windows 10 1607)
//  IUnknown(0-2) → ID3D12Object(3-6) → ID3D12Device(7+)
// ─────────────────────────────────────────────────────────────────────────────

namespace device {
    // Inherited
    inline constexpr uint32_t QueryInterface     = 0;
    inline constexpr uint32_t AddRef             = 1;
    inline constexpr uint32_t Release            = 2;
    inline constexpr uint32_t GetPrivateData     = 3;
    inline constexpr uint32_t SetPrivateData     = 4;
    inline constexpr uint32_t SetPrivateDataInterface = 5;
    inline constexpr uint32_t SetName            = 6;

    // ID3D12Device
    inline constexpr uint32_t GetNodeCount                          =  7;
    inline constexpr uint32_t CreateCommandQueue                    =  8;
    inline constexpr uint32_t CreateCommandAllocator                =  9;
    inline constexpr uint32_t CreateGraphicsPipelineState           = 10;  // ← PSO hook
    inline constexpr uint32_t CreateComputePipelineState            = 11;  // ← PSO hook
    inline constexpr uint32_t CreateCommandList                     = 12;
    inline constexpr uint32_t CheckFeatureSupport                   = 13;
    inline constexpr uint32_t CreateDescriptorHeap                  = 14;
    inline constexpr uint32_t GetDescriptorHandleIncrementSize      = 15;
    inline constexpr uint32_t CreateRootSignature                   = 16;
    inline constexpr uint32_t CreateConstantBufferView              = 17;
    inline constexpr uint32_t CreateShaderResourceView              = 18;
    inline constexpr uint32_t CreateUnorderedAccessView             = 19;
    inline constexpr uint32_t CreateRenderTargetView                = 20;
    inline constexpr uint32_t CreateDepthStencilView                = 21;
    inline constexpr uint32_t CreateSampler                         = 22;
    inline constexpr uint32_t CopyDescriptors                       = 23;
    inline constexpr uint32_t CopyDescriptorsSimple                 = 24;
    inline constexpr uint32_t GetResourceAllocationInfo             = 25;
    inline constexpr uint32_t GetCustomHeapProperties               = 26;
    // Was missing entirely, and every slot below it was therefore one too low.
    // Verified against ID3D12DeviceVtbl in the Windows SDK: a table that skips
    // an entry does not fail to compile, it silently hooks the wrong function --
    // CreateHeap at 27 would have patched CreateCommittedResource.
    inline constexpr uint32_t CreateCommittedResource               = 27;  // ← resource hook
    inline constexpr uint32_t CreateHeap                            = 28;
    inline constexpr uint32_t CreatePlacedResource                  = 29;
    inline constexpr uint32_t CreateReservedResource                = 30;
    inline constexpr uint32_t CreateSharedHandle                    = 31;
    inline constexpr uint32_t OpenSharedHandle                      = 32;
    inline constexpr uint32_t OpenSharedHandleByName                = 33;
    inline constexpr uint32_t MakeResident                          = 34;
    inline constexpr uint32_t Evict                                 = 35;
    inline constexpr uint32_t CreateFence                           = 36;
    inline constexpr uint32_t GetDeviceRemovedReason                = 37;
    inline constexpr uint32_t GetCopyableFootprints                 = 38;
    inline constexpr uint32_t CreateQueryHeap                       = 39;
    inline constexpr uint32_t SetStablePowerState                   = 40;
    inline constexpr uint32_t CreateCommandSignature                = 41;
    inline constexpr uint32_t GetResourceTiling                     = 42;
    inline constexpr uint32_t GetAdapterLuid                        = 43;

    inline constexpr uint32_t kSlotCount                            = 44;

    // Extension interfaces, each continuing the slot numbering of the one before.
    // Verified against the SDK's ID3D12Device14Vtbl by slot_checks.c; these were
    // off by one from CreateHeap1 on until ID3D12Device4's CreateCommittedResource1
    // was put back in its place.

    // ── ID3D12Device1 ────────────────────────────────────────────────────────
    inline constexpr uint32_t CreatePipelineLibrary                 = 44;
    inline constexpr uint32_t SetEventOnMultipleFenceCompletion     = 45;
    inline constexpr uint32_t SetResidencyPriority                  = 46;

    // ── ID3D12Device2 ────────────────────────────────────────────────────────
    inline constexpr uint32_t CreatePipelineState                   = 47;  // ← mesh/amplification PSO hook

    // ── ID3D12Device3 ────────────────────────────────────────────────────────
    inline constexpr uint32_t OpenExistingHeapFromAddress           = 48;
    inline constexpr uint32_t OpenExistingHeapFromFileMapping       = 49;
    inline constexpr uint32_t EnqueueMakeResident                   = 50;

    // ── ID3D12Device4 ────────────────────────────────────────────────────────
    inline constexpr uint32_t CreateCommandList1                    = 51;  // ← list creation hook
    inline constexpr uint32_t CreateProtectedResourceSession        = 52;
    inline constexpr uint32_t CreateCommittedResource1              = 53;  // ← resource hook
    inline constexpr uint32_t CreateHeap1                           = 54;
    inline constexpr uint32_t CreateReservedResource1               = 55;  // ← resource hook
    inline constexpr uint32_t GetResourceAllocationInfo1            = 56;

    // ── ID3D12Device5 ────────────────────────────────────────────────────────
    inline constexpr uint32_t CreateLifetimeTracker                 = 57;
    inline constexpr uint32_t RemoveDevice                          = 58;
    inline constexpr uint32_t EnumerateMetaCommands                 = 59;
    inline constexpr uint32_t EnumerateMetaCommandParameters        = 60;
    inline constexpr uint32_t CreateMetaCommand                     = 61;
    inline constexpr uint32_t CreateStateObject                     = 62;  // ← DXR PSO
    inline constexpr uint32_t GetRaytracingAccelerationStructurePrebuildInfo = 63;
    inline constexpr uint32_t CheckDriverMatchingIdentifier         = 64;

    // ── ID3D12Device6 ────────────────────────────────────────────────────────
    inline constexpr uint32_t SetBackgroundProcessingMode           = 65;

    // ── ID3D12Device7 ────────────────────────────────────────────────────────
    inline constexpr uint32_t AddToStateObject                      = 66;
    inline constexpr uint32_t CreateProtectedResourceSession1       = 67;

    // ── ID3D12Device8 ────────────────────────────────────────────────────────
    inline constexpr uint32_t GetResourceAllocationInfo2            = 68;
    inline constexpr uint32_t CreateCommittedResource2              = 69;  // ← resource hook
    inline constexpr uint32_t CreatePlacedResource1                 = 70;  // ← resource hook
    inline constexpr uint32_t CreateSamplerFeedbackUnorderedAccessView = 71;
    inline constexpr uint32_t GetCopyableFootprints1                = 72;

    // ── ID3D12Device9 ────────────────────────────────────────────────────────
    inline constexpr uint32_t CreateShaderCacheSession              = 73;
    inline constexpr uint32_t ShaderCacheControl                    = 74;
    inline constexpr uint32_t CreateCommandQueue1                   = 75;

    // ── ID3D12Device10 ───────────────────────────────────────────────────────
    inline constexpr uint32_t CreateCommittedResource3              = 76;  // ← resource hook
    inline constexpr uint32_t CreatePlacedResource2                 = 77;  // ← resource hook
    inline constexpr uint32_t CreateReservedResource2               = 78;  // ← resource hook

    // ── ID3D12Device11 ───────────────────────────────────────────────────────
    inline constexpr uint32_t CreateSampler2                        = 79;

    // ── ID3D12Device12 ───────────────────────────────────────────────────────
    inline constexpr uint32_t GetResourceAllocationInfo3            = 80;

    // ── ID3D12Device13 ───────────────────────────────────────────────────────
    inline constexpr uint32_t OpenExistingHeapFromAddress1          = 81;

    // ── ID3D12Device14 ───────────────────────────────────────────────────────
    inline constexpr uint32_t CreateRootSignatureFromSubobjectInLibrary = 82;
} // namespace device

// ─────────────────────────────────────────────────────────────────────────────
//  Short names.
//
//  The backend sources were written against swap_chain / cmd_queue / cmd_list;
//  the tables above are named for the exact interface whose slot numbering they
//  document, which is the more useful name to read. Aliases rather than a
//  rename in either direction: the long names say precisely which interface a
//  slot index belongs to, and the short ones are what the call sites say.
// ─────────────────────────────────────────────────────────────────────────────

namespace swap_chain = swap_chain3;
namespace cmd_queue  = command_queue;
namespace cmd_list   = graphics_command_list;

} // namespace vangfx::d3d12_slots
