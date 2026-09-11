#pragma once

/**
 * vangfx/effect/replace.hpp — One-call pixel / vertex shader replacement.
 *
 * Wraps the full Context + ShaderBuilder + Effect pipeline into a single
 * function call.  No knowledge of descriptor heaps, root signatures, PSOs,
 * or command lists required.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  PIXEL SHADER REPLACEMENT
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   // Greyscale — one line.
 *   vangfx::replace_ps(ctx, R"(
 *       float4 main(float2 uv : TEXCOORD) : SV_Target {
 *           float4 c = tex.Sample(samp, uv);
 *           float  g = dot(c.rgb, float3(0.299, 0.587, 0.114));
 *           return float4(g, g, g, c.a);
 *       }
 *   )");
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  SCOPED REPLACEMENT (auto-restores when the guard goes out of scope)
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   {
 *       auto guard = vangfx::ScopedShaderReplace::ps(ctx, hlsl_source);
 *   } // restored here
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  FILTER REPLACEMENTS
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   // Only replace on draws with >= 3000 vertices
 *   vangfx::ReplaceFilter f;
 *   f.min_verts = 3000;
 *   vangfx::replace_ps(ctx, hlsl, f);
 *
 *   // Only replace when rendering to a 1080p render target
 *   vangfx::ReplaceFilter f;
 *   f.rt_width = 1920; f.rt_height = 1080;
 *   vangfx::replace_ps(ctx, hlsl, f);
 */

#include <vangfx/core/context.hpp>
#include <vangfx/events/draw.hpp>
#include <vangfx/events/surface.hpp>
#include <functional>
#include <string>
#include <string_view>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  ReplaceFilter — optional predicate to restrict which draw calls are affected
//
//  All conditions are ANDed.  Default (no conditions set) matches every draw.
// ─────────────────────────────────────────────────────────────────────────────

struct ReplaceFilter {
    // Vertex count thresholds (0 = no limit)
    uint32_t min_verts = 0;
    uint32_t max_verts = UINT32_MAX;

    // Render-target size (0 = match any)
    uint32_t rt_width  = 0;
    uint32_t rt_height = 0;

    // Custom predicate — return true to apply replacement, false to skip.
    std::function<bool(const FrameContext&, const DrawEvent&)> predicate;

    [[nodiscard]] bool matches(const FrameContext& frame, const DrawEvent& ev) const {
        uint32_t vc = (ev.vertex_count > 0) ? ev.vertex_count : ev.index_count;
        if (vc < min_verts || vc > max_verts) return false;
        if (rt_width || rt_height) {
            auto surf = frame.surfaces();
            if (rt_width  && surf.width  != rt_width)  return false;
            if (rt_height && surf.height != rt_height) return false;
        }
        if (predicate && !predicate(frame, ev)) return false;
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ReplaceOptions — compiler options for the replacement shader
// ─────────────────────────────────────────────────────────────────────────────

struct ReplaceOptions {
    std::string entry   = "main";  // HLSL entry point
    std::string profile = "";      // "" = auto-select from backend + stage
    bool        use_dxc = false;   // force DXC (D3D12 SM 6.x)
    bool        debug   = false;   // embed debug info in blob
};

// ─────────────────────────────────────────────────────────────────────────────
//  replace_ps — replace the pixel/fragment shader on matching draw calls
//
//  Compiles hlsl_source, installs an on_draw hook that swaps the pixel shader
//  before the draw and restores it after.  Thread-safe; can be called from
//  any thread before the first Present.
//
//  Returns an EffectHandle that identifies this replacement.
//  Returns an invalid handle (id == 0) on compile failure (error is logged).
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] EffectHandle replace_ps(
    Context&          ctx,
    std::string_view  hlsl_source,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    options = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  replace_vs — replace the vertex shader on matching draw calls
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] EffectHandle replace_vs(
    Context&          ctx,
    std::string_view  hlsl_source,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    options = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  replace_effect — replace both vertex and pixel shaders atomically
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] EffectHandle replace_effect(
    Context&          ctx,
    std::string_view  vs_hlsl,
    std::string_view  ps_hlsl,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    options = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  remove_replacement — unregister a replacement installed by replace_*
// ─────────────────────────────────────────────────────────────────────────────

void remove_replacement(Context& ctx, EffectHandle handle);

// ─────────────────────────────────────────────────────────────────────────────
//  ScopedShaderReplace — RAII; removes the replacement on destruction
// ─────────────────────────────────────────────────────────────────────────────

class ScopedShaderReplace {
public:
    ScopedShaderReplace() = default;

    ScopedShaderReplace(ScopedShaderReplace&& o) noexcept
        : ctx_(o.ctx_), handle_(o.handle_) { o.ctx_ = nullptr; o.handle_ = {}; }

    ScopedShaderReplace& operator=(ScopedShaderReplace&& o) noexcept {
        reset();
        ctx_ = o.ctx_; handle_ = o.handle_;
        o.ctx_ = nullptr; o.handle_ = {};
        return *this;
    }

    ~ScopedShaderReplace() { reset(); }

    [[nodiscard]] static ScopedShaderReplace ps(
        Context& ctx, std::string_view hlsl,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        ScopedShaderReplace s;
        s.ctx_    = &ctx;
        s.handle_ = replace_ps(ctx, hlsl, std::move(f), std::move(o));
        return s;
    }

    [[nodiscard]] static ScopedShaderReplace vs(
        Context& ctx, std::string_view hlsl,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        ScopedShaderReplace s;
        s.ctx_    = &ctx;
        s.handle_ = replace_vs(ctx, hlsl, std::move(f), std::move(o));
        return s;
    }

    [[nodiscard]] static ScopedShaderReplace effect(
        Context& ctx,
        std::string_view vs_hlsl, std::string_view ps_hlsl,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        ScopedShaderReplace s;
        s.ctx_    = &ctx;
        s.handle_ = replace_effect(ctx, vs_hlsl, ps_hlsl, std::move(f), std::move(o));
        return s;
    }

    [[nodiscard]] bool valid()   const noexcept { return ctx_ && is_valid(handle_); }
    void reset() noexcept {
        if (ctx_ && is_valid(handle_)) {
            remove_replacement(*ctx_, handle_);
            ctx_    = nullptr;
            handle_ = {};
        }
    }

private:
    Context*     ctx_    = nullptr;
    EffectHandle handle_ = {};
};

} // namespace vangfx
