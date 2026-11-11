#pragma once

/**
 * vangfx/resources/buffer.hpp — Vertex, index, constant, structured, and UAV
 * buffer types, plus CPU readback.
 */

#include "format.hpp"
#include "texture.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  Shader stage (used throughout vangfx for per-stage binding queries)
// ─────────────────────────────────────────────────────────────────────────────

enum class ShaderStage : uint8_t {
    Vertex,
    Pixel,
    Geometry,   // D3D11 only
    Compute,    // D3D11 only
    Hull,       // D3D11 only
    Domain,     // D3D11 only
};

// ─────────────────────────────────────────────────────────────────────────────
//  Opaque buffer handles
// ─────────────────────────────────────────────────────────────────────────────

struct VertexBufferHandle     { uint64_t id = 0; };
struct IndexBufferHandle      { uint64_t id = 0; };
struct ConstantBufferHandle   { uint64_t id = 0; };
struct StructuredBufferHandle { uint64_t id = 0; }; // D3D11/D3D12
struct UAVHandle              { uint64_t id = 0; }; // D3D11/D3D12

// D3D12-only buffer handles
struct RawBufferHandle        { uint64_t id = 0; }; // ByteAddressBuffer (SRV R32_TYPELESS)
struct AccelStructureHandle   { uint64_t id = 0; }; // DXR acceleration structure

