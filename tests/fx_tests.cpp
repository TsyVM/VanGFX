/**
 * tests/fx_tests.cpp — Phase 4 tests for fx layer components.
 *
 * Tests DepthService, EffectDesc, and Chain (using the D3D11 backend as the
 * device so no additional test infrastructure is needed).
 */

#include "harness.hpp"
#include "gpu11.hpp"

#include <vangfx/fx/depth.hpp>
#include <vangfx/fx/effect_desc.hpp>
#include <vangfx/fx/chain.hpp>
#include <vangfx/core/paths.hpp>
#include <vangfx/core/config.hpp>

#include <filesystem>
#include <fstream>

#ifdef VANGFX_D3D11
#include "../src/gpu/d3d11/device_d3d11.hpp"
#endif

namespace fs = std::filesystem;

// ── DepthService ──────────────────────────────────────────────────────────────

VGX_TEST(depth_service_basic) {
    vangfx::fx::DepthService ds;

    // Frame 1: record two depth surfaces, one at backbuffer size (800×600)
    int depth_a = 1, depth_b = 2;
    for (int i = 0; i < 10; ++i) ds.record_draw(&depth_a, 800, 600);
    for (int i = 0; i < 3;  ++i) ds.record_draw(&depth_b, 800, 600);

    ds.end_frame(800, 600);
    CHECK(ds.best_depth() == &depth_a); // most draws
}

VGX_TEST(depth_service_ignores_wrong_size) {
    vangfx::fx::DepthService ds;

    int depth_a = 1, depth_b = 2;
    for (int i = 0; i < 10; ++i) ds.record_draw(&depth_a, 1024, 768); // wrong size
    for (int i = 0; i < 2;  ++i) ds.record_draw(&depth_b, 800, 600);  // correct

    ds.end_frame(800, 600);
    CHECK(ds.best_depth() == &depth_b);
}

VGX_TEST(depth_service_begin_frame_clears) {
    vangfx::fx::DepthService ds;
    int depth_a = 1;
    ds.record_draw(&depth_a, 64, 64);
    ds.end_frame(64, 64);
    CHECK(ds.best_depth() == &depth_a);

    ds.begin_frame();
    ds.end_frame(64, 64);
    CHECK(ds.best_depth() == nullptr);
}

VGX_TEST(depth_service_no_draws) {
    vangfx::fx::DepthService ds;
    ds.end_frame(1920, 1080);
    CHECK(ds.best_depth() == nullptr);
}

VGX_TEST(depth_service_null_ptr_ignored) {
    vangfx::fx::DepthService ds;
    ds.record_draw(nullptr, 64, 64); // should not crash
    ds.end_frame(64, 64);
    CHECK(ds.best_depth() == nullptr);
}

// ── EffectDesc ────────────────────────────────────────────────────────────────

static fs::path make_temp_effect(const std::string& ini_content,
                                  const std::string& folder_name = "TestFX") {
    auto tmp = fs::temp_directory_path() / "vgx_fx_tests" / folder_name;
    fs::create_directories(tmp);
    std::ofstream f(tmp / "effect.ini");
    f << ini_content;
    return tmp;
}

VGX_TEST(effect_desc_load_basic) {
    auto folder = make_temp_effect(
        "[Effect]\n"
        "Name=My Effect\n"
        "Description=A test\n"
        "Order=50\n"
        "\n"
        "[Pass main]\n"
        "PS=main.hlsl\n"
        "Inputs=backbuffer,depth\n",
        "BasicFX");

    vangfx::fx::EffectDesc desc;
    bool ok = desc.load(folder);
    CHECK(ok);
    CHECK_EQ(desc.name, std::string("My Effect"));
    CHECK_EQ(desc.order, 50);
    CHECK_EQ(desc.passes.size(), 1u);
    CHECK_EQ(desc.passes[0].ps_file, std::string("main.hlsl"));
    CHECK_EQ(desc.passes[0].inputs.size(), 2u);
    CHECK_EQ(desc.passes[0].inputs[0], std::string("backbuffer"));
    CHECK_EQ(desc.passes[0].inputs[1], std::string("depth"));
}

VGX_TEST(effect_desc_load_params) {
    auto folder = make_temp_effect(
        "[Effect]\n"
        "Name=ParamFX\n"
        "\n"
        "[Pass main]\n"
        "PS=main.hlsl\n"
        "\n"
        "[Params]\n"
        "Brightness=0.8\n"
        "Contrast=1.2\n",
        "ParamFX");

    vangfx::fx::EffectDesc desc;
    bool ok = desc.load(folder);
    CHECK(ok);
    CHECK_EQ(desc.params.size(), 2u);
    // Brightness should be 0.8
    CHECK_MSG(std::abs(desc.float4[0][0] - 0.8f) < 0.001f, "Brightness mismatch");
}

VGX_TEST(effect_desc_missing_effect_section) {
    auto folder = make_temp_effect(
        "[Pass main]\n"
        "PS=main.hlsl\n",
        "NoSectionFX");

    vangfx::fx::EffectDesc desc;
    bool ok = desc.load(folder);
    CHECK(!ok);
}

VGX_TEST(effect_desc_save_load_user_params) {
    auto folder = make_temp_effect(
        "[Effect]\n"
        "Name=UserParamFX\n"
        "\n"
        "[Pass main]\n"
        "PS=main.hlsl\n"
        "\n"
        "[Params]\n"
        "Alpha=0.5\n",
        "UserParamFX");

    vangfx::fx::EffectDesc desc;
    CHECK(desc.load(folder));

    desc.params[0].value = 0.75f;
    desc.float4[0][0]    = 0.75f;
    CHECK(desc.save_user_params());

    // Reload — user.ini should override Params
    vangfx::fx::EffectDesc desc2;
    CHECK(desc2.load(folder));
    CHECK_MSG(std::abs(desc2.float4[0][0] - 0.75f) < 0.001f, "user.ini override not applied");
}

