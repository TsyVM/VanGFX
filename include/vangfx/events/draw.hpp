#pragma once

/**
 * vangfx/events/draw.hpp — Draw call and compute dispatch events.
 *
 * DrawEvent is common to all backends.  D3D12-specific fields (command_list,
 * pso, root_bindings) are zero/null on D3D9 and D3D11.
 */

#include <vangfx/resources/buffer.hpp>
#include <vangfx/d3d12/command.hpp>
#include <vangfx/d3d12/pso.hpp>
#include <vangfx/d3d12/heap.hpp>
#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  DrawType
// ─────────────────────────────────────────────────────────────────────────────

enum class DrawType : uint8_t {
    Draw,
    DrawIndexed,
    DrawInstanced,
    DrawIndexedInstanced,
    DrawAuto,               // D3D11 stream-output auto draw
    DrawIndirect,           // D3D11
    DrawIndexedIndirect,    // D3D11
    Dispatch,               // compute
    DispatchIndirect,       // D3D11 compute
};

// ─────────────────────────────────────────────────────────────────────────────
//  DrawEvent — fired on every draw/dispatch call
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t kMaxRootBindings = 64;

struct DrawEvent {
    DrawType           type           = DrawType::Draw;
    uint32_t           vertex_count   = 0;
    uint32_t           index_count    = 0;
    uint32_t           instance_count = 0;
    int32_t            start_vertex   = 0;
    uint32_t           start_index    = 0;
    int32_t            base_vertex    = 0;
    VertexBufferHandle indirect_args  = {}; // handle for indirect draws
    uint32_t           dispatch_x     = 0;  // compute thread groups
    uint32_t           dispatch_y     = 0;
    uint32_t           dispatch_z     = 0;
    bool               intercept      = false;

    // ── D3D12-only fields (zero/null on D3D9/D3D11) ───────────────────────────

    // The command list recording this draw call.
    d3d12::CommandListHandle  command_list    = {};

    // The PSO active at the time of this draw/dispatch.
    d3d12::PSOHandle          pso             = {};

    // Root parameter bindings active at draw time.
    d3d12::RootBinding        root_bindings[kMaxRootBindings] = {};
    uint32_t                  root_binding_count = 0;

    // D3D12 vertex/index buffer views recorded by IASetVertexBuffers /
    // IASetIndexBuffer.
    D3D12VertexBufferBinding  d3d12_vb = {};
    D3D12IndexBufferBinding   d3d12_ib = {};
};

} // namespace vangfx
