/**
 * src/core/shader.cpp — ShaderBuilder::build() implementation.
 *
 * Routing logic:
 *   • Pre-compiled bytecode (DXBC / DXIL): wrap and return immediately.
 *   • HLSL for D3D9 / D3D11 (SM ≤ 5.1): compile via d3dcompiler_47.dll.
 *   • HLSL for D3D12 (SM 6.x) or when use_dxc_ is set: compile via
 *     dxcompiler.dll, then validate with dxil.dll if validate_dxil_ is set.
 *   • profile containing "_6_" always routes to DXC regardless of use_dxc_.
 *
 * Both compiler DLLs are loaded at runtime via LoadLibrary / GetProcAddress so
 * the SDK does not carry a hard dependency on either — games ship their own
 * copies and VanGFX finds whichever is loaded first.
 *
 * Error handling:
 *   Every failure returns a VanGFXError value; no exceptions are thrown.
 *   Compiler diagnostic messages are forwarded verbatim in VanGFXError::message.
 */

#include <vangfx/core/shader.hpp>
#include <vangfx/core/context.hpp>
#include <vangfx/core/error.hpp>
#include "shader_compile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// d3dcompiler interface — loaded at runtime, not linked.
#include <d3dcompiler.h>
// D3D_COMPILE_STANDARD_FILE_INCLUDE was not in the June 2010 DXSDK header.
#ifndef D3D_COMPILE_STANDARD_FILE_INCLUDE
#define D3D_COMPILE_STANDARD_FILE_INCLUDE ((ID3DInclude*)(UINT_PTR)1)
#endif

// DXC interfaces — loaded at runtime, not linked.
// initguid.h must precede dxcapi.h in exactly one TU to get CLSID definitions.
#include <initguid.h>
#include <dxcapi.h>

// Microsoft::WRL::ComPtr — needed for ID3DBlob, IDxcResult, etc.
#include <wrl/client.h>

