#pragma once

/**
 * src/core/shader_compile.hpp — compiling HLSL to bytes without a context.
 *
 * ShaderBuilder::build() compiles and registers in one step. A backend that has
 * to recompile a shader it already holds for a different target -- D3D12, when
 * a game's pipelines use DXIL and the replacement was built as DXBC, or the
 * other way round -- uses this with the builder it kept.
 */

#include <vangfx/core/error.hpp>
#include <vangfx/core/shader.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vangfx::detail {

// Compiles the builder's source (inline or file) for `profile`: DXC for SM 6.x
// profiles, d3dcompiler otherwise. Macros, include directories, optimisation
// and debug settings are the builder's.
Result<std::vector<uint8_t>> compile_hlsl(const ShaderBuilder& b, const std::string& profile);

// Whether dxcompiler.dll (SM 6) can be loaded. It does not ship with Windows;
// d3dcompiler_47.dll (SM 5 and older) does.
[[nodiscard]] bool dxc_available() noexcept;

// The profile VanGFX compiles a D3D12 shader of `stage` ("ps", "vs") for when
// the caller did not choose one: SM 6.0 when DXC is available, else SM 5.1.
[[nodiscard]] std::string d3d12_default_profile(const char* stage);

} // namespace vangfx::detail
