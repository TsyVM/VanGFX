#include "compile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace vgxc {

// ── d3dcompiler_47 (DXBC) ────────────────────────────────────────────────────

using PFN_D3DReflect = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);

static HMODULE     s_d3dc         = nullptr;
static pD3DCompile s_d3d_compile  = nullptr;
static PFN_D3DReflect s_d3d_reflect = nullptr;
static std::once_flag s_d3dc_once;

static bool load_d3dcompiler() {
    std::call_once(s_d3dc_once, [] {
        s_d3dc = LoadLibraryW(L"d3dcompiler_47.dll");
        if (s_d3dc) {
            s_d3d_compile = reinterpret_cast<pD3DCompile>(
                GetProcAddress(s_d3dc, "D3DCompile"));
            s_d3d_reflect = reinterpret_cast<PFN_D3DReflect>(
                GetProcAddress(s_d3dc, "D3DReflect"));
        }
    });
    return s_d3d_compile != nullptr;
}

static CompileResult compile_dxbc(const CompileInput& in) {
    CompileResult r;
    if (!load_d3dcompiler()) {
        r.error = "d3dcompiler_47.dll not found";
        return r;
    }

    // Decide SM profile: map generic "ps"/"vs" to SM 5.0
    std::string profile = in.profile;
    if (profile.empty()) {
        // Guess from entry suffix convention
        profile = (in.source_name.find("vs.") != std::string::npos ||
                   in.source_name.find("_vs") != std::string::npos)
                      ? "vs_5_0" : "ps_5_0";
    }

    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = s_d3d_compile(
        in.hlsl.c_str(), in.hlsl.size(),
        in.source_name.empty() ? nullptr : in.source_name.c_str(),
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        in.entry.c_str(), profile.c_str(),
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &code, &errors);

    if (errors && errors->GetBufferSize())
        r.error = static_cast<const char*>(errors->GetBufferPointer());

    if (FAILED(hr) || !code) return r;

    r.bytecode.assign(
        static_cast<const uint8_t*>(code->GetBufferPointer()),
        static_cast<const uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
    r.ok = true;
    return r;
}

// ── DXC (DXIL / SPIR-V) ──────────────────────────────────────────────────────

static HMODULE          s_dxc_lib   = nullptr;
static DxcCreateInstanceProc s_dxc_create = nullptr;
static std::once_flag   s_dxc_once;

static bool load_dxc() {
    std::call_once(s_dxc_once, [] {
        s_dxc_lib = LoadLibraryW(L"dxcompiler.dll");
        if (s_dxc_lib)
            s_dxc_create = reinterpret_cast<DxcCreateInstanceProc>(
                GetProcAddress(s_dxc_lib, "DxcCreateInstance"));
    });
    return s_dxc_create != nullptr;
}

static CompileResult compile_dxc(const CompileInput& in, bool spirv) {
    CompileResult r;
    if (!load_dxc()) {
        r.error = "dxcompiler.dll not found";
        return r;
    }

    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(s_dxc_create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        r.error = "DxcCreateInstance(IDxcCompiler3) failed";
        return r;
    }
    ComPtr<IDxcUtils> utils;
    if (FAILED(s_dxc_create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
        r.error = "DxcCreateInstance(IDxcUtils) failed";
        return r;
    }

    // Build source blob
    ComPtr<IDxcBlobEncoding> src_blob;
    utils->CreateBlobFromPinned(in.hlsl.c_str(),
                                 static_cast<UINT32>(in.hlsl.size()),
                                 DXC_CP_UTF8, &src_blob);

    // Decide profile (SM 6.0 for DXIL / SPIR-V)
    std::string profile = in.profile;
    if (profile.empty()) {
        profile = (in.source_name.find("vs.") != std::string::npos ||
                   in.source_name.find("_vs") != std::string::npos)
                      ? "vs_6_0" : "ps_6_0";
    }
    // Upgrade SM 5 → SM 6 for DXC if the profile is SM 5
    if (profile.size() >= 4 && profile[profile.size()-3] == '5')
        profile[profile.size()-3] = '6';

    std::vector<LPCWSTR> args;
    std::wstring w_profile(profile.begin(), profile.end());
    std::wstring w_entry(in.entry.begin(), in.entry.end());
    std::wstring w_name(in.source_name.begin(), in.source_name.end());

    args.push_back(w_name.empty() ? L"<hlsl>" : w_name.c_str());
    args.push_back(L"-T"); args.push_back(w_profile.c_str());
    args.push_back(L"-E"); args.push_back(w_entry.c_str());
    args.push_back(L"-Od");
    if (spirv) {
        args.push_back(L"-spirv");
        args.push_back(L"-fspv-target-env=vulkan1.1");
        args.push_back(L"-fvk-use-dx-layout");
    }

    DxcBuffer src_buf{ src_blob->GetBufferPointer(),
                       src_blob->GetBufferSize(), DXC_CP_UTF8 };
    ComPtr<IDxcResult> result;
    HRESULT hr = compiler->Compile(&src_buf,
                                    args.data(),
                                    static_cast<UINT32>(args.size()),
                                    nullptr, IID_PPV_ARGS(&result));

    // Extract errors
    ComPtr<IDxcBlobUtf8> err_blob;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS,
                                     IID_PPV_ARGS(&err_blob), nullptr))
            && err_blob && err_blob->GetStringLength())
        r.error = err_blob->GetStringPointer();

    HRESULT compile_status = S_OK;
    result->GetStatus(&compile_status);
    if (FAILED(hr) || FAILED(compile_status)) return r;

    ComPtr<IDxcBlob> out_blob;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT,
                                  IID_PPV_ARGS(&out_blob), nullptr)) || !out_blob)
        return r;

    r.bytecode.assign(
        static_cast<const uint8_t*>(out_blob->GetBufferPointer()),
        static_cast<const uint8_t*>(out_blob->GetBufferPointer()) +
            out_blob->GetBufferSize());
    r.ok = true;
    return r;
}

