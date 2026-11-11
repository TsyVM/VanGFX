/**
 * src/backend/d3d12/effect_bridge.cpp — per-draw pipeline swaps for Effects.
 *
 * See the header for why bind_effect becomes a pipeline swap on this API.
 */

#include "effect_bridge.hpp"
#include "d3d12_impl.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d12 {

// ─────────────────────────────────────────────────────────────────────────────
//  PsoBlueprint
// ─────────────────────────────────────────────────────────────────────────────

PsoBlueprint::~PsoBlueprint() {
    if (desc.pRootSignature) desc.pRootSignature->Release();
}

std::unique_ptr<PsoBlueprint> PsoBlueprint::record(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& src) {
    auto bp = std::make_unique<PsoBlueprint>();
    bp->desc = src;

    auto copy = [](const D3D12_SHADER_BYTECODE& in, std::vector<uint8_t>& store, D3D12_SHADER_BYTECODE& out) {
        if (in.pShaderBytecode && in.BytecodeLength) {
            const auto* p = static_cast<const uint8_t*>(in.pShaderBytecode);
            store.assign(p, p + in.BytecodeLength);
            out = { store.data(), store.size() };
        } else {
            out = {};
        }
    };
    copy(src.VS, bp->vs, bp->desc.VS);
    copy(src.PS, bp->ps, bp->desc.PS);
    copy(src.GS, bp->gs, bp->desc.GS);
    copy(src.HS, bp->hs, bp->desc.HS);
    copy(src.DS, bp->ds, bp->desc.DS);

    // Semantic names are strings the application owns; each gets its own copy,
    // and the element descriptions are pointed at those once the vector of
    // strings has stopped moving.
    if (src.InputLayout.pInputElementDescs && src.InputLayout.NumElements) {
        const UINT n = src.InputLayout.NumElements;
        bp->elements.assign(src.InputLayout.pInputElementDescs, src.InputLayout.pInputElementDescs + n);
        bp->semantics.reserve(n);
        for (UINT i = 0; i < n; ++i)
            bp->semantics.emplace_back(bp->elements[i].SemanticName ? bp->elements[i].SemanticName : "");
        for (UINT i = 0; i < n; ++i) bp->elements[i].SemanticName = bp->semantics[i].c_str();
        bp->desc.InputLayout = { bp->elements.data(), n };
    } else {
        bp->desc.InputLayout = {};
    }

    const auto& so = src.StreamOutput;
    if (so.pSODeclaration && so.NumEntries) {
        bp->so_entries.assign(so.pSODeclaration, so.pSODeclaration + so.NumEntries);
        bp->so_semantics.reserve(so.NumEntries);
        for (UINT i = 0; i < so.NumEntries; ++i)
            bp->so_semantics.emplace_back(bp->so_entries[i].SemanticName ? bp->so_entries[i].SemanticName : "");
        for (UINT i = 0; i < so.NumEntries; ++i)
            if (bp->so_entries[i].SemanticName) bp->so_entries[i].SemanticName = bp->so_semantics[i].c_str();
        if (so.pBufferStrides && so.NumStrides)
            bp->so_strides.assign(so.pBufferStrides, so.pBufferStrides + so.NumStrides);
        bp->desc.StreamOutput.pSODeclaration = bp->so_entries.data();
        bp->desc.StreamOutput.pBufferStrides = bp->so_strides.empty() ? nullptr : bp->so_strides.data();
        bp->desc.StreamOutput.NumStrides     = static_cast<UINT>(bp->so_strides.size());
    } else {
        bp->desc.StreamOutput = {};
    }

    // A cached blob describes the original shaders; offered with different ones
    // it is refused, so the rebuilt description never carries it.
    bp->desc.CachedPSO = {};
    if (bp->desc.pRootSignature) bp->desc.pRootSignature->AddRef();
    return bp;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoBlueprint::with(const std::vector<uint8_t>* new_vs,
                                                      const std::vector<uint8_t>* new_ps) const noexcept {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = desc;
    if (new_vs && !new_vs->empty()) d.VS = { new_vs->data(), new_vs->size() };
    if (new_ps && !new_ps->empty()) d.PS = { new_ps->data(), new_ps->size() };
    return d;
}

namespace effect_bridge {

namespace {

struct Shadow {
    uint64_t             effect_id = 0;
    D3D12ContextImpl*    ctx       = nullptr;
    ID3D12PipelineState* pso       = nullptr;   // null: the runtime refused it
};

struct Retired {
    D3D12ContextImpl*    ctx   = nullptr;
    ID3D12PipelineState* pso   = nullptr;
    uint32_t             frame = 0;   // the context's frame index when retired
};

// Frames a retired shadow is kept after its last possible use. A game keeps at
// most two or three frames in flight; the release waits one more.
constexpr uint32_t kRetireFrames = 4;

std::shared_mutex                                   g_mutex;
std::unordered_map<uint64_t, std::vector<Shadow>>   g_shadows;   // app pipeline id → its shadows
std::vector<Retired>                                g_retired;
std::unordered_map<const void*, bool>               g_warned_unrecorded;  // per context

enum class Lookup { Missing, Ready, Refused };

Lookup find(uint64_t pso_id, uint64_t effect_id, ID3D12PipelineState*& out) {
    std::shared_lock lk(g_mutex);
    auto it = g_shadows.find(pso_id);
    if (it == g_shadows.end()) return Lookup::Missing;
    for (const Shadow& s : it->second)
        if (s.effect_id == effect_id) { out = s.pso; return s.pso ? Lookup::Ready : Lookup::Refused; }
    return Lookup::Missing;
}

}  // namespace

Result<void> bind(D3D12ContextImpl& ctx, D3D12FrameContextImpl& frame, const Effect& e) {
    ListState* st = frame.list_state();
    ID3D12GraphicsCommandList* list = frame.list();
    if (!st || !list) {
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D12: bind_effect swaps the pipeline of the draw being reported, so it "
            "works only inside an on_draw callback.");
    }
    if (!is_valid(e.handle)) {
        return gfx_err<void>(ShaderError::InvalidHandle,
            "D3D12: the Effect has no handle. It was not produced by Context::link_effect.");
    }
    if (is_valid(e.cs.handle) || is_valid(e.gs.handle) || is_valid(e.hs.handle) || is_valid(e.ds.handle)) {
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D12: only the vertex and pixel stages of a graphics pipeline can be swapped.");
    }

    ID3D12PipelineState* app = st->restore_pso ? st->restore_pso : st->pso_raw;
    if (!app) {
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D12: the command list has no pipeline state set for this draw.");
    }
    const PsoRecord rec = pso_find(app);
    if (rec.compute) {
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D12: the pipeline bound for this call is a compute pipeline.");
    }
    if (!rec.blueprint) {
        bool first = false;
        {
            std::unique_lock lk(g_mutex);
            first = !g_warned_unrecorded[&ctx];
            g_warned_unrecorded[&ctx] = true;
        }
        if (first) {
            ctx.log(LogLevel::Warn,
                "D3D12: a draw matched an Effect but its pipeline was created before "
                "pipeline recording started (it starts when the first Effect is linked), "
                "or was built from a pipeline state stream. Such draws keep their own "
                "shaders. Link Effects before the game builds its pipelines -- attaching "
                "at device creation (vangfx::AutoAttach) does that. Reported once.");
        }
        return gfx_err<void>(ShaderError::Unsupported,
            "D3D12: this draw's pipeline was not recorded, so it cannot be rebuilt.");
    }

    ID3D12PipelineState* shadow = nullptr;
    switch (find(rec.id, e.handle.id, shadow)) {
    case Lookup::Ready:
        break;
    case Lookup::Refused:
        return gfx_err<void>(ShaderError::CompileFailed,
            "D3D12: the runtime refused this pipeline with the Effect's shaders.");
    case Lookup::Missing: {
        // The replacement has to be in the same container as the game's own
        // shaders: a pipeline mixing DXBC and DXIL is refused.
        const PsoBlueprint& bp = *rec.blueprint;
        const bool dxil = !bp.vs.empty() ? bytecode_is_dxil(bp.vs.data(), bp.vs.size())
                                         : bytecode_is_dxil(bp.ps.data(), bp.ps.size());
        std::string why;
        std::vector<uint8_t> vs, ps;
        if (is_valid(e.vs.handle)) vs = ctx.shader_bytecode_as(e.vs.handle, dxil, why);
        if (is_valid(e.ps.handle)) ps = ctx.shader_bytecode_as(e.ps.handle, dxil, why);
        if ((is_valid(e.vs.handle) && vs.empty()) || (is_valid(e.ps.handle) && ps.empty()) || (vs.empty() && ps.empty())) {
            std::unique_lock lk(g_mutex);
            g_shadows[rec.id].push_back({ e.handle.id, &ctx, nullptr });   // do not try again for this pipeline
            return gfx_err<void>(ShaderError::CompileFailed,
                "D3D12: the Effect's shaders are not available in the form this pipeline needs: " + why);
        }
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC d = rec.blueprint->with(&vs, &ps);
        ID3D12PipelineState* built = nullptr;
        HRESULT hr = E_FAIL;
        {
            InternalScope internal_work;
            hr = ctx.device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&built));
        }
        bool inserted = true;
        {
            std::unique_lock lk(g_mutex);
            auto& list_of = g_shadows[rec.id];
            for (const Shadow& s : list_of)
                if (s.effect_id == e.handle.id) { inserted = false; shadow = s.pso; break; }
            if (inserted) {
                list_of.push_back({ e.handle.id, &ctx, SUCCEEDED(hr) ? built : nullptr });
                shadow = SUCCEEDED(hr) ? built : nullptr;
            }
        }
        if (!inserted && built) built->Release();   // another thread got there first
        if (inserted && FAILED(hr)) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "D3D12: CreateGraphicsPipelineState refused pipeline %llu with Effect %llu "
                "(HRESULT 0x%08lX). The replacement most likely does not match the root "
                "signature -- the same constant buffers, textures and samplers must sit in "
                "the same registers. That pipeline's draws keep their own shaders.",
                static_cast<unsigned long long>(rec.id),
                static_cast<unsigned long long>(e.handle.id),
                static_cast<unsigned long>(hr));
            ctx.log(LogLevel::Error, msg);
        }
        if (!shadow) {
            return gfx_err<void>(ShaderError::CompileFailed,
                "D3D12: the runtime refused this pipeline with the Effect's shaders.");
        }
        break;
    }
    }

    if (!st->restore_pso) st->restore_pso = app;
    set_pipeline_unreported(list, shadow);
    return {};
}

