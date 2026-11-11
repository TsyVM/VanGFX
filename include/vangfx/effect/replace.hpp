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
 *   // Paint every draw of 3000+ vertices solid green.
 *   vangfx::ReplaceFilter big;
 *   big.min_verts = 3000;
 *   auto h = vangfx::replace_ps(ctx, R"(
 *       float4 main() : SV_Target { return float4(0, 1, 0, 1); }
 *   )", big);
 *   if (!h) log(h.error().message);   // the compiler's diagnostic
 *
 * ─────────────────────────────────────────────────────────────────────────
 *  SCOPED REPLACEMENT (auto-restores when the guard goes out of scope)
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   {
 *       auto guard = vangfx::ScopedShaderReplace::ps(ctx, hlsl_source);
 *   } // removed here
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

    // Only draws running these shaders (DrawEvent::vs_hash / ps_hash; 0 = any).
    // The draw census (vangfx/census.hpp) lists the hashes a frame uses.
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;

    // Custom predicate — return true to apply replacement, false to skip.
    std::function<bool(const FrameContext&, const DrawEvent&)> predicate;

    [[nodiscard]] bool matches(const FrameContext& frame, const DrawEvent& ev) const {
        uint32_t vc = (ev.vertex_count > 0) ? ev.vertex_count : ev.index_count;
        if (vc < min_verts || vc > max_verts) return false;
        if (vs_hash && ev.vs_hash != vs_hash) return false;
        if (ps_hash && ev.ps_hash != ps_hash) return false;
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
//  replace_ps — replace the pixel shader on matching draw calls
//
//  Compiles hlsl_source and swaps it in for every draw the filter matches, for
//  that draw only: the game's shader is back for the next one. Works on all
//  three backends. The replacement sees the game's bindings (textures,
//  samplers, constant buffers in the game's registers) -- it replaces the
//  game's shader, so it has to read what the game's shader read.
//
//  D3D12: pipelines are rebuilt with the replacement, which needs their
//  description; that is recorded for pipelines created after the first
//  replacement, so install replacements before the game builds its pipelines
//  (attach at device creation with vangfx::AutoAttach). The default profile is
//  SM 6.0 when dxcompiler.dll can be loaded, SM 5.1 when it cannot; either way
//  the replacement is recompiled to match a game whose pipelines use the other
//  form, since D3D12 refuses a pipeline mixing DXBC (SM 5) and DXIL (SM 6).
//
//  Returns the compiler's diagnostic on failure.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] Result<EffectHandle> replace_ps(
    Context&          ctx,
    std::string_view  hlsl_source,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    options = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  replace_vs — replace the vertex shader on matching draw calls
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] Result<EffectHandle> replace_vs(
    Context&          ctx,
    std::string_view  hlsl_source,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    options = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  replace_effect — replace both vertex and pixel shaders together
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] Result<EffectHandle> replace_effect(
    Context&          ctx,
    std::string_view  vs_hlsl,
    std::string_view  ps_hlsl,
    ReplaceFilter     filter  = ReplaceFilter{},
    ReplaceOptions    options = ReplaceOptions{});

// ─────────────────────────────────────────────────────────────────────────────
//  remove_replacement — unregister a replacement installed by replace_*
//
//  Once this returns no draw is using it. Replacements also go away with their
//  context.
// ─────────────────────────────────────────────────────────────────────────────

void remove_replacement(Context& ctx, EffectHandle handle);

// ─────────────────────────────────────────────────────────────────────────────
//  ScopedShaderReplace — RAII; removes the replacement on destruction
//
//      auto r = vangfx::ScopedShaderReplace::ps(ctx, hlsl);
//      if (!r) log(r.error().message);
// ─────────────────────────────────────────────────────────────────────────────

class ScopedShaderReplace {
public:
    ScopedShaderReplace() = default;
    ScopedShaderReplace(Context& ctx, EffectHandle h) noexcept : ctx_(&ctx), handle_(h) {}

    ScopedShaderReplace(ScopedShaderReplace&& o) noexcept
        : ctx_(o.ctx_), handle_(o.handle_) { o.ctx_ = nullptr; o.handle_ = {}; }

    ScopedShaderReplace& operator=(ScopedShaderReplace&& o) noexcept {
        if (this != &o) {
            reset();
            ctx_ = o.ctx_; handle_ = o.handle_;
            o.ctx_ = nullptr; o.handle_ = {};
        }
        return *this;
    }

    ~ScopedShaderReplace() { reset(); }

    [[nodiscard]] static Result<ScopedShaderReplace> ps(
        Context& ctx, std::string_view hlsl,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        auto h = replace_ps(ctx, hlsl, std::move(f), std::move(o));
        if (!h) return std::unexpected(h.error());
        return ScopedShaderReplace(ctx, *h);
    }

    [[nodiscard]] static Result<ScopedShaderReplace> vs(
        Context& ctx, std::string_view hlsl,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        auto h = replace_vs(ctx, hlsl, std::move(f), std::move(o));
        if (!h) return std::unexpected(h.error());
        return ScopedShaderReplace(ctx, *h);
    }

    [[nodiscard]] static Result<ScopedShaderReplace> effect(
        Context& ctx, std::string_view vs_hlsl, std::string_view ps_hlsl,
        ReplaceFilter f = ReplaceFilter{}, ReplaceOptions o = ReplaceOptions{})
    {
        auto h = replace_effect(ctx, vs_hlsl, ps_hlsl, std::move(f), std::move(o));
        if (!h) return std::unexpected(h.error());
        return ScopedShaderReplace(ctx, *h);
    }

    [[nodiscard]] bool         valid()  const noexcept { return ctx_ && is_valid(handle_); }
    [[nodiscard]] EffectHandle handle() const noexcept { return handle_; }

    void reset() noexcept {
        if (ctx_ && is_valid(handle_)) remove_replacement(*ctx_, handle_);
        ctx_    = nullptr;
        handle_ = {};
    }

private:
    Context*     ctx_    = nullptr;
    EffectHandle handle_ = {};
};

} // namespace vangfx
