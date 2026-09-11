#pragma once

/**
 * vangfx/d3d12/pso.hpp — Pipeline State Object (PSO) descriptors, root
 * signature handle, and input layout for D3D12.
 *
 * D3D12 bakes almost the entire pipeline (shaders, rasterizer, blend, depth,
 * stencil, input layout, render target formats, MSAA) into a single immutable
 * PSO at creation time.  VanGFX intercepts CreateGraphicsPipelineState and
 * CreateComputePipelineState and fires a PSOCreateEvent so mods can inspect or
 * replace pipeline configuration before GPU compilation begins.
 *
 * PSOHandle uniquely identifies a live ID3D12PipelineState.  When a PSO is
 * destroyed the handle becomes invalid (id == 0).
 */

#include <vangfx/d3d12/heap.hpp>      // RootSignatureHandle
#include <vangfx/resources/format.hpp>
#include <cstdint>
#include <string_view>

namespace vangfx::d3d12 {

// ─────────────────────────────────────────────────────────────────────────────
//  PSOHandle — opaque reference to a compiled PSO
// ─────────────────────────────────────────────────────────────────────────────

struct PSOHandle { uint64_t id = 0; };

[[nodiscard]] inline bool is_valid(PSOHandle h) noexcept { return h.id != 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  PSOKind
// ─────────────────────────────────────────────────────────────────────────────

enum class PSOKind : uint8_t {
    Graphics,   // CreateGraphicsPipelineState
    Compute,    // CreateComputePipelineState
    MeshShader, // CreatePipelineState (amplification/mesh, DX12 Ultimate)
};

// ─────────────────────────────────────────────────────────────────────────────
//  ShaderBytecodeView — non-owning view of a compiled DXIL/DXBC blob
//
//  points into the game's memory; do not hold past the PSOCreateEvent callback.
// ─────────────────────────────────────────────────────────────────────────────

struct ShaderBytecodeView {
    const void* data = nullptr;
    size_t      size = 0;

    [[nodiscard]] bool empty() const noexcept { return data == nullptr || size == 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  InputClassification — D3D12_INPUT_CLASSIFICATION
// ─────────────────────────────────────────────────────────────────────────────

enum class InputClassification : uint8_t {
    PerVertex,    // D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
    PerInstance,  // D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
};

// ─────────────────────────────────────────────────────────────────────────────
//  InputElement — mirrors D3D12_INPUT_ELEMENT_DESC
// ─────────────────────────────────────────────────────────────────────────────

struct InputElement {
    const char*          semantic_name   = nullptr;  // e.g. "POSITION", "TEXCOORD"
    uint32_t             semantic_index  = 0;
    SurfaceFormat        format          = SurfaceFormat::Unknown;
    uint32_t             input_slot      = 0;
    uint32_t             aligned_offset  = 0;        // 0xFFFFFFFF = D3D12_APPEND_ALIGNED_ELEMENT
    InputClassification  classification  = InputClassification::PerVertex;
    uint32_t             instance_step   = 0;        // step rate for PerInstance data
};

// ─────────────────────────────────────────────────────────────────────────────
//  RasterizerDesc — relevant rasterizer fields (subset of D3D12_RASTERIZER_DESC)
// ─────────────────────────────────────────────────────────────────────────────

enum class FillMode : uint8_t { Wireframe, Solid };
enum class CullMode : uint8_t { None, Front, Back };

struct RasterizerDesc {
    FillMode fill_mode               = FillMode::Solid;
    CullMode cull_mode               = CullMode::Back;
    bool     front_counter_clockwise = false;
    int32_t  depth_bias              = 0;
    float    depth_bias_clamp        = 0.f;
    float    slope_scaled_depth_bias = 0.f;
    bool     depth_clip_enable       = true;
    bool     multisample_enable      = false;
    bool     antialiased_line_enable = false;
    uint32_t forced_sample_count     = 0;
    bool     conservative_raster     = false;   // DX12 tier 1+
};

// ─────────────────────────────────────────────────────────────────────────────
//  BlendFactor / BlendOp / BlendDesc — mirrors D3D12_BLEND / BLEND_OP / BLEND_DESC
// ─────────────────────────────────────────────────────────────────────────────

enum class BlendFactor : uint8_t {
    Zero, One,
    SrcColor, InvSrcColor, SrcAlpha, InvSrcAlpha,
    DestAlpha, InvDestAlpha, DestColor, InvDestColor,
    SrcAlphaSat, BlendFactor_, InvBlendFactor, Src1Color, InvSrc1Color,
    Src1Alpha, InvSrc1Alpha,
};

enum class BlendOp : uint8_t { Add, Subtract, RevSubtract, Min, Max };

struct RenderTargetBlend {
    bool         blend_enable     = false;
    BlendFactor  src_blend        = BlendFactor::One;
    BlendFactor  dest_blend       = BlendFactor::Zero;
    BlendOp      blend_op         = BlendOp::Add;
    BlendFactor  src_blend_alpha  = BlendFactor::One;
    BlendFactor  dest_blend_alpha = BlendFactor::Zero;
    BlendOp      blend_op_alpha   = BlendOp::Add;
    uint8_t      write_mask       = 0x0F; // D3D12_COLOR_WRITE_ENABLE_ALL
};

struct BlendDesc {
    bool               alpha_to_coverage = false;
    bool               independent_blend = false;  // false = rt[0] replicated
    RenderTargetBlend  rt[8]             = {};
};

// ─────────────────────────────────────────────────────────────────────────────
//  DepthStencilDesc (abridged) — covers the common mod-relevant fields
// ─────────────────────────────────────────────────────────────────────────────

enum class ComparisonFunc : uint8_t {
    Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always
};

struct DepthStencilDesc {
    bool           depth_enable        = true;
    bool           depth_write         = true;
    ComparisonFunc depth_func          = ComparisonFunc::Less;
    bool           stencil_enable      = false;
    uint8_t        stencil_read_mask   = 0xFF;
    uint8_t        stencil_write_mask  = 0xFF;
};

// ─────────────────────────────────────────────────────────────────────────────
//  GraphicsPSODesc — the full D3D12 graphics PSO as seen at interception time
//
//  The shader bytecode views point into the game's memory and are only valid
//  during the PSOCreateEvent callback.  Copy them if you need them later.
//
//  input_elements / input_element_count describe the vertex input layout.
//  They point into game memory; same lifetime caveat applies.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t kMaxRenderTargets = 8;

struct GraphicsPSODesc {
    RootSignatureHandle  root_signature   = {};

    // Shader stages (DXIL blobs as seen in D3D12_GRAPHICS_PIPELINE_STATE_DESC)
    ShaderBytecodeView   vs;   // vertex shader        (required)
    ShaderBytecodeView   ps;   // pixel shader         (optional)
    ShaderBytecodeView   gs;   // geometry shader      (optional)
    ShaderBytecodeView   hs;   // hull shader          (optional)
    ShaderBytecodeView   ds;   // domain shader        (optional)
    ShaderBytecodeView   as;   // amplification shader (mesh pipeline)
    ShaderBytecodeView   ms;   // mesh shader          (mesh pipeline)

    // Input layout
    const InputElement*  input_elements       = nullptr;
    uint32_t             input_element_count  = 0;

    // Fixed-function state
    RasterizerDesc       rasterizer   = {};
    BlendDesc            blend        = {};
    DepthStencilDesc     depth_stencil = {};

    // Render target formats
    uint32_t             num_render_targets   = 0;
    SurfaceFormat        rtv_formats[kMaxRenderTargets] = {};
    SurfaceFormat        dsv_format           = SurfaceFormat::Unknown;

    // MSAA
    uint32_t             sample_count   = 1;
    uint32_t             sample_quality = 0;

    // Primitive topology type (D3D12_PRIMITIVE_TOPOLOGY_TYPE)
    uint8_t              topology_type  = 4;  // 4 = TRIANGLE

    uint32_t             node_mask      = 0;

    // D3D12_PIPELINE_STATE_FLAGS
    uint32_t             flags          = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ComputePSODesc
// ─────────────────────────────────────────────────────────────────────────────

struct ComputePSODesc {
    RootSignatureHandle root_signature = {};
    ShaderBytecodeView  cs;              // compute shader (required)
    uint32_t            node_mask       = 0;
    uint32_t            flags           = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  PSOCreateEvent — fired by Context::on_pso_create() before the D3D12 call
//
//  Mods may:
//    • inspect the desc fields to identify which pass this PSO belongs to.
//    • swap shader bytecode (replace desc.ps.data / desc.ps.size with a
//      patched blob, valid until the callback returns).
//    • set intercept = true to block the creation entirely (pso handle
//      remains invalid; the game may crash — use only for debugging).
// ─────────────────────────────────────────────────────────────────────────────

struct PSOCreateEvent {
    PSOKind  kind      = PSOKind::Graphics;
    PSOHandle pso      = {};  // filled by runtime after creation (read in on_pso_create)
    bool     intercept = false;

    // Only one of these is valid depending on kind:
    GraphicsPSODesc* graphics = nullptr;  // PSOKind::Graphics or MeshShader
    ComputePSODesc*  compute  = nullptr;  // PSOKind::Compute
};

} // namespace vangfx::d3d12
