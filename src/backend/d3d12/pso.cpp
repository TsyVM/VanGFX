/**
 * src/backend/d3d12/pso.cpp — pipeline state creation.
 *
 * Hooked on ID3D12Device (every device class, once each):
 *   CreateGraphicsPipelineState, CreateComputePipelineState,
 *   CreatePipelineState (ID3D12Device2: pipeline state streams, which is how
 *   modern engines build all their pipelines, mesh shaders or not).
 *
 * Each creation is reported through on_pso_create before the runtime sees it;
 * a callback may swap shader bytecode in the description or block the creation.
 * The pipeline is then registered with the hub (hub.hpp) under the id the event
 * carried, with the hashes of the shaders it runs, so SetPipelineState and every
 * draw event can name it; and, while any context on the device has linked an
 * Effect, with a PsoBlueprint the effect bridge rebuilds it from.
 *
 * Destruction is learned through compat/lifetime.hpp, not a Release hook.
 */

#include "d3d12_impl.hpp"
#include "effect_bridge.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <format_map.hpp>
#include <hash.hpp>
#include <vangfx/d3d12/pso.hpp>

#include <cstring>
#include <vector>

namespace vangfx::backend::d3d12 {

using namespace ::vangfx::d3d12;
namespace dev = ::vangfx::d3d12_slots::device;

namespace {

using CreateGfxFn    = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateCmpFn    = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStreamFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

hooks::Site s_gfx   { dev::CreateGraphicsPipelineState, "ID3D12Device::CreateGraphicsPipelineState" };
hooks::Site s_cmp   { dev::CreateComputePipelineState,  "ID3D12Device::CreateComputePipelineState" };
hooks::Site s_stream{ dev::CreatePipelineState,         "ID3D12Device2::CreatePipelineState" };

// ── Description → event ──────────────────────────────────────────────────────

InputClassification to_classification(D3D12_INPUT_CLASSIFICATION c) noexcept {
    return c == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? InputClassification::PerInstance
                                                             : InputClassification::PerVertex;
}

vangfx::d3d12::CullMode to_cull(D3D12_CULL_MODE m) noexcept {
    switch (m) {
    case D3D12_CULL_MODE_FRONT: return vangfx::d3d12::CullMode::Front;
    case D3D12_CULL_MODE_BACK:  return vangfx::d3d12::CullMode::Back;
    default:                    return vangfx::d3d12::CullMode::None;
    }
}

ComparisonFunc to_compare(D3D12_COMPARISON_FUNC f) noexcept {
    // D3D12_COMPARISON_FUNC runs NEVER = 1 .. ALWAYS = 8 in the same order.
    if (f >= D3D12_COMPARISON_FUNC_NEVER && f <= D3D12_COMPARISON_FUNC_ALWAYS)
        return static_cast<ComparisonFunc>(f - D3D12_COMPARISON_FUNC_NEVER);
    return ComparisonFunc::Less;
}

// Mapped by name: D3D12_BLEND starts at 1 and skips 12 and 13, so the old
// static_cast put every factor from ONE on in the wrong place.
vangfx::d3d12::BlendFactor to_blend(D3D12_BLEND b) noexcept {
    switch (b) {
    case D3D12_BLEND_ZERO:             return vangfx::d3d12::BlendFactor::Zero;
    case D3D12_BLEND_ONE:              return vangfx::d3d12::BlendFactor::One;
    case D3D12_BLEND_SRC_COLOR:        return vangfx::d3d12::BlendFactor::SrcColor;
    case D3D12_BLEND_INV_SRC_COLOR:    return vangfx::d3d12::BlendFactor::InvSrcColor;
    case D3D12_BLEND_SRC_ALPHA:        return vangfx::d3d12::BlendFactor::SrcAlpha;
    case D3D12_BLEND_INV_SRC_ALPHA:    return vangfx::d3d12::BlendFactor::InvSrcAlpha;
    case D3D12_BLEND_DEST_ALPHA:       return vangfx::d3d12::BlendFactor::DestAlpha;
    case D3D12_BLEND_INV_DEST_ALPHA:   return vangfx::d3d12::BlendFactor::InvDestAlpha;
    case D3D12_BLEND_DEST_COLOR:       return vangfx::d3d12::BlendFactor::DestColor;
    case D3D12_BLEND_INV_DEST_COLOR:   return vangfx::d3d12::BlendFactor::InvDestColor;
    case D3D12_BLEND_SRC_ALPHA_SAT:    return vangfx::d3d12::BlendFactor::SrcAlphaSat;
    case D3D12_BLEND_BLEND_FACTOR:     return vangfx::d3d12::BlendFactor::BlendFactor_;
    case D3D12_BLEND_INV_BLEND_FACTOR: return vangfx::d3d12::BlendFactor::InvBlendFactor;
    case D3D12_BLEND_SRC1_COLOR:       return vangfx::d3d12::BlendFactor::Src1Color;
    case D3D12_BLEND_INV_SRC1_COLOR:   return vangfx::d3d12::BlendFactor::InvSrc1Color;
    case D3D12_BLEND_SRC1_ALPHA:       return vangfx::d3d12::BlendFactor::Src1Alpha;
    case D3D12_BLEND_INV_SRC1_ALPHA:   return vangfx::d3d12::BlendFactor::InvSrc1Alpha;
    default:                           return vangfx::d3d12::BlendFactor::One;
    }
}

vangfx::d3d12::BlendOp to_blend_op(D3D12_BLEND_OP op) noexcept {
    switch (op) {
    case D3D12_BLEND_OP_SUBTRACT:     return vangfx::d3d12::BlendOp::Subtract;
    case D3D12_BLEND_OP_REV_SUBTRACT: return vangfx::d3d12::BlendOp::RevSubtract;
    case D3D12_BLEND_OP_MIN:          return vangfx::d3d12::BlendOp::Min;
    case D3D12_BLEND_OP_MAX:          return vangfx::d3d12::BlendOp::Max;
    default:                          return vangfx::d3d12::BlendOp::Add;
    }
}

ShaderBytecodeView view(const D3D12_SHADER_BYTECODE& b) noexcept { return { b.pShaderBytecode, b.BytecodeLength }; }
void               apply(D3D12_SHADER_BYTECODE& b, const ShaderBytecodeView& v) noexcept { b.pShaderBytecode = v.data; b.BytecodeLength = v.size; }
uint64_t           hash_of(const D3D12_SHADER_BYTECODE& b) noexcept { return hash_bytes(b.pShaderBytecode, b.BytecodeLength); }

void fill_elements(const D3D12_INPUT_LAYOUT_DESC& layout, std::vector<InputElement>& out, GraphicsPSODesc& g) {
    out.clear();
    if (!layout.pInputElementDescs) return;
    out.reserve(layout.NumElements);
    for (UINT i = 0; i < layout.NumElements; ++i) {
        const auto& s = layout.pInputElementDescs[i];
        InputElement e{};
        e.semantic_name  = s.SemanticName;
        e.semantic_index = s.SemanticIndex;
        e.format         = fmtmap::from_dxgi(s.Format);
        e.input_slot     = s.InputSlot;
        e.aligned_offset = s.AlignedByteOffset;
        e.classification = to_classification(s.InputSlotClass);
        e.instance_step  = s.InstanceDataStepRate;
        out.push_back(e);
    }
    g.input_elements      = out.data();
    g.input_element_count = static_cast<uint32_t>(out.size());
}

void fill_raster(GraphicsPSODesc& g, const D3D12_RASTERIZER_DESC& r) noexcept {
    g.rasterizer.fill_mode               = r.FillMode == D3D12_FILL_MODE_WIREFRAME ? vangfx::d3d12::FillMode::Wireframe
                                                                                   : vangfx::d3d12::FillMode::Solid;
    g.rasterizer.cull_mode               = to_cull(r.CullMode);
    g.rasterizer.front_counter_clockwise = r.FrontCounterClockwise != FALSE;
    g.rasterizer.depth_bias              = r.DepthBias;
    g.rasterizer.depth_bias_clamp        = r.DepthBiasClamp;
    g.rasterizer.slope_scaled_depth_bias = r.SlopeScaledDepthBias;
    g.rasterizer.depth_clip_enable       = r.DepthClipEnable != FALSE;
    g.rasterizer.multisample_enable      = r.MultisampleEnable != FALSE;
    g.rasterizer.antialiased_line_enable = r.AntialiasedLineEnable != FALSE;
    g.rasterizer.forced_sample_count     = r.ForcedSampleCount;
    g.rasterizer.conservative_raster     = r.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
}

void fill_blend(GraphicsPSODesc& g, const D3D12_BLEND_DESC& b) noexcept {
    g.blend.alpha_to_coverage = b.AlphaToCoverageEnable != FALSE;
    g.blend.independent_blend = b.IndependentBlendEnable != FALSE;
    for (int i = 0; i < 8; ++i) {
        const auto& s = b.RenderTarget[i];
        auto& d = g.blend.rt[i];
        d.blend_enable     = s.BlendEnable != FALSE;
        d.write_mask       = s.RenderTargetWriteMask;
        d.src_blend        = to_blend(s.SrcBlend);
        d.dest_blend       = to_blend(s.DestBlend);
        d.blend_op         = to_blend_op(s.BlendOp);
        d.src_blend_alpha  = to_blend(s.SrcBlendAlpha);
        d.dest_blend_alpha = to_blend(s.DestBlendAlpha);
        d.blend_op_alpha   = to_blend_op(s.BlendOpAlpha);
    }
}

void fill_depth(GraphicsPSODesc& g, BOOL enable, D3D12_DEPTH_WRITE_MASK mask, D3D12_COMPARISON_FUNC func,
                BOOL stencil, UINT8 read_mask, UINT8 write_mask) noexcept {
    g.depth_stencil.depth_enable       = enable != FALSE;
    g.depth_stencil.depth_write        = mask == D3D12_DEPTH_WRITE_MASK_ALL;
    g.depth_stencil.depth_func         = to_compare(func);
    g.depth_stencil.stencil_enable     = stencil != FALSE;
    g.depth_stencil.stencil_read_mask  = read_mask;
    g.depth_stencil.stencil_write_mask = write_mask;
}

void fill_graphics(GraphicsPSODesc& g, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, std::vector<InputElement>& elements) {
    g.root_signature = { d.pRootSignature ? id_of(d.pRootSignature) : 0 };
    g.vs = view(d.VS); g.ps = view(d.PS); g.gs = view(d.GS); g.hs = view(d.HS); g.ds = view(d.DS);
    fill_elements(d.InputLayout, elements, g);
    fill_raster(g, d.RasterizerState);
    fill_blend(g, d.BlendState);
    const auto& ds = d.DepthStencilState;
    fill_depth(g, ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc, ds.StencilEnable, ds.StencilReadMask, ds.StencilWriteMask);
    g.num_render_targets = d.NumRenderTargets;
    for (UINT i = 0; i < 8; ++i) g.rtv_formats[i] = fmtmap::from_dxgi(d.RTVFormats[i]);
    g.dsv_format     = fmtmap::from_dxgi(d.DSVFormat);
    g.sample_count   = d.SampleDesc.Count;
    g.sample_quality = d.SampleDesc.Quality;
    g.topology_type  = static_cast<uint8_t>(d.PrimitiveTopologyType);
    g.node_mask      = d.NodeMask;
    g.flags          = static_cast<uint32_t>(d.Flags);
}

// ── Shared plumbing ──────────────────────────────────────────────────────────

bool anyone_wants_pso(const ContextSet& cs) noexcept {
    for (D3D12ContextImpl* c : cs) if (c->wants_pso()) return true;
    return false;
}

bool anyone_records(const ContextSet& cs) noexcept {
    for (D3D12ContextImpl* c : cs) if (c->records_pipelines()) return true;
    return false;
}

// Returns whether a callback blocked the creation.
bool dispatch(const ContextSet& cs, PSOCreateEvent& ev) {
    for (D3D12ContextImpl* ctx : cs) {
        if (!ctx->wants_pso()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index());
        ctx->dispatch_pso_create(fc, ev);
    }
    return ev.intercept;
}

// The pipeline behind whatever interface the application asked for.
ID3D12PipelineState* pipeline_of(void* out) noexcept {
    if (!out) return nullptr;
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(static_cast<IUnknown*>(out)->QueryInterface(IID_PPV_ARGS(&pso)))) return nullptr;
    pso->Release();   // the application holds it
    return pso;
}

HRESULT blocked(void** out) noexcept {
    if (out) *out = nullptr;
    return E_ABORT;
}

// ── Graphics ─────────────────────────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE hook_create_gfx(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                          REFIID riid, void** out) {
    auto orig = s_gfx.original<CreateGfxFn>(self);
    if (hub_idle() || internal() || !desc) return orig(self, desc, riid, out);

