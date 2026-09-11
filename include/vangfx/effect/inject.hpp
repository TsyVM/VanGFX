#pragma once

/**
 * vangfx/effect/inject.hpp — HLSL snippet injection.
 *
 * Injects a small HLSL fragment that runs before or after the original pixel
 * output.  Useful for overlays and post-processing without replacing the
 * game's full shader.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  EXAMPLES
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   // Clamp brightness (HDR protection)
 *   vangfx::inject_ps_post(ctx, "color.rgb = min(color.rgb, 1.0);");
 *
 *   // Red overlay tint, 30% mix
 *   vangfx::inject_ps_post(ctx,
 *       "color.rgb = lerp(color.rgb, float3(1,0.2,0.2), 0.3);");
 *
 *   // UV wobble (runs before the game's pixel shader samples textures)
 *   vangfx::inject_ps_pre(ctx,
 *       "vangfx_uv.x += sin(vangfx_uv.y*20 + vangfx_time*3) * 0.005;");
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  AVAILABLE VARIABLES IN SNIPPET
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   inject_ps_post:
 *     float4 color        — original pixel shader output; modify in-place
 *     float2 vangfx_uv    — screen-space UV [0,1]x[0,1]
 *     float  vangfx_time  — seconds since context creation
 *
 *   inject_ps_pre:
 *     float2 vangfx_uv    — screen-space UV; modify to shift sampling
 *     float  vangfx_time  — seconds since context creation
 */

#include <vangfx/effect/replace.hpp>
#include <string_view>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  inject_ps_post — run an HLSL snippet after the game's pixel shader output
//
//  Modify the variable `color` (float4) in-place; result writes to SV_Target0.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] EffectHandle inject_ps_post(
    Context&          ctx,
    std::string_view  hlsl_snippet,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    opts    = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  inject_ps_pre — run an HLSL snippet before texture sampling
//
//  Modify `vangfx_uv` to shift where the shader samples from.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] EffectHandle inject_ps_pre(
    Context&          ctx,
    std::string_view  hlsl_snippet,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    opts    = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  remove_injection
// ─────────────────────────────────────────────────────────────────────────────

void remove_injection(Context& ctx, EffectHandle handle);

// ─────────────────────────────────────────────────────────────────────────────
//  ScopedInjection — RAII; removes the injection on destruction
// ─────────────────────────────────────────────────────────────────────────────

class ScopedInjection {
public:
    ScopedInjection() = default;

    ScopedInjection(ScopedInjection&& o) noexcept
        : ctx_(o.ctx_), handle_(o.handle_) { o.ctx_ = nullptr; o.handle_ = {}; }

    ScopedInjection& operator=(ScopedInjection&& o) noexcept {
        reset();
        ctx_ = o.ctx_; handle_ = o.handle_;
        o.ctx_ = nullptr; o.handle_ = {};
        return *this;
    }

    ~ScopedInjection() { reset(); }

    [[nodiscard]] static ScopedInjection post(
        Context& ctx, std::string_view snippet,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        ScopedInjection s;
        s.ctx_    = &ctx;
        s.handle_ = inject_ps_post(ctx, snippet, std::move(f), std::move(o));
        return s;
    }

    [[nodiscard]] static ScopedInjection pre(
        Context& ctx, std::string_view snippet,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        ScopedInjection s;
        s.ctx_    = &ctx;
        s.handle_ = inject_ps_pre(ctx, snippet, std::move(f), std::move(o));
        return s;
    }

    [[nodiscard]] bool valid() const noexcept { return ctx_ && is_valid(handle_); }
    void reset() noexcept {
        if (ctx_ && is_valid(handle_)) {
            remove_injection(*ctx_, handle_);
            ctx_    = nullptr;
            handle_ = {};
        }
    }

private:
    Context*     ctx_    = nullptr;
    EffectHandle handle_ = {};
};

} // namespace vangfx
