/**
 * tests/translator_tests.cpp — vangfx::d3d9 translator core
 *
 * Phase 3: D3D9ShaderTranslator (SM1-3 → HLSL), FFPShaderGen (FFP → HLSL),
 * FormatConverter, RenderStateTracker.
 *
 * All tests run on the CPU; no GPU hardware required.
 * Shader compile tests use D3DCompile from d3dcompiler.dll.
 */

#include "harness.hpp"

#include <vangfx/translate/d3d9/D3D9ShaderTranslator.h>
#include <vangfx/translate/d3d9/FFPShaderGen.h>
#include <vangfx/translate/d3d9/FormatConverter.h>
#include <vangfx/translate/d3d9/RenderStateTracker.h>

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>

using namespace vgx;
using Microsoft::WRL::ComPtr;

// ── Compile helper ────────────────────────────────────────────────────────────

static bool d3d_compile(const char* hlsl, const char* target, std::string* err = nullptr) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr,
                            "main", target, 0, 0,
                            code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) && err && errors)
        *err = std::string(static_cast<char*>(errors->GetBufferPointer()),
                           errors->GetBufferSize());
    return SUCCEEDED(hr);
}

static std::vector<BYTE> d3d_compile_bytes(const char* hlsl, const char* target) {
    ComPtr<ID3DBlob> code;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr,
                          "main", target, 0, 0, code.GetAddressOf(), nullptr)))
        return {};
    auto* p = static_cast<BYTE*>(code->GetBufferPointer());
    return std::vector<BYTE>(p, p + code->GetBufferSize());
}

// ── D3D9ShaderTranslator ──────────────────────────────────────────────────────

VGX_TEST(translator_ps30_roundtrip) {
    // A minimal ps_3_0 that just returns a constant colour.
    const char* src = R"(
float4 main() : COLOR { return float4(1,0,0,1); }
)";
    auto bytecode = d3d_compile_bytes(src, "ps_3_0");
    if (bytecode.empty()) { return; }  // d3dcompiler not available

    vangfx::d3d9::Sm3TranslateResult result;
    bool translated = vangfx::d3d9::TranslateD3D9Shader(
        reinterpret_cast<const uint32_t*>(bytecode.data()), bytecode.size(), {}, result);
    CHECK(translated);
    CHECK(!result.hlsl.empty());

    // The translated HLSL must compile for ps_5_0.
    std::string err;
    bool ok = d3d_compile(result.hlsl.c_str(), "ps_5_0", &err);
    CHECK_MSG(ok, err.c_str());
}

VGX_TEST(translator_vs30_roundtrip) {
    const char* src = R"(
float4 main(float4 pos : POSITION) : SV_Position { return pos; }
)";
    auto bytecode = d3d_compile_bytes(src, "vs_3_0");
    if (bytecode.empty()) { return; }

    vangfx::d3d9::Sm3TranslateResult result;
    bool translated = vangfx::d3d9::TranslateD3D9Shader(
        reinterpret_cast<const uint32_t*>(bytecode.data()), bytecode.size(), {}, result);
    CHECK(translated);
    CHECK(!result.hlsl.empty());

    std::string err;
    bool ok = d3d_compile(result.hlsl.c_str(), "vs_5_0", &err);
    CHECK_MSG(ok, err.c_str());
}

VGX_TEST(translator_ps20_roundtrip) {
    // ps_2_0 is an older profile; translator should still handle it.
    const char* src = R"(
sampler s0;
float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(s0, uv); }
)";
    auto bytecode = d3d_compile_bytes(src, "ps_2_0");
    if (bytecode.empty()) { return; }

    vangfx::d3d9::Sm3TranslateResult result;
    bool translated = vangfx::d3d9::TranslateD3D9Shader(
        reinterpret_cast<const uint32_t*>(bytecode.data()), bytecode.size(), {}, result);
    CHECK(translated);
    CHECK(!result.hlsl.empty());

    std::string err;
    bool ok = d3d_compile(result.hlsl.c_str(), "ps_5_0", &err);
    CHECK_MSG(ok, err.c_str());
}

// ── FFPShaderGen ──────────────────────────────────────────────────────────────

VGX_TEST(ffp_gen_unlit_notex) {
    // Simplest case: no lighting, no texture. Must compile.
    vangfx::d3d9::RenderStateTracker rs;
    rs.SetRenderState(D3DRS_LIGHTING, FALSE);

    vangfx::d3d9::FFPFixedState fx;
    vangfx::d3d9::FFPPermKey key = vangfx::d3d9::BuildFFPKey(
        rs.State(), fx, D3DFVF_XYZ | D3DFVF_DIFFUSE, nullptr);

    std::string vs = vangfx::d3d9::GenerateFFPVS(key);
    std::string ps = vangfx::d3d9::GenerateFFPPS(key);
    CHECK(!vs.empty());
    CHECK(!ps.empty());

    std::string vsErr, psErr;
    bool vsOk = d3d_compile(vs.c_str(), "vs_5_0", &vsErr);
    bool psOk = d3d_compile(ps.c_str(), "ps_5_0", &psErr);
    CHECK_MSG(vsOk, vsErr.c_str());
    CHECK_MSG(psOk, psErr.c_str());
}

