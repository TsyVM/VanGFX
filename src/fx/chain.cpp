#include <vangfx/fx/chain.hpp>
#include <vangfx/fx/effect_desc.hpp>
#include <vangfx/gpu/pass.hpp>
#include <vangfx/gpu/pipeline_desc.hpp>
#include <vangfx/core/paths.hpp>
#include <vangfx/core/config.hpp>
#include <vangfx/resources/texture.hpp>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <vector>
#include <filesystem>

namespace vangfx::fx {

// ── Compiled pass ─────────────────────────────────────────────────────────────

struct CompiledPass {
    PassDesc desc;
    gpu::Pipeline pipeline;
    // Per-pass intermediate RT (null if effect writes directly to backbuffer)
    gpu::Texture  rt;
};

struct LoadedEffect {
    EffectDesc desc;
    std::vector<CompiledPass> compiled_passes;
    bool ready = false;
};

// ── ChainImpl ─────────────────────────────────────────────────────────────────

class ChainImpl final : public Chain {
public:
    ChainImpl(const vangfx::Paths& paths, const vangfx::Config& cfg)
        : paths_(paths), cfg_(cfg) {}

    void reload() override {
        std::lock_guard lock(mutex_);
        dirty_ = true;
    }

    void run_present(gpu::Device& device,
                     gpu::Texture& backbuffer,
                     gpu::Texture* depth,
                     float time,
                     float delta) override {
        {
            std::lock_guard lock(mutex_);
            if (dirty_) {
                load_effects(device);
                dirty_ = false;
            }
        }

        uint32_t bb_w = backbuffer.width();
        uint32_t bb_h = backbuffer.height();

        gpu::Texture* previous = nullptr;
        gpu::Texture* current  = &backbuffer;

        for (auto& fx : effects_) {
            if (!fx.ready) continue;
            for (auto& cp : fx.compiled_passes) {
                if (!cp.pipeline.valid()) continue;

                // Build params
                gpu::VanGFXParams params{};
                std::memcpy(params.float4, fx.desc.float4, sizeof(fx.desc.float4));
                params.time        = time;
                params.delta       = delta;
                params.resolution[0] = float(bb_w);
                params.resolution[1] = float(bb_h);

                auto pass = device.begin_pass();

                // Determine render target
                uint32_t rt_w = cp.desc.width  ? cp.desc.width  : bb_w;
                uint32_t rt_h = cp.desc.height ? cp.desc.height : bb_h;

                if (cp.rt.valid()) {
                    pass->set_render_target(cp.rt);
                    pass->set_viewport(0, 0, rt_w, rt_h);
                } else {
                    pass->set_viewport(0, 0, bb_w, bb_h);
                }

                pass->set_pipeline(const_cast<gpu::Pipeline&>(cp.pipeline));

                // Bind inputs by name
                for (uint32_t i = 0; i < cp.desc.inputs.size(); ++i) {
                    const auto& input_name = cp.desc.inputs[i];
                    if (input_name == "backbuffer" && current)
                        pass->bind(i, *current);
                    else if (input_name == "depth" && depth)
                        pass->bind(i, *depth);
                    else if (input_name == "previous" && previous)
                        pass->bind(i, *previous);
                }

                pass->update_params(params);
                pass->draw_fullscreen();
                pass->end();

                previous = current;
                if (cp.rt.valid()) current = &cp.rt;
            }
        }

        // Blit final intermediate back to backbuffer if needed
        // (If the last pass wrote to backbuffer directly, nothing to do)

        if (overlay_cb_) {
            overlay_cb_(device, *current, bb_w, bb_h);
        }
    }

    void set_overlay(OverlayCb cb) override {
        overlay_cb_ = std::move(cb);
    }

    std::vector<EffectDesc*> effects() override {
        std::vector<EffectDesc*> out;
        for (auto& fx : effects_) out.push_back(&fx.desc);
        return out;
    }

private:
    void load_effects(gpu::Device& device) {
        effects_.clear();

        const auto& root = paths_.effects();
        if (!std::filesystem::exists(root)) return;

        std::vector<std::filesystem::path> folders;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory())
                folders.push_back(entry.path());
        }

        for (auto& folder : folders) {
            LoadedEffect fx;
            if (!fx.desc.load(folder)) continue;

            // Compile each pass
            for (const auto& pd : fx.desc.passes) {
                CompiledPass cp;
                cp.desc = pd;

                gpu::PipelineDesc pipe_desc{};

                // Load PS HLSL
                if (!pd.ps_file.empty()) {
                    auto ps_path = folder / pd.ps_file;
                    std::ifstream f(ps_path);
                    if (f.is_open()) {
                        std::ostringstream ss;
                        ss << f.rdbuf();
                        pipe_desc.shaders.ps.hlsl  = ss.str();
                        pipe_desc.shaders.ps.entry = "main";
                    }
                }

                // Load VS HLSL (empty = built-in fullscreen VS)
                if (!pd.vs_file.empty()) {
                    auto vs_path = folder / pd.vs_file;
                    std::ifstream f(vs_path);
                    if (f.is_open()) {
                        std::ostringstream ss;
                        ss << f.rdbuf();
                        pipe_desc.shaders.vs.hlsl  = ss.str();
                        pipe_desc.shaders.vs.entry = "main";
                    }
                }

                auto res = device.create_pipeline(pipe_desc);
                if (!res.has_value()) continue;
                cp.pipeline = std::move(*res);

                // Create intermediate RT for this pass
                uint32_t rt_w = pd.width  ? pd.width  : 1920; // placeholder; resize in run_present
                uint32_t rt_h = pd.height ? pd.height : 1080;
                // Only create an RT if the pass has an output other than the backbuffer
                // For simplicity: create an RT for every pass except the last one in the effect
                bool is_last_pass = (&pd == &fx.desc.passes.back());
                if (!is_last_pass) {
                    vangfx::TextureDesc td{};
                    td.width            = rt_w;
                    td.height           = rt_h;
                    td.format           = vangfx::SurfaceFormat::RGBA8;
                    td.is_render_target = true;
                    auto tr = device.create_texture(td, nullptr);
                    if (tr.has_value()) cp.rt = std::move(*tr);
                }

                fx.compiled_passes.push_back(std::move(cp));
            }

            fx.ready = !fx.compiled_passes.empty();
            effects_.push_back(std::move(fx));
        }

        // Sort by order
        std::sort(effects_.begin(), effects_.end(),
                  [](const LoadedEffect& a, const LoadedEffect& b){
                      return a.desc.order < b.desc.order;
                  });
    }

    vangfx::Paths  paths_;
    vangfx::Config cfg_;
    OverlayCb      overlay_cb_;
    std::vector<LoadedEffect> effects_;
    std::mutex     mutex_;
    bool           dirty_ = true;
};

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<Chain> Chain::create(const vangfx::Paths& paths,
                                     const vangfx::Config& cfg) {
    return std::make_unique<ChainImpl>(paths, cfg);
}

} // namespace vangfx::fx