    HubReader reader;
    const ContextSet cs = contexts_for(self);
    if (!cs) return orig(self, desc, riid, out);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
    const uint64_t id = ContextImpl::alloc_id();
    if (anyone_wants_pso(cs)) {
        std::vector<InputElement> elements;
        GraphicsPSODesc g{};
        fill_graphics(g, d, elements);
        PSOCreateEvent ev{};
        ev.kind     = PSOKind::Graphics;
        ev.pso      = { id };
        ev.graphics = &g;
        if (dispatch(cs, ev)) return blocked(out);
        apply(d.VS, g.vs); apply(d.PS, g.ps); apply(d.GS, g.gs); apply(d.HS, g.hs); apply(d.DS, g.ds);
    }

    const HRESULT hr = orig(self, &d, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (ID3D12PipelineState* pso = pipeline_of(*out)) {
            PsoRecord rec{ id, hash_of(d.VS), hash_of(d.PS), false, nullptr };
            if (anyone_records(cs)) rec.blueprint = PsoBlueprint::record(d);
            pso_register(pso, std::move(rec));
        }
    }
    return hr;
}

// ── Compute ──────────────────────────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE hook_create_cmp(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                          REFIID riid, void** out) {
    auto orig = s_cmp.original<CreateCmpFn>(self);
    if (hub_idle() || internal() || !desc) return orig(self, desc, riid, out);

    HubReader reader;
    const ContextSet cs = contexts_for(self);
    if (!cs) return orig(self, desc, riid, out);

    D3D12_COMPUTE_PIPELINE_STATE_DESC d = *desc;
    const uint64_t id = ContextImpl::alloc_id();
    if (anyone_wants_pso(cs)) {
        ComputePSODesc c{};
        c.root_signature = { d.pRootSignature ? id_of(d.pRootSignature) : 0 };
        c.cs             = view(d.CS);
        c.node_mask      = d.NodeMask;
        c.flags          = static_cast<uint32_t>(d.Flags);
        PSOCreateEvent ev{};
        ev.kind    = PSOKind::Compute;
        ev.pso     = { id };
        ev.compute = &c;
        if (dispatch(cs, ev)) return blocked(out);
        apply(d.CS, c.cs);
    }

    const HRESULT hr = orig(self, &d, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (ID3D12PipelineState* pso = pipeline_of(*out))
            pso_register(pso, PsoRecord{ id, 0, hash_of(d.CS), true, nullptr });
    }
    return hr;
}

