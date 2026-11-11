#pragma once

/**
 * vangfx/events/surface.hpp — Render target, depth buffer, and present events.
 *
 * RenderTargetEvent covers both D3D11 OMSetRenderTargets and D3D12
 * OMSetRenderTargets (which uses CPU descriptor handles rather than interface
 * pointers).  The d3d12_rtv_handles[] and d3d12_dsv_handle fields are only
 * populated when the backend is D3D12; the generic handles[] array is always
 * populated by the runtime for all backends.
 *
 * ResourceBarrierEvent is D3D12-only and is fired by Context::on_resource_barrier.
 */

#include <vangfx/resources/surface.hpp>
#include <vangfx/d3d12/command.hpp>
#include <vangfx/d3d12/heap.hpp>
#include <vangfx/d3d12/resource.hpp>
#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  RenderTargetEvent — fired on IDirect3DDevice9::SetRenderTarget /
//                      ID3D11DeviceContext::OMSetRenderTargets
//
//  count == 0  → depth-only pass (shadow map)
//  count == 1  → forward pass
//  count >= 3  → likely deferred GBuffer fill
// ─────────────────────────────────────────────────────────────────────────────

struct RenderTargetEvent {
    RenderTargetHandle targets[8] = {};
    uint32_t           count      = 0;
    DepthBufferHandle  depth      = {};
    uint32_t           width      = 0;
    uint32_t           height     = 0;
    bool               intercept  = false; // set true to suppress the D3D call

    // ── D3D12-only (zero on D3D9/D3D11) ──────────────────────────────────────
    // Raw CPU descriptor handles passed to ID3D12GraphicsCommandList::OMSetRenderTargets.
    // These are the actual D3D12_CPU_DESCRIPTOR_HANDLE values; valid only
    // during the callback.  Use the targets[] handles for resource queries.
    d3d12::CpuDescriptorHandle d3d12_rtv_handles[8] = {};
    d3d12::CpuDescriptorHandle d3d12_dsv_handle     = {};
    // True when RTsSingleHandleToDescriptorRange = TRUE was passed (contiguous RTV range).
    bool                        d3d12_single_handle_range = false;
    // The command list recording this OMSetRenderTargets call.
    d3d12::CommandListHandle    d3d12_command_list  = {};
};

// ─────────────────────────────────────────────────────────────────────────────
//  DepthEvent — fired when only the depth/stencil surface changes
// ─────────────────────────────────────────────────────────────────────────────

struct DepthEvent {
    DepthBufferHandle depth       = {};
    SurfaceFormat     format      = SurfaceFormat::Unknown;
    bool              has_stencil = false;
    bool              intercept   = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  PresentEvent — fired on IDirect3DDevice9::Present / IDXGISwapChain::Present
// ─────────────────────────────────────────────────────────────────────────────

struct PresentEvent {
    uint32_t sync_interval = 0; // 0 = immediate, 1 = vsync, etc.
    uint32_t flags         = 0; // DXGI_PRESENT_* or D3D9 equivalent
    bool     intercept     = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceBarrierBatch — D3D12-only; fired by Context::on_resource_barrier
//
//  Wraps a single ID3D12GraphicsCommandList::ResourceBarrier() call which may
//  contain multiple D3D12_RESOURCE_BARRIER records.  The barriers array is
//  valid only during the callback.
//
//  Set intercept = true to suppress the underlying ResourceBarrier call.
//  This is dangerous: suppressing a required barrier may corrupt rendering or
//  trigger D3D12 validation errors.  Use only for debugging.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t kMaxBarriersPerBatch = 64;

struct ResourceBarrierBatch {
    d3d12::CommandListHandle  command_list  = {};
    d3d12::ResourceBarrier*   barriers      = nullptr;  // points into caller memory
    uint32_t                  count         = 0;
    bool                      intercept     = false;
};

} // namespace vangfx
