#pragma once

/**
 * vangfx/effect/uniform.hpp — values from C++ for post-processing passes.
 *
 * Every pass added with inject_ps_post / inject_ps_pre sees:
 *
 *   float4 vangfx_float4[8];   // the eight slots set here
 *   float  vangfx_time;        // seconds since the context was created
 *   float  vangfx_delta;       // seconds since the last frame
 *   float2 vangfx_resolution;  // back buffer size in pixels
 *
 *   vangfx::Uniforms u(ctx);
 *   u.set(0, 0.75f);                 // vangfx_float4[0].x = 0.75
 *   u.set(1, 1.0f, 0.5f, 0.2f);      // vangfx_float4[1].xyz
 *
 * The slots belong to the context, so every Uniforms on one context writes the
 * same eight, and a value set once stays until changed. A set() takes effect
 * at the next Present. Replacement shaders (replace_ps) run with the game's
 * bindings and do not see these.
 */

#include <vangfx/core/context.hpp>
#include <cstdint>

namespace vangfx {

static constexpr uint32_t kUniformSlots = 8;

struct Float4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;

    Float4() = default;
    explicit Float4(float v)                   : x(v), y(v), z(v), w(v) {}
    Float4(float x_, float y_)                 : x(x_), y(y_) {}
    Float4(float x_, float y_, float z_)       : x(x_), y(y_), z(z_) {}
    Float4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

class Uniforms {
public:
    explicit Uniforms(Context& ctx) noexcept : ctx_(&ctx) {}

    // Set a slot (0 - kUniformSlots-1). Components not given keep their value.
    void set(uint32_t slot, float x)                            noexcept;
    void set(uint32_t slot, float x, float y)                   noexcept;
    void set(uint32_t slot, float x, float y, float z)          noexcept;
    void set(uint32_t slot, float x, float y, float z, float w) noexcept;
    void set(uint32_t slot, Float4 v)                           noexcept;

    [[nodiscard]] Float4 get(uint32_t slot) const noexcept;

    // Every slot back to zero.
    void reset() noexcept;

private:
    Context* ctx_;
};

} // namespace vangfx
