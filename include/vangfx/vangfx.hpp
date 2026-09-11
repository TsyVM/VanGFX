#pragma once

/**
 * vangfx/vangfx.hpp — VanGFX SDK single-include entry point.
 *
 * Universal D3D9 / D3D11 / D3D12 shader authoring, injection, and pipeline
 * interception layer for DirectX game mods.
 *
 * Usage:
 *   #include <vangfx/vangfx.hpp>
 *
 * MSVC project setup (D3D9/D3D11):
 *   Additional Include Dirs  → vangfx/include/
 *   Additional Library Dirs  → vangfx/libs/win-x64/Release/  (or win-x86)
 *   Additional Dependencies  → vangfx.lib
 *   Additional Dependencies  → d3d9.lib d3d11.lib dxgi.lib d3dcompiler.lib
 *   Runtime Library          → /MT
 *
 * MSVC project setup (D3D12 / additional deps):
 *   Additional Dependencies  → d3d12.lib dxgi.lib dxguid.lib
 *   Additional Dependencies  → dxcompiler.lib   (DXC for SM 6.x DXIL)
 *   Minimum Windows SDK      → 10.0.20348.0 (for mesh shader / DXR headers)
 *
 * Built and maintained by TeamVanilla — https://www.teamvanilla.org/
 */

// ── Core ─────────────────────────────────────────────────────────────────────
#include "core/error.hpp"
#include "core/shader.hpp"   // Shader, ShaderBuilder, Effect
#include "core/context.hpp"  // Context, ContextBuilder, FrameContext, BackendKind

// ── Resources ─────────────────────────────────────────────────────────────────
#include "resources/format.hpp"
#include "resources/surface.hpp"
#include "resources/texture.hpp"
#include "resources/buffer.hpp"

// ── Pipeline ──────────────────────────────────────────────────────────────────
#include "pipeline/topology.hpp"
#include "pipeline/viewport.hpp"
#include "pipeline/state.hpp"

// ── Events ────────────────────────────────────────────────────────────────────
#include "events/surface.hpp"
#include "events/draw.hpp"
#include "events/state.hpp"
#include "events/resource.hpp"
#include "events/query.hpp"

// ── Capture ───────────────────────────────────────────────────────────────────
#include "capture/gbuffer.hpp"
#include "capture/capture.hpp"

// ── D3D12 extensions ──────────────────────────────────────────────────────────
// Included unconditionally; types are in the vangfx::d3d12 namespace.
// On D3D9/D3D11 backends these types appear in events but are zero-valued.
#include "d3d12/heap.hpp"
#include "d3d12/resource.hpp"
#include "d3d12/command.hpp"
#include "d3d12/pso.hpp"

// ── High-level Effect API ─────────────────────────────────────────────────────
// One-call shader replacement, snippet injection, and live parameter binding.
// These headers are what most mod authors will use day-to-day.
//
//  vangfx::replace_ps(ctx, hlsl)          — replace pixel shader
//  vangfx::replace_vs(ctx, hlsl)          — replace vertex shader
//  vangfx::inject_ps_post(ctx, snippet)   — inject code after pixel output
//  vangfx::fx::greyscale(ctx)             — built-in greyscale preset
//  vangfx::fx::tint(ctx, {r,g,b})         — built-in colour tint preset
//  vangfx::Uniforms u(ctx); u.set(0, v)   — live float parameters
//
#include "effect/replace.hpp"    // replace_ps, replace_vs, replace_effect,
                                 //   ScopedShaderReplace, ReplaceFilter
#include "effect/inject.hpp"     // inject_ps_post, inject_ps_pre,
                                 //   ScopedInjection
#include "effect/uniform.hpp"    // Uniforms, Float4 — live cbuffer params

// ── Audio / SFX ───────────────────────────────────────────────────────────────
// XAudio2 / DirectSound interception, framework sound playback, DSP injection,
// and audio capture.  Include vangfx/audio.hpp directly for audio-only use.
//
//  vangfx::AudioContextBuilder{}.build()   — create the audio context
//  audio->on_buffer_submit(cb)             — intercept audio buffers
//  audio->load_wav("shot.wav")             — load a sound into the library
//  audio->play(snd)                        — play it through the framework device
//  audio->inject_master_effect(dsp_cb)     — insert a DSP stage on the master
//
// Linker: add xaudio2.lib and dsound.lib alongside vangfx.lib.
#include "audio.hpp"

// NOTE: compat/ headers (vtable.hpp, com.hpp, *_slots.hpp) are NOT included
// here. They are private implementation details compiled into vangfx.lib and
// are not part of the public SDK.