void unbind(D3D12FrameContextImpl& frame) noexcept {
    ListState* st = frame.list_state();
    ID3D12GraphicsCommandList* list = frame.list();
    if (!st || !list || !st->restore_pso) return;
    set_pipeline_unreported(list, st->restore_pso);
    st->restore_pso = nullptr;
}

void pipeline_destroyed(uint64_t pso_id) noexcept {
    std::vector<Shadow> gone;
    {
        std::unique_lock lk(g_mutex);
        auto it = g_shadows.find(pso_id);
        if (it == g_shadows.end()) return;
        gone.swap(it->second);
        g_shadows.erase(it);
    }
    for (Shadow& s : gone) if (s.pso) s.pso->Release();
}

void effect_destroyed(D3D12ContextImpl& ctx, uint64_t effect_id) noexcept {
    const uint32_t now = ctx.current_frame_index();
    std::unique_lock lk(g_mutex);
    for (auto& [pso_id, list_of] : g_shadows) {
        for (size_t i = 0; i < list_of.size();) {
            if (list_of[i].effect_id == effect_id && list_of[i].ctx == &ctx) {
                if (list_of[i].pso) g_retired.push_back({ &ctx, list_of[i].pso, now });
                list_of[i] = list_of.back();
                list_of.pop_back();
            } else {
                ++i;
            }
        }
    }
}

