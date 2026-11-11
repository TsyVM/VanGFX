#pragma once

/**
 * vangfx/vangfx.hpp — VanGFX SDK single-include entry point.
 *
 * Observe and change what a Direct3D 9, 11 or 12 game draws, from a mod loaded
 * into its process: draw, resource, render-target, state, query and present
 * events; readback; per-draw shader replacement; post-processing passes.
 *
 *   #include <vangfx/vangfx.hpp>
 *
 * Building against it (CMake):
 *   add_subdirectory(VanGFX)                 # or find_package(vangfx) on an SDK drop
 *   target_link_libraries(my_mod PRIVATE vangfx::vangfx)
 *
 * Without CMake: include/ on the include path, lib/vangfx.lib, the static CRT
 * (/MT), C++23, and d3dcompiler.lib dxgi.lib dxguid.lib plus the libraries of
 * the backends the build has (vangfx/config.hpp): d3d9.lib, d3d11.lib,
 * d3d12.lib, xaudio2.lib dsound.lib.
 *
 * Built and maintained by TeamVanilla — https://www.teamvanilla.org/
 */

#include <vangfx/config.hpp> // which backends this library was built with

// ── Core ─────────────────────────────────────────────────────────────────────
#include "core/error.hpp"
#include "core/shader.hpp"   // Shader, ShaderBuilder, Effect
#include "core/context.hpp"  // Context, ContextBuilder, FrameContext, CallbackId

// ── Resources ────────────────────────────────────────────────────────────────
#include "resources/format.hpp"
#include "resources/surface.hpp"
#include "resources/texture.hpp"
#include "resources/buffer.hpp"

// ── Pipeline ─────────────────────────────────────────────────────────────────
#include "pipeline/topology.hpp"
#include "pipeline/viewport.hpp"
#include "pipeline/state.hpp"

// ── Events ───────────────────────────────────────────────────────────────────
#include "events/surface.hpp"
#include "events/draw.hpp"
#include "events/state.hpp"
#include "events/resource.hpp"
#include "events/query.hpp"

// ── Capture ──────────────────────────────────────────────────────────────────
#include "capture/gbuffer.hpp"
#include "capture/capture.hpp"

// ── D3D12 types ──────────────────────────────────────────────────────────────
// Always included: events carry them, zero-valued on D3D9 and D3D11.
#include "d3d12/heap.hpp"
#include "d3d12/resource.hpp"
#include "d3d12/command.hpp"
#include "d3d12/pso.hpp"

// ── Effects ──────────────────────────────────────────────────────────────────
//  vangfx::replace_ps(ctx, hlsl, filter)   — swap a draw's pixel shader
//  vangfx::replace_vs(ctx, hlsl, filter)   — swap a draw's vertex shader
//  vangfx::inject_ps_post(ctx, snippet)    — post-processing pass at Present
//  vangfx::Uniforms u(ctx); u.set(0, v)    — values for post passes
#include "effect/replace.hpp"
#include "effect/inject.hpp"
#include "effect/uniform.hpp"

// ── Audio ────────────────────────────────────────────────────────────────────
#if VANGFX_AUDIO
#include "audio.hpp"
#endif