// ── SPIRV-Cross stubs ─────────────────────────────────────────────────────────

#ifdef VANGFX_SPIRV_CROSS
#  include "cross_impl.hpp"  // linked when SPIRV-Cross source is vendored
#else
static CompileResult cross_not_available(Target t) {
    static constexpr const char* names[] = {
        "dxbc", "dxil", "spirv", "glsl", "essl", "msl" };
    CompileResult r;
    r.error = std::string("SPIRV-Cross not available; ") +
              names[static_cast<int>(t)] + " target skipped";
    return r;
}
#endif

// ── Public API ────────────────────────────────────────────────────────────────

CompileResult compile(const CompileInput& in, Target target) {
    switch (target) {
    case Target::DXBC:  return compile_dxbc(in);
    case Target::DXIL:  return compile_dxc(in, false);
    case Target::SPIRV: return compile_dxc(in, true);
    case Target::GLSL:
    case Target::ESSL:
    case Target::MSL:
#ifdef VANGFX_SPIRV_CROSS
        return cross_compile(in, target);
#else
        return cross_not_available(target);
#endif
    }
    return {};
}

// ── Reflection ───────────────────────────────────────────────────────────────

bool reflect_dxbc(const std::vector<uint8_t>& dxbc,
                  std::vector<Binding>& out,
                  std::string& error) {
    if (!load_d3dcompiler() || !s_d3d_reflect) {
        error = "d3dcompiler_47.dll not found";
        return false;
    }

    ComPtr<ID3D11ShaderReflection> refl;
    HRESULT hr = s_d3d_reflect(dxbc.data(), dxbc.size(),
                                IID_ID3D11ShaderReflection,
                                reinterpret_cast<void**>(refl.GetAddressOf()));
    if (FAILED(hr)) { error = "D3DReflect failed"; return false; }

    D3D11_SHADER_DESC sd{};
    refl->GetDesc(&sd);

    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        refl->GetResourceBindingDesc(i, &bd);

        Binding b;
        b.name = bd.Name;
        b.reg  = static_cast<int>(bd.BindPoint);

        switch (bd.Type) {
        case D3D_SIT_CBUFFER:
            b.type = "cbuffer"; b.reg_prefix = "b"; break;
        case D3D_SIT_TEXTURE:
        case D3D_SIT_STRUCTURED:
        case D3D_SIT_BYTEADDRESS:
            b.type = "texture"; b.reg_prefix = "t"; break;
        case D3D_SIT_SAMPLER:
            b.type = "sampler"; b.reg_prefix = "s"; break;
        case D3D_SIT_UAV_RWTYPED:
        case D3D_SIT_UAV_RWSTRUCTURED:
        case D3D_SIT_UAV_RWBYTEADDRESS:
            b.type = "uav"; b.reg_prefix = "u"; break;
        default:
            b.type = "unknown"; b.reg_prefix = "?"; break;
        }
        out.push_back(std::move(b));
    }
    return true;
}

}  // namespace vgxc
