#pragma once

/**
 * vangfx/pipeline/state.hpp — Blend, depth/stencil, rasterizer, sampler,
 * and full PipelineState snapshot types.
 */

#include "viewport.hpp"
#include "topology.hpp"
#include <array>
#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  Enums
// ─────────────────────────────────────────────────────────────────────────────

enum class FilterMode : uint8_t  { Point, Linear, Anisotropic };
enum class AddressMode : uint8_t { Wrap, Mirror, Clamp, Border, MirrorOnce };
enum class FillMode : uint8_t    { Solid, Wireframe };
enum class CullMode : uint8_t    { None, Front, Back };

enum class CompareFunc : uint8_t {
    Never, Less, Equal, LessEqual,
    Greater, NotEqual, GreaterEqual, Always,
};

enum class BlendFactor : uint8_t {
    Zero, One,
    SrcColor,    InvSrcColor,
    SrcAlpha,    InvSrcAlpha,
    DstAlpha,    InvDstAlpha,
    DstColor,    InvDstColor,
    SrcAlphaSat,
    BlendFactor, InvBlendFactor,
    Src1Color,   InvSrc1Color,
    Src1Alpha,   InvSrc1Alpha,
};

enum class BlendOp : uint8_t {
    Add, Subtract, RevSubtract, Min, Max,
};

enum class StencilOpMode : uint8_t {
    Keep, Zero, Replace, IncrSat, DecrSat, Invert, Incr, Decr,
};

// ─────────────────────────────────────────────────────────────────────────────
//  BlendState
// ─────────────────────────────────────────────────────────────────────────────

struct RenderTargetBlend {
    bool         blend_enable  = false;
    BlendFactor  src_color     = BlendFactor::One;
    BlendFactor  dst_color     = BlendFactor::Zero;
    BlendOp      color_op      = BlendOp::Add;
    BlendFactor  src_alpha     = BlendFactor::One;
    BlendFactor  dst_alpha     = BlendFactor::Zero;
    BlendOp      alpha_op      = BlendOp::Add;
    uint8_t      write_mask    = 0x0F; // RGBA channel mask
};

struct BlendState {
    bool              alpha_to_coverage  = false;
    bool              independent_blend  = false; // D3D11 per-RT blend
    RenderTargetBlend targets[8]         = {};    // [0] only on D3D9

    // The constant colour BlendFactor and InvBlendFactor multiply against.
    //
    // Every backend sets one -- D3D9 through D3DRS_BLENDFACTOR, D3D11 through
    // OMSetBlendState, D3D12 through OMSetBlendFactor -- and there was nowhere
    // to report it, so a callback could see that a blend used BlendFactor and
    // never what the factor was. Defaults to opaque white, which is what a
    // device starts at.
    float             blend_factor[4]    = { 1.f, 1.f, 1.f, 1.f };
};

// ─────────────────────────────────────────────────────────────────────────────
//  DepthStencilState
// ─────────────────────────────────────────────────────────────────────────────

struct StencilOp {
    StencilOpMode fail       = StencilOpMode::Keep;
    StencilOpMode depth_fail = StencilOpMode::Keep;
    StencilOpMode pass       = StencilOpMode::Keep;
    CompareFunc   func       = CompareFunc::Always;
};

struct DepthStencilState {
    bool        depth_enable       = true;
    bool        depth_write        = true;
    CompareFunc depth_func         = CompareFunc::Less;
    bool        stencil_enable     = false;
    uint8_t     stencil_read_mask  = 0xFF;
    uint8_t     stencil_write_mask = 0xFF;
    StencilOp   front_face         = {};
    StencilOp   back_face          = {};    // two-sided stencil
    uint32_t    stencil_ref        = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  RasterizerState
// ─────────────────────────────────────────────────────────────────────────────

struct RasterizerState {
    FillMode fill_mode                  = FillMode::Solid;
    CullMode cull_mode                  = CullMode::Back;
    bool     front_ccw                  = false;
    int32_t  depth_bias                 = 0;
    float    depth_bias_clamp           = 0.0f;
    float    slope_scaled_depth_bias    = 0.0f;
    bool     depth_clip_enable          = true;
    bool     scissor_enable             = false;
    bool     multisample_enable         = false;
    bool     antialiased_line           = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  SamplerState
// ─────────────────────────────────────────────────────────────────────────────

struct SamplerState {
    FilterMode  filter          = FilterMode::Linear;
    AddressMode address_u       = AddressMode::Wrap;
    AddressMode address_v       = AddressMode::Wrap;
    AddressMode address_w       = AddressMode::Wrap;
    float       mip_bias        = 0.0f;
    uint32_t    max_anisotropy  = 1;
    CompareFunc compare_func    = CompareFunc::Never;   // for shadow samplers
    float       border_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float       min_lod         = -3.402823466e+38f;
    float       max_lod         =  3.402823466e+38f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  VertexDeclaration — opaque descriptor for the input layout / vertex format
// ─────────────────────────────────────────────────────────────────────────────

struct VertexDeclaration {
    uint64_t handle = 0; // null = none bound
};

// ─────────────────────────────────────────────────────────────────────────────
//  PipelineState — full snapshot of the device pipeline
// ─────────────────────────────────────────────────────────────────────────────

struct PipelineState {
    BlendState        blend;
    DepthStencilState depth_stencil;
    RasterizerState   raster;
    SamplerState      samplers[16]; // per-slot
    Viewport          viewports[8]; // D3D11 up to 8, D3D9 always 1
    ScissorRect       scissors[8];
    PrimitiveTopology topology    = PrimitiveTopology::TriangleList;
    VertexDeclaration vertex_decl;
};

} // namespace vangfx
