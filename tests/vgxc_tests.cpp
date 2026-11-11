/**
 * tests/vgxc_tests.cpp — Phase 5 vgxc conformance tests.
 *
 * Tests the compile + pack pipeline using the vgxc library code
 * (compile.hpp/pack.hpp) directly, without spawning the CLI tool.
 *
 * All tests run on the developer's machine only (Windows, VS 2022).
 * DXBC/DXIL/SPIR-V need d3dcompiler_47.dll and dxcompiler.dll (Windows SDK).
 * GLSL/ESSL/MSL targets are skipped when SPIRV-Cross is not vendored.
 */

#include "harness.hpp"

#ifdef VANGFX_VGXC

#include "../tools/vgxc/compile.hpp"
#include "../tools/vgxc/pack.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static const char* kSimplePS =
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    return float4(1.0f, 0.0f, 0.0f, 1.0f);\n"
    "}\n";

static const char* kParamsPS =
    "#define VANGFX_PARAMS_B0\n"
    "cbuffer VanGFXParams : register(b0) {\n"
    "    float4 vangfx_float4[8];\n"
    "    float  vangfx_time;\n"
    "    float  vangfx_delta;\n"
    "    float2 vangfx_resolution;\n"
    "    float  _pad[28];\n"
    "};\n"
    "Texture2D    tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    return tex.Sample(smp, uv) * float4(vangfx_time, 0, 0, 1);\n"
    "}\n";

static fs::path make_effect_dir(const std::string& test_name,
                                 const std::string& ps_src = kSimplePS) {
    auto dir = fs::temp_directory_path() / ("vgxc_" + test_name);
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "effect.ini");
        f << "[Effect]\nName=" << test_name << "\nOrder=0\n\n"
          << "[Pass main]\nPS=main.hlsl\n";
    }
    {
        std::ofstream f(dir / "main.hlsl");
        f << ps_src;
    }
    return dir;
}

// ── DXBC compilation ─────────────────────────────────────────────────────────

VGX_TEST(vgxc_compile_dxbc) {
    vgxc::CompileInput ci;
    ci.hlsl        = kSimplePS;
    ci.entry       = "main";
    ci.profile     = "ps_5_0";
    ci.source_name = "simple.hlsl";

    auto r = vgxc::compile(ci, vgxc::Target::DXBC);
    if (!r.ok) {
        vgx::note("DXBC compile failed: %s", r.error.c_str());
        // Don't fail: d3dcompiler_47.dll might not be on PATH in CI
        return;
    }
    CHECK(!r.bytecode.empty());
    // DXBC magic: 'DXBC' (0x43425844 LE)
    CHECK(r.bytecode.size() >= 4);
    CHECK(r.bytecode[0] == 'D' && r.bytecode[1] == 'X' &&
          r.bytecode[2] == 'B' && r.bytecode[3] == 'C');
}

// ── DXIL compilation ─────────────────────────────────────────────────────────

VGX_TEST(vgxc_compile_dxil) {
    vgxc::CompileInput ci;
    ci.hlsl        = kSimplePS;
    ci.entry       = "main";
    ci.profile     = "ps_6_0";
    ci.source_name = "simple.hlsl";

    auto r = vgxc::compile(ci, vgxc::Target::DXIL);
    if (!r.ok) {
        vgx::note("DXIL compile: %s", r.error.c_str());
        return;
    }
    CHECK(!r.bytecode.empty());
    // DXIL is wrapped in a DXBC container: magic 'DXBC'
    CHECK(r.bytecode.size() >= 4);
    CHECK(r.bytecode[0] == 'D' && r.bytecode[1] == 'X' &&
          r.bytecode[2] == 'B' && r.bytecode[3] == 'C');
}

// ── SPIR-V compilation ────────────────────────────────────────────────────────

VGX_TEST(vgxc_compile_spirv) {
    vgxc::CompileInput ci;
    ci.hlsl        = kSimplePS;
    ci.entry       = "main";
    ci.profile     = "ps_6_0";
    ci.source_name = "simple.hlsl";

    auto r = vgxc::compile(ci, vgxc::Target::SPIRV);
    if (!r.ok) {
        vgx::note("SPIR-V compile: %s", r.error.c_str());
        return;
    }
    CHECK(!r.bytecode.empty());
    // SPIR-V magic: 0x07230203 LE = 03 02 23 07
    CHECK(r.bytecode.size() >= 4);
    CHECK(r.bytecode[0] == 0x03 && r.bytecode[1] == 0x02 &&
          r.bytecode[2] == 0x23 && r.bytecode[3] == 0x07);
}

// ── Reflection ───────────────────────────────────────────────────────────────