// ── Chain (D3D11) ─────────────────────────────────────────────────────────────

#ifdef VANGFX_D3D11

static fs::path make_chain_effect(const fs::path& effects_root,
                                   const std::string& name = "FX1",
                                   const std::string& ps_src = "") {
    auto folder = effects_root / name;
    fs::create_directories(folder);

    // Write effect.ini
    {
        std::ofstream f(folder / "effect.ini");
        f << "[Effect]\nName=" << name << "\nOrder=0\n\n[Pass main]\nPS=main.hlsl\nInputs=backbuffer\n";
    }
    // Write main.hlsl
    {
        std::ofstream f(folder / "main.hlsl");
        if (ps_src.empty()) {
            f << "Texture2D tex : register(t0);\n"
              << "SamplerState smp : register(s0);\n"
              << "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
              << "    return tex.Sample(smp, uv);\n"
              << "}\n";
        } else {
            f << ps_src;
        }
    }
    return folder;
}

VGX_TEST(chain_create) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    auto tmp_dir = fs::temp_directory_path() / "vgx_chain_create_test";
    fs::create_directories(tmp_dir / "effects");

    vangfx::Paths paths(tmp_dir);
    vangfx::Config cfg;
    auto chain = vangfx::fx::Chain::create(paths, cfg);
    CHECK(chain != nullptr);
}

VGX_TEST(chain_run_no_effects) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    static constexpr uint32_t W = 64, H = 64;

    auto tmp_dir = fs::temp_directory_path() / "vgx_chain_no_effects";
    fs::create_directories(tmp_dir / "effects");

    vangfx::Paths paths(tmp_dir);
    vangfx::Config cfg;
    auto chain = vangfx::fx::Chain::create(paths, cfg);

    // Create a backbuffer texture (RT)
    vangfx::TextureDesc td{};
    td.width = W; td.height = H;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto bb_res = dev->create_texture(td, nullptr);
    CHECK(bb_res.has_value());

    // run_present with no effects should not crash
    chain->run_present(*dev, *bb_res, nullptr, 0.0f, 0.016f);
    CHECK(true);
}

VGX_TEST(chain_load_one_effect) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    auto tmp_dir = fs::temp_directory_path() / "vgx_chain_one_effect";
    auto effects_root = tmp_dir / "effects";
    fs::create_directories(effects_root);
    make_chain_effect(effects_root, "Passthrough");

    vangfx::Paths paths(tmp_dir);
    paths.with_root(tmp_dir);
    vangfx::Config cfg;
    auto chain = vangfx::fx::Chain::create(paths, cfg);

    static constexpr uint32_t W = 64, H = 64;
    vangfx::TextureDesc td{};
    td.width = W; td.height = H;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto bb_res = dev->create_texture(td, nullptr);
    CHECK(bb_res.has_value());

    chain->run_present(*dev, *bb_res, nullptr, 1.0f, 0.016f);

    // After first run, effects() should have one entry
    auto efx = chain->effects();
    CHECK_EQ(efx.size(), 1u);
    CHECK_EQ(efx[0]->name, std::string("Passthrough"));
}

VGX_TEST(chain_reload) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    auto tmp_dir = fs::temp_directory_path() / "vgx_chain_reload";
    fs::remove_all(tmp_dir);
    auto effects_root = tmp_dir / "effects";
    fs::create_directories(effects_root);
    make_chain_effect(effects_root, "FX1");

    vangfx::Paths paths(tmp_dir);
    paths.with_root(tmp_dir);
    vangfx::Config cfg;
    auto chain = vangfx::fx::Chain::create(paths, cfg);

    vangfx::TextureDesc td{};
    td.width = td.height = 64;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto bb_res = dev->create_texture(td, nullptr);
    CHECK(bb_res.has_value());

    // First run loads effects
    chain->run_present(*dev, *bb_res, nullptr, 0.0f, 0.016f);
    CHECK_EQ(chain->effects().size(), 1u);

    // Add a second effect folder
    make_chain_effect(effects_root, "FX2");

    // reload() + run again
    chain->reload();
    chain->run_present(*dev, *bb_res, nullptr, 0.1f, 0.016f);
    CHECK_EQ(chain->effects().size(), 2u);
}

VGX_TEST(chain_overlay_callback) {
    vgx::Dx11 d11;
    auto dev = vangfx::gpu::make_d3d11_device(d11.dev.Get(), d11.dc.Get());

    auto tmp_dir = fs::temp_directory_path() / "vgx_chain_overlay";
    fs::create_directories(tmp_dir / "effects");

    vangfx::Paths paths(tmp_dir);
    vangfx::Config cfg;
    auto chain = vangfx::fx::Chain::create(paths, cfg);

    bool cb_called = false;
    chain->set_overlay([&](vangfx::gpu::Device&, vangfx::gpu::Texture&, uint32_t, uint32_t) {
        cb_called = true;
    });

    vangfx::TextureDesc td{};
    td.width = td.height = 64;
    td.format = vangfx::SurfaceFormat::RGBA8;
    td.is_render_target = true;
    auto bb_res = dev->create_texture(td, nullptr);
    CHECK(bb_res.has_value());

    chain->run_present(*dev, *bb_res, nullptr, 0.0f, 0.016f);
    CHECK(cb_called);
}

#endif // VANGFX_D3D11
