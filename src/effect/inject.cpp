/**
 * src/effect/inject.cpp — inject_ps_post / inject_ps_pre: post-processing
 * passes over the finished frame.
 *
 * The snippet is wrapped in a full-screen pixel shader for the backend's own
 * post-pass pipeline and compiled here, so a mistake in it comes back from
 * inject_ps_* as the compiler's message. The backend's present hook then runs
 * every pass, in the order added, over the back buffer: copy the frame to a
 * texture, draw a full-screen triangle with the pass into the back buffer, and
 * put the game's pipeline state back.
 *
 * (The old inject_ps_post was a replace_ps of every draw with "sample t0 at the
 * screen position" -- which replaced the game's shaders, and sampled whatever
 * happened to be in t0.)
 */

#include <vangfx/effect/inject.hpp>
#include "../core/context_impl.hpp"
#include "../core/shader_compile.hpp"

#include <string>
#include <string_view>

namespace vangfx {

namespace {

// Direct3D 10+ style: ps_5_0 on D3D11; on D3D12 ps_6_0, or ps_5_1 without DXC.
constexpr std::string_view kHeaderSM5 = R"(
cbuffer vangfx_params : register(b0) {
    float4 vangfx_float4[8];
    float  vangfx_time;
    float  vangfx_delta;
    float2 vangfx_resolution;
};
Texture2D    vangfx_scene : register(t0);
SamplerState vangfx_samp  : register(s0);
float4 vangfx_sample(float2 uv) { return vangfx_scene.SampleLevel(vangfx_samp, uv, 0); }

float4 main(float4 vangfx_position : SV_Position) : SV_Target {
    float2 vangfx_uv = vangfx_position.xy / vangfx_resolution;
)";

// Direct3D 9, ps_3_0: constants in registers, VPOS for the pixel position
// (integer at the pixel's top-left corner, hence the half-pixel).
constexpr std::string_view kHeaderSM3 = R"(
float4 vangfx_float4[8] : register(c0);
float4 vangfx_frame     : register(c8);
#define vangfx_time       (vangfx_frame.x)
#define vangfx_delta      (vangfx_frame.y)
#define vangfx_resolution (vangfx_frame.zw)
sampler2D vangfx_scene : register(s0);
float4 vangfx_sample(float2 uv) { return tex2Dlod(vangfx_scene, float4(uv, 0, 0)); }

float4 main(float2 vangfx_vpos : VPOS) : COLOR0 {
    float2 vangfx_uv = (vangfx_vpos + 0.5) / vangfx_resolution;
)";

std::string wrap(const std::string& profile, std::string_view snippet, bool post) {
    std::string src(profile.starts_with("ps_3") ? kHeaderSM3 : kHeaderSM5);
    if (post) {
        src += "    float4 color = vangfx_sample(vangfx_uv);\n    {\n#line 1 \"snippet\"\n";
        src += snippet;
        src += "\n    }\n    return color;\n}\n";
    } else {
        src += "    {\n#line 1 \"snippet\"\n";
        src += snippet;
        src += "\n    }\n    return vangfx_sample(vangfx_uv);\n}\n";
    }
    return src;
}

Result<EffectHandle> add(Context& ctx, std::string_view snippet, bool post) {
    auto& impl = static_cast<ContextImpl&>(ctx);
    const std::string profile = impl.post_pass_profile();
    const std::string src = wrap(profile, snippet, post);
    auto bytes = detail::compile_hlsl(ShaderBuilder{}.source(src).entry("main").profile(profile), profile);
    if (!bytes) return gfx_err<EffectHandle>(bytes.error().code, "inject: " + bytes.error().message);
    return EffectHandle{ impl.add_post_pass(std::move(*bytes)) };
}

} // namespace

Result<EffectHandle> inject_ps_post(Context& ctx, std::string_view hlsl_snippet) { return add(ctx, hlsl_snippet, true); }
Result<EffectHandle> inject_ps_pre (Context& ctx, std::string_view hlsl_snippet) { return add(ctx, hlsl_snippet, false); }

void remove_injection(Context& ctx, EffectHandle handle) {
    if (is_valid(handle)) static_cast<ContextImpl&>(ctx).remove_post_pass(handle.id);
}

} // namespace vangfx