// ── Pipeline state streams ───────────────────────────────────────────────────
//
// A stream is a run of { type, payload } records, each padded to a pointer
// boundary. The payload follows the type at the payload's own alignment -- a
// UINT sample mask sits right after the 4-byte type, a pointer-bearing payload
// four bytes later -- which is what CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT lays
// out. (The old parser put every payload on a pointer boundary, which misread a
// sample mask, a format or the flags and then lost its place for the rest of the
// stream.) An unrecognised type stops the walk: its size is what says where the
// next record starts.

struct PayloadShape { size_t size = 0, align = 0; };

PayloadShape shape_of(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) noexcept {
#define VANGFX_SHAPE(T) PayloadShape{ sizeof(T), alignof(T) }
    switch (t) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:        return VANGFX_SHAPE(ID3D12RootSignature*);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:                    return VANGFX_SHAPE(D3D12_SHADER_BYTECODE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:         return VANGFX_SHAPE(D3D12_STREAM_OUTPUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:                 return VANGFX_SHAPE(D3D12_BLEND_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:           return VANGFX_SHAPE(UINT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:            return VANGFX_SHAPE(D3D12_RASTERIZER_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:         return VANGFX_SHAPE(D3D12_DEPTH_STENCIL_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:          return VANGFX_SHAPE(D3D12_INPUT_LAYOUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:    return VANGFX_SHAPE(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:    return VANGFX_SHAPE(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return VANGFX_SHAPE(D3D12_RT_FORMAT_ARRAY);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:  return VANGFX_SHAPE(DXGI_FORMAT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:           return VANGFX_SHAPE(DXGI_SAMPLE_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:             return VANGFX_SHAPE(UINT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:            return VANGFX_SHAPE(D3D12_CACHED_PIPELINE_STATE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:                 return VANGFX_SHAPE(D3D12_PIPELINE_STATE_FLAGS);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:        return VANGFX_SHAPE(D3D12_DEPTH_STENCIL_DESC1);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:       return VANGFX_SHAPE(D3D12_VIEW_INSTANCING_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:        return VANGFX_SHAPE(D3D12_DEPTH_STENCIL_DESC2);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:           return VANGFX_SHAPE(D3D12_RASTERIZER_DESC1);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:           return VANGFX_SHAPE(D3D12_RASTERIZER_DESC2);
    default:                                                        return {};
    }
#undef VANGFX_SHAPE
}

constexpr size_t round_up(size_t v, size_t a) noexcept { return (v + a - 1) & ~(a - 1); }

enum Stage : int { kVS, kPS, kGS, kHS, kDS, kAS, kMS, kCS, kStages };

struct Stream {
    GraphicsPSODesc               g{};
    ComputePSODesc                c{};
    std::vector<InputElement>     elements;
    ID3D12RootSignature*          root = nullptr;
    size_t                        stage_at[kStages];   // payload offset of each stage's bytecode, or SIZE_MAX
    bool                          complete = true;
    Stream() { for (auto& s : stage_at) s = SIZE_MAX; }

    [[nodiscard]] bool compute() const noexcept { return stage_at[kCS] != SIZE_MAX; }
    [[nodiscard]] bool mesh()    const noexcept { return stage_at[kMS] != SIZE_MAX; }
};

void parse(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, Stream& s) {
    const auto* base = static_cast<const uint8_t*>(desc.pPipelineStateSubobjectStream);
    const size_t end = desc.SizeInBytes;
    size_t at = 0;
    while (base && at + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, base + at, sizeof(type));
        const PayloadShape sh = shape_of(type);
        if (!sh.size) { s.complete = false; return; }
        const size_t p = at + round_up(sizeof(type), sh.align);
        if (p + sh.size > end) { s.complete = false; return; }
        const void* payload = base + p;

        auto bytecode = [&](Stage st, ShaderBytecodeView& v) {
            v = view(*static_cast<const D3D12_SHADER_BYTECODE*>(payload));
            s.stage_at[st] = p;
        };
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            s.root = *static_cast<ID3D12RootSignature* const*>(payload); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: bytecode(kVS, s.g.vs); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: bytecode(kPS, s.g.ps); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: bytecode(kGS, s.g.gs); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: bytecode(kHS, s.g.hs); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: bytecode(kDS, s.g.ds); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: bytecode(kAS, s.g.as); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: bytecode(kMS, s.g.ms); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: bytecode(kCS, s.c.cs); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            fill_blend(s.g, *static_cast<const D3D12_BLEND_DESC*>(payload)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
            fill_raster(s.g, *static_cast<const D3D12_RASTERIZER_DESC*>(payload)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
            const auto& d = *static_cast<const D3D12_DEPTH_STENCIL_DESC*>(payload);
            fill_depth(s.g, d.DepthEnable, d.DepthWriteMask, d.DepthFunc, d.StencilEnable, d.StencilReadMask, d.StencilWriteMask);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
            const auto& d = *static_cast<const D3D12_DEPTH_STENCIL_DESC1*>(payload);
            fill_depth(s.g, d.DepthEnable, d.DepthWriteMask, d.DepthFunc, d.StencilEnable, d.StencilReadMask, d.StencilWriteMask);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            fill_elements(*static_cast<const D3D12_INPUT_LAYOUT_DESC*>(payload), s.elements, s.g); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
            const auto& rt = *static_cast<const D3D12_RT_FORMAT_ARRAY*>(payload);
            s.g.num_render_targets = rt.NumRenderTargets;
            for (UINT i = 0; i < 8; ++i) s.g.rtv_formats[i] = fmtmap::from_dxgi(rt.RTFormats[i]);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            s.g.dsv_format = fmtmap::from_dxgi(*static_cast<const DXGI_FORMAT*>(payload)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: {
            const auto& sd = *static_cast<const DXGI_SAMPLE_DESC*>(payload);
            s.g.sample_count = sd.Count; s.g.sample_quality = sd.Quality;
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            s.g.topology_type = static_cast<uint8_t>(*static_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE*>(payload)); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            s.g.node_mask = s.c.node_mask = *static_cast<const UINT*>(payload); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            s.g.flags = s.c.flags = static_cast<uint32_t>(*static_cast<const D3D12_PIPELINE_STATE_FLAGS*>(payload)); break;
        default: break;
        }
        at = round_up(p + sh.size, alignof(void*));
    }
}

HRESULT STDMETHODCALLTYPE hook_create_stream(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                             REFIID riid, void** out) {
    auto orig = s_stream.original<CreateStreamFn>(self);
    if (hub_idle() || internal() || !desc) return orig(self, desc, riid, out);

    HubReader reader;
    const ContextSet cs = contexts_for(self);
    if (!cs) return orig(self, desc, riid, out);

    Stream s;
    parse(*desc, s);
    const uint64_t id = ContextImpl::alloc_id();
    const D3D12_PIPELINE_STATE_STREAM_DESC* use = desc;
    std::vector<uint8_t>             patched;
    D3D12_PIPELINE_STATE_STREAM_DESC patched_desc{};

    if (anyone_wants_pso(cs)) {
        const RootSignatureHandle rs{ s.root ? id_of(s.root) : 0 };
        s.g.root_signature = rs;
        s.c.root_signature = rs;
        PSOCreateEvent ev{};
        ev.pso = { id };
        if (s.compute()) { ev.kind = PSOKind::Compute;    ev.compute  = &s.c; }
        else             { ev.kind = s.mesh() ? PSOKind::MeshShader : PSOKind::Graphics; ev.graphics = &s.g; }
        if (!s.complete) {
            for (D3D12ContextImpl* ctx : cs)
                ctx->log(LogLevel::Debug, "D3D12: a pipeline state stream held a subobject this build does not "
                                          "know; what came before it is reported.");
        }
        if (dispatch(cs, ev)) return blocked(out);

        // Bytecode a callback swapped is written into a copy of the stream.
        const ShaderBytecodeView now[kStages] = { s.g.vs, s.g.ps, s.g.gs, s.g.hs, s.g.ds, s.g.as, s.g.ms, s.c.cs };
        const auto* base = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
        for (int st = 0; st < kStages; ++st) {
            if (s.stage_at[st] == SIZE_MAX) continue;
            D3D12_SHADER_BYTECODE was;
            std::memcpy(&was, base + s.stage_at[st], sizeof(was));
            if (was.pShaderBytecode == now[st].data && was.BytecodeLength == now[st].size) continue;
            if (patched.empty()) patched.assign(base, base + desc->SizeInBytes);
            const D3D12_SHADER_BYTECODE b{ now[st].data, now[st].size };
            std::memcpy(patched.data() + s.stage_at[st], &b, sizeof(b));
        }
        if (!patched.empty()) {
            patched_desc = { patched.size(), patched.data() };
            use = &patched_desc;
        }
    }

    const HRESULT hr = orig(self, use, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (ID3D12PipelineState* pso = pipeline_of(*out)) {
            PsoRecord rec{};
            rec.id = id;
            if (s.compute()) {
                rec.compute = true;
                rec.ps_hash = hash_bytes(s.c.cs.data, s.c.cs.size);
            } else {
                const ShaderBytecodeView& front = s.mesh() ? s.g.ms : s.g.vs;
                rec.vs_hash = hash_bytes(front.data, front.size);
                rec.ps_hash = hash_bytes(s.g.ps.data, s.g.ps.size);
            }
            pso_register(pso, std::move(rec));
        }
    }
    return hr;
}

} // namespace

void install_pso_hooks(ID3D12Device* device) noexcept {
    s_gfx.install(device, &hook_create_gfx);
    s_cmp.install(device, &hook_create_cmp);
    ID3D12Device2* d2 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
        s_stream.install(d2, &hook_create_stream);
        d2->Release();
    }
}

void uninstall_pso_hooks() noexcept {
    s_gfx.uninstall();
    s_cmp.uninstall();
    s_stream.uninstall();
}

} // namespace vangfx::backend::d3d12
