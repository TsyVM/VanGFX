#pragma once

/**
 * vangfx/events/resource.hpp — Texture and buffer lifecycle events.
 *
 * ResourceEvent is fired whenever the game creates or destroys a GPU resource
 * that vangfx tracks.  Use it to maintain a mod-side catalogue of live
 * textures, render targets, and buffers.
 *
 * event.action == ResourceAction::Create  → resource has just been allocated;
 *                                           all descriptor fields are valid.
 * event.action == ResourceAction::Destroy → resource is about to be freed;
 *                                           handle will be invalid after return.
 */

#include <vangfx/resources/buffer.hpp>
#include <vangfx/resources/surface.hpp>
#include <vangfx/resources/texture.hpp>
#include <vangfx/d3d12/resource.hpp>
#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceKind — which type of D3D object triggered the event
// ─────────────────────────────────────────────────────────────────────────────

enum class ResourceKind : uint8_t {
    Texture,
    RenderTarget,
    DepthBuffer,
    VertexBuffer,
    IndexBuffer,
    ConstantBuffer,
    StructuredBuffer,      // D3D11/D3D12
    UAV,                   // D3D11/D3D12
    RawBuffer,             // D3D12 — ByteAddressBuffer (R32_TYPELESS SRV)
    AccelStructure,        // D3D12 — DXR BLAS/TLAS
};

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceAction — what happened to the resource
// ─────────────────────────────────────────────────────────────────────────────

enum class ResourceAction : uint8_t {
    Create,     // resource was just allocated
    Update,     // resource content was written (dynamic resources only)
    Destroy,    // resource is about to be freed
};

// ─────────────────────────────────────────────────────────────────────────────
//  ResourceEvent
//
//  Exactly one of the handle fields will be non-zero; which one corresponds
//  to the kind field.  The desc_* union member is only valid on Create.
// ─────────────────────────────────────────────────────────────────────────────

struct ResourceEvent {
    ResourceKind   kind      = ResourceKind::Texture;
    ResourceAction action    = ResourceAction::Create;

    // ── Handles (one is non-zero) ─────────────────────────────────────────────
    TextureHandle          texture          = {};
    RenderTargetHandle     render_target    = {};
    DepthBufferHandle      depth_buffer     = {};
    VertexBufferHandle     vertex_buffer    = {};
    IndexBufferHandle      index_buffer     = {};
    ConstantBufferHandle   constant_buffer  = {};
    StructuredBufferHandle structured_buf   = {};
    UAVHandle              uav              = {};
    RawBufferHandle        raw_buffer       = {};   // D3D12
    AccelStructureHandle   accel_structure  = {};   // D3D12

    // ── Descriptors (valid on Create only) ───────────────────────────────────
    TextureDesc        tex_desc  = {};
    RenderTargetDesc   rt_desc   = {};
    DepthBufferDesc    db_desc   = {};
    VertexBufferDesc   vb_desc   = {};
    StructuredBufferDesc sb_desc = {};

    // ── Update info (valid on Update only) ───────────────────────────────────
    uint32_t update_size_bytes = 0; // bytes written to a dynamic resource

    // ── D3D12-only (default-constructed / ignored on D3D9/D3D11) ─────────────
    d3d12::D3D12ResourceDesc d3d12_desc = {};
};

} // namespace vangfx