VGX_TEST(ffp_gen_one_tex_stage) {
    vangfx::d3d9::RenderStateTracker rs;
    rs.SetRenderState(D3DRS_LIGHTING, FALSE);
    rs.SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    rs.SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    rs.SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    rs.SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    rs.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    rs.SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

    vangfx::d3d9::FFPFixedState fx;
    vangfx::d3d9::FFPPermKey key = vangfx::d3d9::BuildFFPKey(
        rs.State(), fx, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, nullptr);

    std::string vs = vangfx::d3d9::GenerateFFPVS(key);
    std::string ps = vangfx::d3d9::GenerateFFPPS(key);
    CHECK(!vs.empty());
    CHECK(!ps.empty());

    std::string vsErr, psErr;
    bool vsOk = d3d_compile(vs.c_str(), "vs_5_0", &vsErr);
    bool psOk = d3d_compile(ps.c_str(), "ps_5_0", &psErr);
    CHECK_MSG(vsOk, vsErr.c_str());
    CHECK_MSG(psOk, psErr.c_str());
}

VGX_TEST(ffp_gen_perm_key_size) {
    // Verify the layout assertion that SFOn12 added still holds.
    static_assert(sizeof(vangfx::d3d9::FFPPermKey) ==
                  16 + 8 * sizeof(vangfx::d3d9::FFPStageDesc) + 32,
                  "FFPPermKey layout changed");
}

// ── FormatConverter ───────────────────────────────────────────────────────────

VGX_TEST(format_converter_known_formats) {
    CHECK_EQ(vangfx::d3d9::FormatConverter::ToDxgi(D3DFMT_A8R8G8B8).dxgiFormat, DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK_EQ(vangfx::d3d9::FormatConverter::ToDxgi(D3DFMT_X8R8G8B8).dxgiFormat, DXGI_FORMAT_B8G8R8X8_UNORM);
    CHECK_EQ(vangfx::d3d9::FormatConverter::ToDxgi(D3DFMT_R5G6B5).dxgiFormat,   DXGI_FORMAT_B5G6R5_UNORM);
    CHECK_EQ(vangfx::d3d9::FormatConverter::ToDxgi(D3DFMT_D24S8).dxgiFormat,    DXGI_FORMAT_D24_UNORM_S8_UINT);
    // Unknown format should return DXGI_FORMAT_UNKNOWN.
    CHECK_EQ(vangfx::d3d9::FormatConverter::ToDxgi(static_cast<D3DFORMAT>(0xDEAD)).dxgiFormat,
             DXGI_FORMAT_UNKNOWN);
}

VGX_TEST(format_converter_roundtrip) {
    // A few formats that have a 1:1 D3D9 ↔ DXGI mapping.
    for (auto fmt : { D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_R16F,
                      D3DFMT_G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_R32F }) {
        auto mapping = vangfx::d3d9::FormatConverter::ToDxgi(fmt);
        if (mapping.dxgiFormat == DXGI_FORMAT_UNKNOWN) continue;
        D3DFORMAT back = vangfx::d3d9::FormatConverter::ToD3D9(mapping.dxgiFormat);
        CHECK_EQ(back, fmt);
    }
}

// ── RenderStateTracker ────────────────────────────────────────────────────────

VGX_TEST(render_state_tracker_defaults) {
    vangfx::d3d9::RenderStateTracker rs;
    // D3D9 default: lighting on, z-test on (LESSEQUAL), z-write on.
    CHECK_EQ(rs.State().rs[D3DRS_LIGHTING],      static_cast<DWORD>(TRUE));
    CHECK_EQ(rs.State().rs[D3DRS_ZENABLE],       static_cast<DWORD>(D3DZB_TRUE));
    CHECK_EQ(rs.State().rs[D3DRS_ZWRITEENABLE],  static_cast<DWORD>(TRUE));
    CHECK_EQ(rs.State().rs[D3DRS_ZFUNC],         static_cast<DWORD>(D3DCMP_LESSEQUAL));
}

VGX_TEST(render_state_tracker_set_get) {
    vangfx::d3d9::RenderStateTracker rs;
    rs.SetRenderState(D3DRS_LIGHTING, FALSE);
    CHECK_EQ(rs.State().rs[D3DRS_LIGHTING], static_cast<DWORD>(FALSE));
    rs.SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    CHECK_EQ(rs.State().rs[D3DRS_ALPHABLENDENABLE], static_cast<DWORD>(TRUE));
}
