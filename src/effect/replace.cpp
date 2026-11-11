/**
 * src/effect/replace.cpp — replace_ps, replace_vs, replace_effect.
 *
 * Each replacement is compiled into an Effect and kept in the context's
 * registry with its filter. One on_draw callback per context walks the registry
 * on every draw and, for each replacement whose filter matches, calls
 * FrameContext::bind_effect. Every backend swaps the shaders for that one draw
 * and puts the game's back after it, so a filter selecting one object changes
 * that object and nothing drawn after it.
 *
 * The registry belongs to the context: it is created on the first replacement
 * and taken down, with its callback, when the context is destroyed. (It used to
 * be keyed by the Context's address and never cleared, so a new context
 * allocated at the same address found the old one's registry and never got a
 * callback of its own.)
 */

#include <vangfx/effect/replace.hpp>
#include <vangfx/core/shader.hpp>
#include "../core/context_impl.hpp"
#include "../core/shader_compile.hpp"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vangfx {

namespace {

struct Record {
    Effect        effect;
    ReplaceFilter filter;
};

struct Registry {
    std::shared_mutex   mtx;
    std::vector<Record> records;
    CallbackId          draw_cb{};

    void apply(FrameContext& frame, DrawEvent& ev) {
        std::shared_lock lk(mtx);
        for (auto& rec : records) {
            if (!rec.filter.matches(frame, ev)) continue;
            // Stages the Effect does not carry stay as the game set them. A
            // failure (D3D12 pipeline not recorded, a refused shader) leaves the
            // draw as it was; the backend logs why, once.
            (void)frame.bind_effect(rec.effect);
        }
    }
};

std::mutex                                           g_mtx;
std::unordered_map<Context*, std::shared_ptr<Registry>> g_registries;

std::shared_ptr<Registry> find_registry(Context& ctx) {
    std::lock_guard lk(g_mtx);
    auto it = g_registries.find(&ctx);
    return it != g_registries.end() ? it->second : nullptr;
}

std::shared_ptr<Registry> registry_for(Context& ctx) {
    std::lock_guard lk(g_mtx);
    if (auto it = g_registries.find(&ctx); it != g_registries.end()) return it->second;

    auto reg = std::make_shared<Registry>();
    reg->draw_cb = ctx.on_draw([reg](FrameContext& frame, DrawEvent& ev) { reg->apply(frame, ev); });
    g_registries.emplace(&ctx, reg);

    static_cast<ContextImpl&>(ctx).add_teardown([&ctx] {
        std::shared_ptr<Registry> gone;
        {
            std::lock_guard lk(g_mtx);
            auto it = g_registries.find(&ctx);
            if (it == g_registries.end()) return;
            gone = std::move(it->second);
            g_registries.erase(it);
        }
        ctx.remove_callback(gone->draw_cb);
        std::unique_lock lk(gone->mtx);
        for (auto& rec : gone->records) ctx.destroy(rec.effect.handle);
        gone->records.clear();
    });
    return reg;
}

// D3D12 compiles SM 6.0 when dxcompiler.dll can be loaded and SM 5.1 when it
// cannot (d3dcompiler ships with Windows; DXC does not). Either way the backend
// recompiles the replacement to match a game whose pipelines use the other
// form, since a pipeline cannot mix DXBC and DXIL.
std::string auto_profile(Context& ctx, ShaderStage stage) {
    const bool ps = stage == ShaderStage::Pixel;
    switch (ctx.backend()) {
    case BackendKind::D3D9:  return ps ? "ps_3_0" : "vs_3_0";
    case BackendKind::D3D12: return detail::d3d12_default_profile(ps ? "ps" : "vs");
    default:                 return ps ? "ps_5_0" : "vs_5_0";
    }
}

Result<Shader> compile(Context& ctx, std::string_view src, ShaderStage stage, const ReplaceOptions& opts) {
    const std::string profile = opts.profile.empty() ? auto_profile(ctx, stage) : opts.profile;
    return ShaderBuilder{}
        .source(std::string(src))
        .entry(opts.entry.empty() ? "main" : opts.entry)
        .profile(profile)
        .stage(stage)
        .use_dxc(opts.use_dxc)
        .debug_info(opts.debug)
        .build(ctx);
}

Result<EffectHandle> install(Context& ctx, std::string_view vs_src, std::string_view ps_src,
                             ReplaceFilter filter, const ReplaceOptions& opts) {
    if (vs_src.empty() && ps_src.empty())
        return gfx_err<EffectHandle>(ShaderError::InvalidFormat, "replace: no shader source given.");

    Shader vs, ps;
    if (!vs_src.empty()) {
        auto r = compile(ctx, vs_src, ShaderStage::Vertex, opts);
        if (!r) return gfx_err<EffectHandle>(r.error().code, "replace: vertex shader: " + r.error().message);
        vs = std::move(*r);
    }
    if (!ps_src.empty()) {
        auto r = compile(ctx, ps_src, ShaderStage::Pixel, opts);
        if (!r) return gfx_err<EffectHandle>(r.error().code, "replace: pixel shader: " + r.error().message);
        ps = std::move(*r);
    }
    auto effect = ctx.link_effect(vs, ps);
    if (!effect) return gfx_err<EffectHandle>(effect.error().code, "replace: " + effect.error().message);

    const EffectHandle handle = effect->handle;
    auto reg = registry_for(ctx);
    std::unique_lock lk(reg->mtx);
    reg->records.push_back({ std::move(*effect), std::move(filter) });
    return handle;
}

} // namespace

Result<EffectHandle> replace_ps(Context& ctx, std::string_view hlsl, ReplaceFilter filter, ReplaceOptions options) {
    return install(ctx, {}, hlsl, std::move(filter), options);
}

Result<EffectHandle> replace_vs(Context& ctx, std::string_view hlsl, ReplaceFilter filter, ReplaceOptions options) {
    return install(ctx, hlsl, {}, std::move(filter), options);
}

Result<EffectHandle> replace_effect(Context& ctx, std::string_view vs_hlsl, std::string_view ps_hlsl,
                                    ReplaceFilter filter, ReplaceOptions options) {
    return install(ctx, vs_hlsl, ps_hlsl, std::move(filter), options);
}

void remove_replacement(Context& ctx, EffectHandle handle) {
    if (!is_valid(handle)) return;
    auto reg = find_registry(ctx);
    if (!reg) return;
    Effect gone;
    {
        // Exclusive: waits for any draw still applying the record.
        std::unique_lock lk(reg->mtx);
        for (size_t i = 0; i < reg->records.size(); ++i) {
            if (reg->records[i].effect.handle.id == handle.id) {
                gone = std::move(reg->records[i].effect);
                reg->records.erase(reg->records.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
    }
    if (gone.valid()) ctx.destroy(gone.handle);
}

} // namespace vangfx
