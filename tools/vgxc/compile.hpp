#pragma once

/**
 * tools/vgxc/compile.hpp — HLSL → DXBC / DXIL / SPIR-V / GLSL / ESSL / MSL
 *
 * DXBC: d3dcompiler_47.dll (D3DCompile), always available on Windows.
 * DXIL: dxcompiler.dll (IDxcCompiler3), from Windows SDK 10.0.17763+.
 * SPIR-V: dxcompiler.dll with -spirv flag.
 * GLSL/ESSL/MSL: SPIRV-Cross (optional; see VANGFX_SPIRV_CROSS).
 */

#include <string>
#include <vector>
#include <cstdint>

namespace vgxc {

enum class Target {
    DXBC,   // D3D9/10/11 — SM2–SM5 bytecode via d3dcompiler_47
    DXIL,   // D3D12 — SM6+ bytecode via DXC
    SPIRV,  // Vulkan — SPIR-V via DXC -spirv
    GLSL,   // OpenGL  — via SPIRV-Cross (optional)
    ESSL,   // GLES    — via SPIRV-Cross (optional)
    MSL,    // Metal   — via SPIRV-Cross (optional)
};

struct CompileInput {
    std::string hlsl;
    std::string entry       = "main";
    std::string profile;      // "ps_5_0" / "ps_6_0" / ...
    std::string source_name;  // filename for error messages
};

struct CompileResult {
    bool                   ok    = false;
    std::string            error;
    std::vector<uint8_t>   bytecode;  // DXBC / DXIL / SPIR-V
    std::string            text;      // GLSL / ESSL / MSL output
};

// Compile HLSL to the given target.
// DXBC uses d3dcompiler_47.dll; DXIL/SPIR-V use dxcompiler.dll (loaded once).
// GLSL/ESSL/MSL require SPIRV-Cross and return ok=false with an error when
// it is not available (build with VANGFX_SPIRV_CROSS to enable).
CompileResult compile(const CompileInput& in, Target target);

// Reflect shader bindings from DXBC bytecode.
// Returns false and sets error on failure.
struct Binding {
    std::string name;
    std::string type;    // "cbuffer" / "texture" / "sampler" / "uav"
    int         reg = 0;
    std::string reg_prefix; // "b" / "t" / "s" / "u"
};

bool reflect_dxbc(const std::vector<uint8_t>& dxbc,
                  std::vector<Binding>& out,
                  std::string& error);

}  // namespace vgxc
