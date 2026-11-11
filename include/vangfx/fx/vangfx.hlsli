// vangfx.hlsli — include in every VanGFX effect pixel shader.
//
// Register layout (do not change without updating fx::Chain):
//   b0 : vangfx_params   — time, delta, resolution, 8 × float4 user params
//   t0 : vangfx_backbuffer  — the frame colour at the start of the effect pass
//   t1 : vangfx_depth       — the best depth buffer (may be unused)
//   t2 : vangfx_previous    — previous effect pass output (or backbuffer)
//   t3+ : effect-specific textures
//   s0 : vangfx_sampler     — linear, clamp, [0,1] UVs

cbuffer vangfx_params : register(b0)
{
    float4 vangfx_float4[8];  // vangfx::gpu::VanGFXParams::float4
    float  vangfx_time;
    float  vangfx_delta;
    float2 vangfx_resolution;
};

Texture2D    vangfx_backbuffer : register(t0);
Texture2D    vangfx_depth      : register(t1);
Texture2D    vangfx_previous   : register(t2);
SamplerState vangfx_sampler    : register(s0);

// Helpers

float2 vangfx_uv(float4 sv_pos)
{
    return sv_pos.xy / vangfx_resolution;
}

float4 vangfx_sample(float2 uv)
{
    return vangfx_backbuffer.Sample(vangfx_sampler, uv);
}