#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  Internal: lazy-load d3dcompiler_47.dll
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct D3DCompilerLib {
    HMODULE          hmod   = nullptr;
    pD3DCompile      compile = nullptr;   // D3DCompile

    static D3DCompilerLib& get() {
        static D3DCompilerLib instance;
        return instance;
    }

    bool loaded() const noexcept { return compile != nullptr; }

    bool load() noexcept {
        if (loaded()) return true;
        // Try the version that ships with the Windows SDK first; fall back to
        // older redistributable copies.
        static const wchar_t* candidates[] = {
            L"d3dcompiler_47.dll",
            L"d3dcompiler_46.dll",
            L"d3dcompiler_43.dll",
        };
        for (auto* name : candidates) {
            hmod = LoadLibraryW(name);
            if (!hmod) continue;
            compile = reinterpret_cast<pD3DCompile>(
                          GetProcAddress(hmod, "D3DCompile"));
            if (compile) return true;
            FreeLibrary(hmod);
            hmod = nullptr;
        }
        return false;
    }

private:
    D3DCompilerLib() { load(); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Internal: lazy-load dxcompiler.dll + dxil.dll
// ─────────────────────────────────────────────────────────────────────────────

struct DXCLib {
    HMODULE             hmod_compiler  = nullptr;
    HMODULE             hmod_validator = nullptr;
    DxcCreateInstanceProc pfn_create   = nullptr;   // from dxcompiler.dll
    DxcCreateInstanceProc pfn_validate = nullptr;   // from dxil.dll

    static DXCLib& get() {
        static DXCLib instance;
        return instance;
    }

    bool compiler_loaded()  const noexcept { return pfn_create   != nullptr; }
    bool validator_loaded() const noexcept { return pfn_validate != nullptr; }

    bool load() noexcept {
        if (!hmod_compiler) {
            hmod_compiler = LoadLibraryW(L"dxcompiler.dll");
            if (hmod_compiler) {
                pfn_create = reinterpret_cast<DxcCreateInstanceProc>(
                    GetProcAddress(hmod_compiler, "DxcCreateInstance"));
                if (!pfn_create) {
                    FreeLibrary(hmod_compiler);
                    hmod_compiler = nullptr;
                }
            }
        }
        if (!hmod_validator) {
            hmod_validator = LoadLibraryW(L"dxil.dll");
            if (hmod_validator) {
                pfn_validate = reinterpret_cast<DxcCreateInstanceProc>(
                    GetProcAddress(hmod_validator, "DxcCreateInstance"));
                if (!pfn_validate) {
                    FreeLibrary(hmod_validator);
                    hmod_validator = nullptr;
                    // Validator is optional — compiler can still be used.
                }
            }
        }
        return compiler_loaded();
    }

private:
    DXCLib() { load(); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Returns true when the shader profile string targets SM 6.x (use DXC).
bool is_sm6(std::string_view profile) noexcept {
    // Profile strings for SM 6: vs_6_0, ps_6_5, cs_6_6, ms_6_5, as_6_5, …
    return profile.find("_6_") != std::string_view::npos;
}

// Read a file from disk into a string.  Returns an error on failure.
Result<std::string> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return gfx_err<std::string>(ShaderError::FileNotFound,
            "Could not open shader source file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Convert a wstring DXC error message to narrow string.
std::string narrow(LPCWSTR ws) {
    if (!ws) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Compile via d3dcompiler (D3D9 / D3D11, SM ≤ 5.1)
// ─────────────────────────────────────────────────────────────────────────────

Result<std::vector<uint8_t>> d3dcompiler_bytes(const ShaderBuilder& b,
                                               const std::string&    source,
                                               const std::string&    profile) {
    auto& lib = D3DCompilerLib::get();
    if (!lib.loaded())
        return gfx_err<std::vector<uint8_t>>(ShaderError::Unsupported,
            "d3dcompiler_47.dll (or compatible) not found in process.");

    std::vector<D3D_SHADER_MACRO> macros;
    macros.reserve(b.shader_macros().size() + 1);
    for (auto& m : b.shader_macros())
        macros.push_back({ m.name.c_str(), m.value.c_str() });
    macros.push_back({ nullptr, nullptr });

    UINT flags1 = 0;
    if (b.optimization_level() == 0)      flags1 |= D3DCOMPILE_SKIP_OPTIMIZATION;
    else if (b.optimization_level() >= 3) flags1 |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (b.emit_debug_info())              flags1 |= D3DCOMPILE_DEBUG;

    const char* src_name = b.source_file_path().empty() ? "<inline>" : b.source_file_path().c_str();
    Microsoft::WRL::ComPtr<ID3DBlob> code_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob;
    const HRESULT hr = lib.compile(source.c_str(), source.size(), src_name, macros.data(),
                                   D3D_COMPILE_STANDARD_FILE_INCLUDE, b.entry_point().c_str(),
                                   profile.c_str(), flags1, 0, &code_blob, &error_blob);
    if (FAILED(hr)) {
        std::string diag;
        if (error_blob)
            diag.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        return gfx_err<std::vector<uint8_t>>(ShaderError::CompileFailed, std::move(diag));
    }
    const auto* p = static_cast<const uint8_t*>(code_blob->GetBufferPointer());
    return std::vector<uint8_t>(p, p + code_blob->GetBufferSize());
}

// Hands compiled bytes to the backend and attaches them to the returned
// Shader, so a caller that wants the blob has it without asking the context.
Result<Shader> register_bytes(const ShaderBuilder& b, std::vector<uint8_t> bytes, ShaderLang lang, Context& ctx) {
    ShaderBuilder wrapped = b;
    wrapped.bytecode(bytes.data(), bytes.size());
    wrapped.lang(lang);
    auto result = ctx.compile_shader(wrapped);
    if (result) result->bytecode = std::move(bytes);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Compile via DXC (D3D12, SM 6.x)
// ─────────────────────────────────────────────────────────────────────────────

Result<std::vector<uint8_t>> dxc_bytes(const ShaderBuilder& b,
                                       const std::string&   source,
                                       const std::string&   profile) {
    auto& lib = DXCLib::get();
    if (!lib.compiler_loaded())
        return gfx_err<std::vector<uint8_t>>(ShaderError::Unsupported,
            "dxcompiler.dll not found in process — required for SM 6.x shaders.");

    // Build IDxcCompiler3.
    Microsoft::WRL::ComPtr<IDxcUtils>     utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    if (FAILED(lib.pfn_create(CLSID_DxcUtils,    IID_PPV_ARGS(&utils)))    ||
        FAILED(lib.pfn_create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        return gfx_err<std::vector<uint8_t>>(ShaderError::Unsupported,
            "DxcCreateInstance failed — dxcompiler.dll may be corrupt.");

    // Wrap source in a DXC buffer.
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> src_blob;
    utils->CreateBlob(source.c_str(), static_cast<UINT32>(source.size()),
                      CP_UTF8, &src_blob);
    DxcBuffer src_buf{ src_blob->GetBufferPointer(),
                       src_blob->GetBufferSize(), CP_UTF8 };

    // Build argument list.
    std::vector<std::wstring> args_storage;
    std::vector<LPCWSTR>      args;

    auto add_arg = [&](std::wstring s) {
        args_storage.push_back(std::move(s));
        args.push_back(args_storage.back().c_str());
    };

    // Source filename (for error messages).
    if (!b.source_file_path().empty()) {
        std::wstring wpath(b.source_file_path().begin(), b.source_file_path().end());
        add_arg(wpath);
    } else {
        add_arg(L"<inline>");
    }

    // Entry point and target profile.
    add_arg(L"-E"); add_arg(std::wstring(b.entry_point().begin(),   b.entry_point().end()));
    add_arg(L"-T"); add_arg(std::wstring(profile.begin(), profile.end()));

    // Optimisation level.
    if (b.optimization_level() == 0)       add_arg(L"-Od");
    else if (b.optimization_level() >= 3)  add_arg(L"-O3");

    if (b.emit_debug_info())               add_arg(L"-Zi");

    // Preprocessor macros: -D NAME=VALUE
    for (auto& m : b.shader_macros()) {
        std::wstring def = L"-D";
        def += std::wstring(m.name.begin(), m.name.end());
        def += L"=";
        def += std::wstring(m.value.begin(), m.value.end());
        add_arg(def);
    }

    // Include directories.
    for (auto& dir : b.shader_include_dirs()) {
        add_arg(L"-I");
        add_arg(std::wstring(dir.begin(), dir.end()));
    }

    // Root-signature define.
    if (!b.root_signature_define().empty()) {
        add_arg(L"-rootsig-define");
        add_arg(std::wstring(b.root_signature_define().begin(),
                              b.root_signature_define().end()));
    }

    // Run compilation.
    Microsoft::WRL::ComPtr<IDxcResult> result;
    HRESULT hr = compiler->Compile(
        &src_buf,
        args.data(), static_cast<UINT32>(args.size()),
        nullptr,   // include handler (nullptr = no file includes)
        IID_PPV_ARGS(&result));

    if (FAILED(hr))
        return gfx_err<std::vector<uint8_t>>(ShaderError::CompileFailed,
            "IDxcCompiler3::Compile returned a fatal HRESULT.");

    // Check for compile errors.
    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> err_blob;
        result->GetErrorBuffer(&err_blob);
        std::string diag;
        if (err_blob)
            diag.assign(static_cast<const char*>(err_blob->GetBufferPointer()),
                        err_blob->GetBufferSize());
        return gfx_err<std::vector<uint8_t>>(ShaderError::CompileFailed, std::move(diag));
    }

    // Extract the DXIL object blob.
    Microsoft::WRL::ComPtr<IDxcBlob> dxil_blob;
    result->GetResult(&dxil_blob);

    // ── Optional DXIL validation ──────────────────────────────────────────────
    if (b.validate_dxil_blob() && lib.validator_loaded()) {
        Microsoft::WRL::ComPtr<IDxcValidator> validator;
        if (SUCCEEDED(lib.pfn_validate(CLSID_DxcValidator, IID_PPV_ARGS(&validator)))) {
            Microsoft::WRL::ComPtr<IDxcOperationResult> val_result;
            validator->Validate(dxil_blob.Get(), DxcValidatorFlags_InPlaceEdit,
                                &val_result);
            HRESULT val_status = S_OK;
            if (val_result) val_result->GetStatus(&val_status);
            if (FAILED(val_status)) {
                Microsoft::WRL::ComPtr<IDxcBlobEncoding> val_err;
                if (val_result) val_result->GetErrorBuffer(&val_err);
                std::string diag = "DXIL validation failed: ";
                if (val_err)
                    diag += std::string(
                        static_cast<const char*>(val_err->GetBufferPointer()),
                        val_err->GetBufferSize());
                return gfx_err<std::vector<uint8_t>>(ShaderError::CompileFailed, std::move(diag));
            }
        }
    }

    const auto* p = static_cast<const uint8_t*>(dxil_blob->GetBufferPointer());
    return std::vector<uint8_t>(p, p + dxil_blob->GetBufferSize());
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  ShaderBuilder::build() — public entry point
// ─────────────────────────────────────────────────────────────────────────────

Result<Shader> ShaderBuilder::build(Context& ctx) const {
    // ── Pre-compiled bytecode path ────────────────────────────────────────────
    //
    // If the caller supplied a blob directly, bypass compilation entirely and
    // hand it straight to the backend context for device upload.
    if (!bytecode_.empty()) {
        auto result = ctx.compile_shader(*this);
        // Reattach the source bytes so the Shader::bytecode field is populated
        // even when compilation was skipped (blob was already provided).
        if (result && result->bytecode.empty())
            result->bytecode = bytecode_;
        return result;
    }

    // ── Resolve HLSL source ───────────────────────────────────────────────────
    std::string source;
    if (!source_file_.empty()) {
        auto r = read_file(source_file_);
        if (!r) return std::unexpected(r.error());
        source = std::move(*r);
    } else if (!inline_source_.empty()) {
        source = inline_source_;
    } else {
        return gfx_err<Shader>(ShaderError::InvalidFormat,
            "ShaderBuilder: no source, source_file, or bytecode supplied.");
    }

    if (profile_.empty())
        return gfx_err<Shader>(ShaderError::InvalidFormat,
            "ShaderBuilder: profile() must be set (e.g. \"vs_5_0\" or \"ps_6_5\").");

    // ── Route to the correct compiler ─────────────────────────────────────────
    const bool dxil = use_dxc_ || is_sm6(profile_);
    auto bytes = dxil ? dxc_bytes(*this, source, profile_) : d3dcompiler_bytes(*this, source, profile_);
    if (!bytes) return std::unexpected(bytes.error());
    return register_bytes(*this, std::move(*bytes), dxil ? ShaderLang::DXIL : ShaderLang::DXBC, ctx);
}

namespace detail {

Result<std::vector<uint8_t>> compile_hlsl(const ShaderBuilder& b, const std::string& profile) {
    std::string source;
    if (!b.source_file_path().empty()) {
        auto r = read_file(b.source_file_path());
        if (!r) return std::unexpected(r.error());
        source = std::move(*r);
    } else if (!b.inline_source_text().empty()) {
        source = b.inline_source_text();
    } else {
        return gfx_err<std::vector<uint8_t>>(ShaderError::InvalidFormat,
            "The shader has no HLSL source to recompile: it was built from bytecode.");
    }
    return is_sm6(profile) ? dxc_bytes(b, source, profile) : d3dcompiler_bytes(b, source, profile);
}

bool dxc_available() noexcept { return DXCLib::get().compiler_loaded(); }

std::string d3d12_default_profile(const char* stage) {
    return std::string(stage) + (dxc_available() ? "_6_0" : "_5_1");
}

} // namespace detail

} // namespace vangfx