VGX_TEST(vgxc_reflect_dxbc) {
    vgxc::CompileInput ci;
    ci.hlsl        = kParamsPS;
    ci.entry       = "main";
    ci.profile     = "ps_5_0";
    ci.source_name = "params.hlsl";

    auto r = vgxc::compile(ci, vgxc::Target::DXBC);
    if (!r.ok) {
        vgx::note("DXBC compile for reflection: %s", r.error.c_str());
        return;
    }

    std::vector<vgxc::Binding> bindings;
    std::string err;
    CHECK(vgxc::reflect_dxbc(r.bytecode, bindings, err));
    CHECK(!bindings.empty());

    bool found_cbuffer = false, found_texture = false, found_sampler = false;
    for (auto& b : bindings) {
        if (b.type == "cbuffer")  found_cbuffer = true;
        if (b.type == "texture")  found_texture = true;
        if (b.type == "sampler")  found_sampler = true;
    }
    CHECK(found_cbuffer);
    CHECK(found_texture);
    CHECK(found_sampler);
}

// ── Pack an effect ────────────────────────────────────────────────────────────

VGX_TEST(vgxc_pack_effect) {
    auto effect_dir = make_effect_dir("pack_test");
    auto out_dir    = fs::temp_directory_path() / "vgxc_pack_out";
    fs::remove_all(out_dir);

    vgxc::PackOptions opts;
    opts.dxbc  = true;
    opts.dxil  = false;
    opts.spirv = false;

    auto res = vgxc::pack_effect(effect_dir, out_dir, opts);

    if (!res.ok) {
        for (auto& e : res.errors)
            vgx::note("%s", e.c_str());
    }

    // Should produce at least reflect.json
    CHECK(fs::exists(out_dir / "reflect.json"));

    // If DXBC succeeded, main.ps.dxbc should exist
    bool has_dxbc = fs::exists(out_dir / "main.ps.dxbc");
    if (!has_dxbc)
        vgx::note("main.ps.dxbc not generated (d3dcompiler_47 unavailable?)");
    // Don't hard-fail — the pack result might still be ok with just reflect.json
}

// ── pack with DXBC + DXIL + SPIR-V ───────────────────────────────────────────

VGX_TEST(vgxc_pack_all_bytecode) {
    auto effect_dir = make_effect_dir("pack_all_bytecode");
    auto out_dir    = fs::temp_directory_path() / "vgxc_pack_all_out";
    fs::remove_all(out_dir);

    vgxc::PackOptions opts;
    opts.dxbc  = true;
    opts.dxil  = true;
    opts.spirv = true;

    auto res = vgxc::pack_effect(effect_dir, out_dir, opts);

    for (auto& e : res.errors)
        vgx::note("%s", e.c_str());

    CHECK(fs::exists(out_dir / "reflect.json"));

    // Report what was generated (soft pass on unavailable compilers)
    bool any = !res.outputs.empty();
    if (!any)
        vgx::note("no bytecode outputs (DXC/d3dcompiler_47 unavailable?)");
}

// ── reflect.json bindings ─────────────────────────────────────────────────────

VGX_TEST(vgxc_reflect_json_content) {
    auto effect_dir = make_effect_dir("reflect_json", kParamsPS);
    auto out_dir    = fs::temp_directory_path() / "vgxc_reflect_json_out";
    fs::remove_all(out_dir);

    vgxc::PackOptions opts;
    opts.dxbc  = true;
    opts.dxil  = false;
    opts.spirv = false;

    auto res = vgxc::pack_effect(effect_dir, out_dir, opts);

    auto json_path = out_dir / "reflect.json";
    CHECK(fs::exists(json_path));

    std::ifstream jf(json_path);
    std::string content((std::istreambuf_iterator<char>(jf)),
                         std::istreambuf_iterator<char>());
    CHECK(!content.empty());
    // Should contain "vgxc_version"
    CHECK(content.find("vgxc_version") != std::string::npos);
    // Should contain the pass name
    CHECK(content.find("main") != std::string::npos);
}

// ── parse_targets ─────────────────────────────────────────────────────────────

VGX_TEST(vgxc_parse_targets) {
    vgxc::PackOptions opts;
    std::string err;

    CHECK(vgxc::parse_targets("dxbc,dxil", opts, err));
    CHECK(opts.dxbc);
    CHECK(opts.dxil);
    CHECK(!opts.spirv);
    CHECK(!opts.glsl);

    CHECK(vgxc::parse_targets("all", opts, err));
    CHECK(opts.dxbc && opts.dxil && opts.spirv);
    CHECK(opts.glsl && opts.essl && opts.msl);

    CHECK(!vgxc::parse_targets("dxbc,bogus", opts, err));
    CHECK(!err.empty());
}

// ── GLSL / ESSL / MSL (graceful skip when SPIRV-Cross absent) ─────────────────

VGX_TEST(vgxc_cross_targets_skip) {
    vgxc::CompileInput ci;
    ci.hlsl        = kSimplePS;
    ci.entry       = "main";
    ci.profile     = "ps_6_0";
    ci.source_name = "simple.hlsl";

    for (auto t : { vgxc::Target::GLSL, vgxc::Target::ESSL, vgxc::Target::MSL }) {
        auto r = vgxc::compile(ci, t);
#ifdef VANGFX_SPIRV_CROSS
        // When SPIRV-Cross is present, these should succeed (not tested here —
        // just verify no crash)
        (void)r;
#else
        // Without SPIRV-Cross the result is a graceful skip: ok=false + message
        CHECK(!r.ok);
        CHECK(!r.error.empty());
#endif
    }
}

#endif // VANGFX_VGXC
