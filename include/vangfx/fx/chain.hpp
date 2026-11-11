#pragma once

/**
 * vangfx/fx/chain.hpp — fx::Chain: the effect runtime.
 *
 * Chain loads every effect folder under a given directory, compiles each
 * pass's shaders, and runs them in order at present time.  It is API-blind:
 * it only talks to gpu::Device and gpu::Pass.
 *
 * Usage:
 *
 *   // Once, during device creation:
 *   auto chain = fx::Chain::create(paths, cfg);
 *
 *   // At Present (called by the front-end backend):
 *   chain->run_present(device,
 *                      backbuffer_wrapped,   // gpu::Texture over the swap-chain BB
 *                      depth_wrapped,        // may be null
 *                      elapsed_seconds,
 *                      frame_delta);
 *
 *   // Live reload (e.g. Home key):
 *   chain->reload();
 */

#include <vangfx/gpu/device.hpp>
#include <vangfx/gpu/resources.hpp>
#include <vangfx/fx/effect_desc.hpp>
#include <functional>
#include <memory>
#include <filesystem>

namespace vangfx {
class Paths;
class Config;
}

namespace vangfx::fx {

class Chain {
public:
    // Create a chain that loads effects from paths.effects().
    [[nodiscard]] static std::unique_ptr<Chain> create(
        const vangfx::Paths&  paths,
        const vangfx::Config& cfg);

    virtual ~Chain() = default;

    // Reload all effect folders from disk (recompile shaders).
    // Safe to call from any thread; takes effect on the next run_present().
    virtual void reload() = 0;

    // Run all loaded effects in order.
    //   device       — the gpu Device for this frame
    //   backbuffer   — the swap-chain back buffer wrapped as a Texture
    //   depth        — the best depth buffer from DepthService (may be null)
    //   time         — seconds elapsed since chain creation
    //   delta        — seconds since the last frame
    virtual void run_present(gpu::Device&  device,
                             gpu::Texture& backbuffer,
                             gpu::Texture* depth,
                             float         time,
                             float         delta) = 0;

    // Set a callback that runs after all effects, before the present swap.
    // Intended for overlay drawing (VanGUI).
    // Callback receives: device, the final intermediate texture (or the
    // backbuffer if no effects ran), width, height.
    using OverlayCb = std::function<void(gpu::Device&,
                                         gpu::Texture& /*final*/,
                                         uint32_t /*w*/,
                                         uint32_t /*h*/)>;
    virtual void set_overlay(OverlayCb cb) = 0;

    // Live list of loaded effects (for overlay display / debug).
    virtual std::vector<EffectDesc*> effects() = 0;
};

} // namespace vangfx::fx
