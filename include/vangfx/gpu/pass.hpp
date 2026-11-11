#pragma once

/**
 * vangfx/gpu/pass.hpp — gpu::Pass: one mod rendering pass inside a game frame.
 *
 * A Pass is created by Device::begin_pass().  It saves the game's pipeline
 * state before its first draw and restores it on end() (or destruction).  On
 * D3D12 / Vulkan / Metal the pass records into its own command list that is
 * submitted to the game's queue before Present.
 *
 * Typical use:
 *
 *     auto pass = device.begin_pass();
 *     pass->set_viewport(0, 0, width, height);
 *     pass->set_pipeline(my_pipeline);
 *     pass->bind(0, backbuffer_srv);    // t0
 *     pass->update_params(params);      // upload to b0
 *     pass->draw_fullscreen();
 *     pass->end();
 */

#include <vangfx/gpu/resources.hpp>
#include <cstdint>
#include <cstddef>

namespace vangfx::gpu {

// VanGFXParams — the constant buffer layout declared in vangfx.hlsli.
// Always uploaded to b0 before every pass that calls update_params().
#pragma pack(push, 4)
struct VanGFXParams {
    float    float4[8][4];       // vangfx_float4[8]
    float    time;               // vangfx_time (seconds)
    float    delta;              // vangfx_delta (frame dt)
    float    resolution[2];      // vangfx_resolution (pixels)
    float    _pad[28];           // pad to 256 bytes for D3D12 CBV alignment
};
#pragma pack(pop)
static_assert(sizeof(VanGFXParams) == 256, "VanGFXParams must be 256 bytes");

class Pass {
public:
    virtual ~Pass() = default;

    // Explicit output render target.  Defaults to the game's current RT.
    virtual void set_render_target(Texture& rt) = 0;

    // Set the viewport for subsequent draws.
    virtual void set_viewport(uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h) = 0;

    // Bind the compiled pipeline (VS + PS, or CS).
    virtual void set_pipeline(Pipeline& p) = 0;

    // Bind a texture to a t-register slot (0-7).
    virtual void bind(uint32_t slot, Texture& t) = 0;

    // Upload VanGFXParams and bind to b0.
    virtual void update_params(const VanGFXParams& params) = 0;

    // Draw a fullscreen triangle (3 vertices, no VB on D3D11/D3D12;
    // a prebuilt quad VB on D3D9).
    virtual void draw_fullscreen() = 0;

    // Generic draw: `vertices` vertices, `instances` instances.
    virtual void draw(uint32_t vertices, uint32_t instances = 1) = 0;

    // Compute dispatch.
    virtual void dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;

    // Finish the pass.  State is restored (D3D9/D3D11) or the command list
    // is closed and queued for submission (D3D12).
    // Calling end() more than once is a no-op.
    virtual void end() = 0;
};

} // namespace vangfx::gpu
