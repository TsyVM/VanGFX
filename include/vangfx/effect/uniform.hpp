#pragma once

/**
 * vangfx/effect/uniform.hpp — Live parameter binding for replacement shaders.
 *
 * Pass values from C++ into your HLSL replacement or injection snippet
 * without recompiling.  All values are written into a small constant buffer
 * (cbuffer vangfx_params) automatically available in every shader installed
 * via replace_ps / replace_vs / inject_ps_post / inject_ps_pre.
 *
 * ──────────────────────────────────────────────────────────────────────────
 *  HLSL side — cbuffer always available in replacement / injection shaders:
 * ──────────────────────────────────────────────────────────────────────────
 *
 *   cbuffer vangfx_params : register(b0) {
 *       float4 vangfx_float4[8];   // set via Uniforms::set(name, float4)
 *       float  vangfx_time;        // always: seconds since context creation
 *       float  vangfx_delta;       // always: seconds since last frame
 *       float2 vangfx_resolution;  // always: back-buffer width/height in px
 *   };
 *
 *   // Access user values by slot (0–7), not by name:
 *   float  my_strength = vangfx_float4[0].x;
 *   float3 my_colour   = vangfx_float4[1].xyz;
 *
 * ──────────────────────────────────────────────────────────────────────────
 *  C++ side
 * ──────────────────────────────────────────────────────────────────────────
 *
 *   vangfx::Uniforms u(ctx);
 *
 *   // Set by slot index (0–7) — fastest path
 *   u.set(0, 0.75f);               // vangfx_float4[0].x = 0.75
 *   u.set(1, 1.0f, 0.5f, 0.2f);   // vangfx_float4[1].xyz = RGB colour
 *
 *   // Update each frame
 *   u.set(0, sinf(time) * 0.5f + 0.5f);
 */

#include <vangfx/core/context.hpp>
#include <array>
#include <cstdint>
#include <cstring>

namespace vangfx {

// Maximum number of user-settable float4 slots
static constexpr uint32_t kUniformSlots = 8;

// ─────────────────────────────────────────────────────────────────────────────
//  Float4 — simple 4-float value type used for uniform slots
// ─────────────────────────────────────────────────────────────────────────────

struct Float4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;

    Float4() = default;
    explicit Float4(float v)                          : x(v),y(v),z(v),w(v) {}
    Float4(float x, float y)                          : x(x),y(y),z(0),w(0) {}
    Float4(float x, float y, float z)                 : x(x),y(y),z(z),w(0) {}
    Float4(float x, float y, float z, float w)        : x(x),y(y),z(z),w(w) {}
};

// ─────────────────────────────────────────────────────────────────────────────
//  Uniforms — manages the vangfx_params constant buffer
//
//  Create one instance per context (or share across effects).
//  Values written via set() are uploaded to the GPU each frame automatically
//  before any replacement shader executes.
// ─────────────────────────────────────────────────────────────────────────────

class Uniforms {
public:
    explicit Uniforms(Context& ctx);

    // Set by slot index (0 – kUniformSlots-1)
    void set(uint32_t slot, float x)                           noexcept;
    void set(uint32_t slot, float x, float y)                  noexcept;
    void set(uint32_t slot, float x, float y, float z)         noexcept;
    void set(uint32_t slot, float x, float y, float z, float w)noexcept;
    void set(uint32_t slot, Float4 v)                          noexcept;

    // Read current CPU-side value (does not read back from GPU)
    [[nodiscard]] Float4 get(uint32_t slot) const noexcept;

    // Force an immediate upload (normally happens automatically before each draw)
    void flush() noexcept;

    // Reset all user slots to zero
    void reset() noexcept;

private:
    Context*              ctx_;
    std::array<Float4, kUniformSlots> slots_ = {};
    ConstantBufferHandle  cbuffer_ = {};
    bool                  dirty_   = false;
};

} // namespace vangfx
