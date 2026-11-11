#pragma once

/**
 * vangfx/effect/inject.hpp — post-processing passes over the finished frame.
 *
 * Each pass is a small HLSL snippet run over every pixel of the back buffer at
 * Present, after the game has drawn the frame and before it is shown. Passes
 * run in the order they were added, each reading what the one before wrote.
 * The game's own shaders and pipeline state are untouched.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  EXAMPLES
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   // Clamp brightness
 *   auto h = vangfx::inject_ps_post(ctx, "color.rgb = min(color.rgb, 1.0);");
 *   if (!h) log(h.error().message);   // the compiler's diagnostic
 *
 *   // Red tint, strength from a uniform
 *   vangfx::Uniforms u(ctx);
 *   u.set(0, 0.3f);
 *   vangfx::inject_ps_post(ctx,
 *       "color.rgb = lerp(color.rgb, float3(1, 0.2, 0.2), vangfx_float4[0].x);");
 *
 *   // Wobble: move where the frame is sampled
 *   vangfx::inject_ps_pre(ctx,
 *       "vangfx_uv.x += sin(vangfx_uv.y * 20 + vangfx_time * 3) * 0.005;");
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  AVAILABLE IN A SNIPPET
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   float4 color              (post) the frame's pixel; modify in place
 *   float2 vangfx_uv          [0,1] position on screen; (pre) modify to move the sample
 *   float4 vangfx_sample(uv)  the frame at any position (blurs, edges, ...)
 *   float  vangfx_time        seconds since the context was created
 *   float  vangfx_delta       seconds since the last frame
 *   float2 vangfx_resolution  back buffer size in pixels
 *   float4 vangfx_float4[8]   set from C++ with vangfx::Uniforms
 *
 * The snippet is compiled for the backend: ps_3_0 on D3D9, ps_5_0 on D3D11,
 * and on D3D12 ps_6_0 (ps_5_1 when dxcompiler.dll cannot be loaded). Stick to
 * what all of them accept to write it once.
 */

#include <vangfx/core/context.hpp>
#include <string_view>

namespace vangfx {

// Run `hlsl_snippet` over the frame, with `color` holding each pixel.
[[nodiscard]] Result<EffectHandle> inject_ps_post(Context& ctx, std::string_view hlsl_snippet);

// Run `hlsl_snippet` before the frame is sampled: modify `vangfx_uv`.
[[nodiscard]] Result<EffectHandle> inject_ps_pre(Context& ctx, std::string_view hlsl_snippet);

// Remove a pass. Passes also go away with their context.
void remove_injection(Context& ctx, EffectHandle handle);

// ─────────────────────────────────────────────────────────────────────────────
//  ScopedInjection — RAII; removes the pass on destruction
// ─────────────────────────────────────────────────────────────────────────────

class ScopedInjection {
public:
    ScopedInjection() = default;
    ScopedInjection(Context& ctx, EffectHandle h) noexcept : ctx_(&ctx), handle_(h) {}

    ScopedInjection(ScopedInjection&& o) noexcept
        : ctx_(o.ctx_), handle_(o.handle_) { o.ctx_ = nullptr; o.handle_ = {}; }

    ScopedInjection& operator=(ScopedInjection&& o) noexcept {
        if (this != &o) {
            reset();
            ctx_ = o.ctx_; handle_ = o.handle_;
            o.ctx_ = nullptr; o.handle_ = {};
        }
        return *this;
    }

    ~ScopedInjection() { reset(); }

    [[nodiscard]] static Result<ScopedInjection> post(Context& ctx, std::string_view snippet) {
        auto h = inject_ps_post(ctx, snippet);
        if (!h) return std::unexpected(h.error());
        return ScopedInjection(ctx, *h);
    }

    [[nodiscard]] static Result<ScopedInjection> pre(Context& ctx, std::string_view snippet) {
        auto h = inject_ps_pre(ctx, snippet);
        if (!h) return std::unexpected(h.error());
        return ScopedInjection(ctx, *h);
    }

    [[nodiscard]] bool         valid()  const noexcept { return ctx_ && is_valid(handle_); }
    [[nodiscard]] EffectHandle handle() const noexcept { return handle_; }

    void reset() noexcept {
        if (ctx_ && is_valid(handle_)) remove_injection(*ctx_, handle_);
        ctx_    = nullptr;
        handle_ = {};
    }

private:
    Context*     ctx_    = nullptr;
    EffectHandle handle_ = {};
};

} // namespace vangfx