void context_destroyed(D3D12ContextImpl& ctx) noexcept {
    std::vector<ID3D12PipelineState*> gone;
    {
        std::unique_lock lk(g_mutex);
        for (auto it = g_shadows.begin(); it != g_shadows.end();) {
            auto& list_of = it->second;
            for (size_t i = 0; i < list_of.size();) {
                if (list_of[i].ctx == &ctx) {
                    if (list_of[i].pso) gone.push_back(list_of[i].pso);
                    list_of[i] = list_of.back();
                    list_of.pop_back();
                } else {
                    ++i;
                }
            }
            it = list_of.empty() ? g_shadows.erase(it) : std::next(it);
        }
        for (size_t i = 0; i < g_retired.size();) {
            if (g_retired[i].ctx == &ctx) {
                gone.push_back(g_retired[i].pso);
                g_retired[i] = g_retired.back();
                g_retired.pop_back();
            } else {
                ++i;
            }
        }
        g_warned_unrecorded.erase(&ctx);
    }
    // The context waited for its queue to drain before calling this.
    for (ID3D12PipelineState* p : gone) p->Release();
}

void collect(D3D12ContextImpl& ctx) noexcept {
    const uint32_t now = ctx.current_frame_index();
    std::vector<ID3D12PipelineState*> gone;
    {
        std::unique_lock lk(g_mutex);
        for (size_t i = 0; i < g_retired.size();) {
            if (g_retired[i].ctx == &ctx && now - g_retired[i].frame >= kRetireFrames) {
                gone.push_back(g_retired[i].pso);
                g_retired[i] = g_retired.back();
                g_retired.pop_back();
            } else {
                ++i;
            }
        }
    }
    for (ID3D12PipelineState* p : gone) p->Release();
}

}  // namespace effect_bridge
}  // namespace vangfx::backend::d3d12
