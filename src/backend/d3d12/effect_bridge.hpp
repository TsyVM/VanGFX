#pragma once

/**
 * src/backend/d3d12/effect_bridge.hpp — how an Effect becomes visible on D3D12.
 *
 * THE PROBLEM
 *
 * FrameContext::bind_effect is written for an API where a shader can be bound:
 * D3D9 has SetPixelShader, D3D11 has PSSetShader, and the effect layer calls
 * one of them per draw when a ReplaceFilter matches. D3D12 has neither. A
 * shader exists only inside a pipeline state object, and a pipeline state is
 * immutable once created.
 *
 * THE ANSWER
 *
 * Swap the pipeline for the one draw. When a pipeline state is created while
 * any context on its device has linked an Effect, pso.cpp keeps a PsoBlueprint:
 * a deep copy of the description it was created from (bytecode, input layout,
 * stream output, root signature). bind_effect, called from a draw callback,
 * builds a "shadow" of the pipeline bound on that command list with the
 * Effect's stages in place of the originals (once per pipeline and Effect, then
 * cached), sets it on the list without reporting the call, and leaves the
 * application's pipeline in ListState::restore_pso; the draw thunk puts it back
 * after the draw. So a ReplaceFilter selects single draws on D3D12 exactly as it
 * does on the other two backends.
 *
 * WHAT IT COSTS
 *
 *   Memory: a blueprint holds a copy of every shader of every pipeline created
 *   while recording is on. Recording starts when the first Effect is linked on
 *   a context (link_effect) and applies to pipelines created from then on; a
 *   draw using a pipeline created earlier cannot be swapped, and bind_effect
 *   says so.
 *
 *   A hitch the first time: building a shadow pipeline is a driver compile, on
 *   the thread recording the draw, once per (pipeline, Effect) pair.
 *
 * ROOT SIGNATURES
 *
 * A replacement has to fit the root signature the original was built against --
 * the same constant buffers, textures and samplers in the same registers --
 * because the root signature is not replaced. When it does not fit, the runtime
 * refuses the shadow pipeline; the draw then runs with the application's own
 * pipeline, bind_effect returns the error, and the refusal is logged once per
 * (pipeline, Effect).
 */

#include <vangfx/core/error.hpp>
#include <vangfx/core/shader.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vangfx::backend::d3d12 {

class D3D12ContextImpl;
class D3D12FrameContextImpl;

// Everything a D3D12_GRAPHICS_PIPELINE_STATE_DESC points at, owned, so the
// description can be rebuilt after the application's copy is gone.
struct PsoBlueprint {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};   // pointers aimed at the members below
    std::vector<uint8_t> vs, ps, gs, hs, ds;
    std::vector<D3D12_INPUT_ELEMENT_DESC>   elements;
    std::vector<std::string>                semantics;
    std::vector<D3D12_SO_DECLARATION_ENTRY> so_entries;
    std::vector<std::string>                so_semantics;
    std::vector<UINT>                       so_strides;

    PsoBlueprint() = default;
    PsoBlueprint(const PsoBlueprint&) = delete;
    PsoBlueprint& operator=(const PsoBlueprint&) = delete;
    ~PsoBlueprint();

    // Copies `src`. The root signature is AddRef'd and held.
    static std::unique_ptr<PsoBlueprint> record(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& src);

    // The description with `vs` / `ps` in place of the originals where non-empty.
    [[nodiscard]] D3D12_GRAPHICS_PIPELINE_STATE_DESC with(const std::vector<uint8_t>* vs,
                                                          const std::vector<uint8_t>* ps) const noexcept;
};

namespace effect_bridge {

// Swap the pipeline bound on the frame's command list for one running `e`'s
// stages, for the draw being reported. Fails, leaving the draw as it was, when
// the call is not from a draw callback, the pipeline was created before
// recording started, or the runtime refuses the shadow pipeline.
Result<void> bind(D3D12ContextImpl& ctx, D3D12FrameContextImpl& frame, const Effect& e);

// Put the application's pipeline back now, if this draw had one swapped.
void unbind(D3D12FrameContextImpl& frame) noexcept;

// An application pipeline is gone: release the shadows built from it. The
// application guarantees the GPU is done with the pipeline, and so with every
// draw that used a shadow in its place.
void pipeline_destroyed(uint64_t pso_id) noexcept;

// An Effect was destroyed, or a context is going away: release their shadows
// once the GPU has finished the frames that may still use them.
void effect_destroyed(D3D12ContextImpl& ctx, uint64_t effect_id) noexcept;
void context_destroyed(D3D12ContextImpl& ctx) noexcept;

// Called once per presented frame: releases retired shadows old enough that
// no submitted work can still reference them.
void collect(D3D12ContextImpl& ctx) noexcept;

}  // namespace effect_bridge
}  // namespace vangfx::backend::d3d12