[[nodiscard]] inline bool is_valid(VertexBufferHandle h)     noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(IndexBufferHandle h)      noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(ConstantBufferHandle h)   noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(StructuredBufferHandle h) noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(UAVHandle h)              noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(RawBufferHandle h)        noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(AccelStructureHandle h)   noexcept { return h.id != 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  Binding descriptors — what's currently bound on the device
// ─────────────────────────────────────────────────────────────────────────────

struct VertexBufferBinding {
    VertexBufferHandle handle  = {};
    uint32_t           stride  = 0;
    uint32_t           offset  = 0;
    uint32_t           slot    = 0;   // D3D11 stream slot
};

struct IndexBufferBinding {
    IndexBufferHandle handle   = {};
    uint32_t          offset   = 0;
    bool              is_32bit = true; // true = R32_UINT, false = R16_UINT
};

struct ConstantBufferBinding {
    ConstantBufferHandle handle = {};
    ShaderStage          stage  = ShaderStage::Vertex;
    uint32_t             slot   = 0;  // b-register (D3D11) / conceptual slot (D3D9)
    uint32_t             size   = 0;  // bytes
};

// ─────────────────────────────────────────────────────────────────────────────
//  Buffer descriptors
// ─────────────────────────────────────────────────────────────────────────────

struct VertexBufferDesc {
    uint32_t size              = 0;
    uint32_t stride            = 0;
    bool     is_dynamic        = false;
    bool     is_stream_output  = false; // D3D11
};

struct StructuredBufferDesc {
    // Total size in bytes. Not always element_count * element_stride: a D3D12
    // resource is reported to the callback before anything has told us how the
    // shader intends to view it, so the byte size is the only figure that is
    // always known.
    uint32_t size              = 0;
    uint32_t element_count     = 0;
    uint32_t element_stride    = 0;
    bool     is_append_consume = false;
    bool     is_indirect_args  = false;
    bool     has_uav           = false;
    bool     has_srv           = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  D3D12 vertex / index buffer view descriptors
//
//  D3D12 does not have a device-managed vertex/index buffer binding;
//  instead the command list records GPU virtual addresses and formats.
//  VanGFX surfaces these from IASetVertexBuffers / IASetIndexBuffer intercepts.
// ─────────────────────────────────────────────────────────────────────────────

// D3D12_VERTEX_BUFFER_VIEW equivalent
struct D3D12VertexBufferView {
    uint64_t gpu_virtual_address = 0;  // ID3D12Resource GPU VA
    uint32_t size_in_bytes       = 0;
    uint32_t stride_in_bytes     = 0;
};

// D3D12_INDEX_BUFFER_VIEW equivalent
struct D3D12IndexBufferView {
    uint64_t     gpu_virtual_address = 0;
    uint32_t     size_in_bytes       = 0;
    SurfaceFormat format             = SurfaceFormat::Unknown; // R16_UINT or R32_UINT
};

// D3D12_CONSTANT_BUFFER_VIEW_DESC equivalent
struct D3D12ConstantBufferView {
    uint64_t gpu_virtual_address = 0;
    uint32_t size_in_bytes       = 0;  // must be a multiple of 256
};

// Observed binding on the D3D12 pipeline (recorded into DrawEvent).
struct D3D12VertexBufferBinding {
    D3D12VertexBufferView views[16] = {};  // up to 16 IASetVertexBuffers slots
    uint32_t              count     = 0;
    uint32_t              start_slot = 0;
};

struct D3D12IndexBufferBinding {
    D3D12IndexBufferView view = {};
    bool                 bound = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ReadbackData — result of ctx.readback(texture_handle)
//
//  pixels is valid only for the lifetime of the ReadbackData object.
//  Copy raw bytes to hold onto them past the next frame.
// ─────────────────────────────────────────────────────────────────────────────

struct ReadbackData {
    std::span<const uint8_t> pixels;
    uint32_t                 width     = 0;
    uint32_t                 height    = 0;
    SurfaceFormat            format    = SurfaceFormat::Unknown;
    uint32_t                 row_pitch = 0;  // bytes per scanline (may include padding)
};

// ─────────────────────────────────────────────────────────────────────────────
//  BufferData — result of ctx.readback(vertex_buffer_handle) and its siblings
//
//  A buffer is bytes, not pixels.  ReadbackData describes a 2D image -- width,
//  height, format, row pitch -- and every one of those fields is a lie about an
//  index buffer, so buffer readback returns this instead.  What the bytes mean
//  is the caller's to know: the vertex layout comes from the pipeline state
//  (PipelineState::input_layout), the index width from the index buffer view,
//  and a constant buffer's contents from the shader that declared it.
//
//  `bytes` is valid only until the next readback on the same FrameContext, and
//  never past the end of the frame.  Copy what you need to keep.
// ─────────────────────────────────────────────────────────────────────────────

struct BufferData {
    std::span<const uint8_t> bytes;

    // Where in the source buffer this came from, and how big the whole source
    // is -- so a caller that asked for a window knows what it was a window into.
    uint64_t offset      = 0;
    uint64_t buffer_size = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  BufferRef — which buffer a GPU virtual address lands in (D3D12)
//
//  DrawEvent carries the vertex and index buffer *views* the game set, and a
//  view is a GPU virtual address rather than a handle. That address is the only
//  thing tying a draw to the memory behind it, so without a way to resolve one
//  there is no route from "this draw" to "these indices" -- and identifying a
//  draw is the first thing anyone modifying a game's rendering has to do.
//
//  FrameContext::buffer_at() resolves an address to the buffer containing it.
//  `offset` is where the address sits inside that buffer, which is usually not
//  zero: a game typically suballocates many meshes out of one large buffer, so
//  the same handle comes back for every draw and the offset is what separates
//  them.
//
//  `id` of 0 means no registered buffer covers that address.
// ─────────────────────────────────────────────────────────────────────────────

struct BufferRef {
    uint64_t id     = 0;   // the id shared by every buffer handle type
    uint64_t offset = 0;   // byte offset of the queried address within it
    uint64_t size   = 0;   // total size of the buffer

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }

    // The handle types are labels over one id space; pick the one that says
    // what you know the bytes are.
    [[nodiscard]] VertexBufferHandle     as_vertex_buffer()     const noexcept { return { id }; }
    [[nodiscard]] IndexBufferHandle      as_index_buffer()      const noexcept { return { id }; }
    [[nodiscard]] ConstantBufferHandle   as_constant_buffer()   const noexcept { return { id }; }
    [[nodiscard]] StructuredBufferHandle as_structured_buffer() const noexcept { return { id }; }
    [[nodiscard]] RawBufferHandle        as_raw_buffer()        const noexcept { return { id }; }
    [[nodiscard]] UAVHandle              as_uav()               const noexcept { return { id }; }
};

} // namespace vangfx
