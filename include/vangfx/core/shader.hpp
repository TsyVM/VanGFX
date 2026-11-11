#pragma once

/**
 * vangfx/core/shader.hpp — Shader, ShaderBuilder, and Effect types.
 *
 * Shader objects are immutable once built.  Build them once at startup
 * and reuse them every frame.  ShaderBuilder follows the builder pattern;
 * call build() to obtain a Result<Shader>.
 *
 * Effect bundles a vertex + pixel (+ optional geometry/hull/domain) shader
 * pair into a single activation unit.  bind() installs all stages at once.
 */

#include "error.hpp"
#include <vangfx/resources/buffer.hpp>  // ShaderStage
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  ShaderHandle — opaque reference to a compiled GPU shader program
// ─────────────────────────────────────────────────────────────────────────────

struct ShaderHandle { uint64_t id = 0; };

[[nodiscard]] inline bool is_valid(ShaderHandle h) noexcept { return h.id != 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  ShaderLang — source language of the input text
// ─────────────────────────────────────────────────────────────────────────────

enum class ShaderLang : uint8_t {
    HLSL,    // HLSL source; compiled via d3dcompiler.dll (D3D9/D3D11) or
             // DXC (D3D12) depending on the active backend and profile string.
    DXBC,    // pre-compiled DXBC bytecode blob (D3D9 / D3D11, SM 2.x–5.1)
    DXIL,    // pre-compiled DXIL bytecode blob (D3D12, SM 6.0+)
             // Must be signed by the DXC validator before device use.
             // Use ShaderBuilder::bytecode() to supply a pre-validated blob.
};



// ─────────────────────────────────────────────────────────────────────────────
//  PSOHandle — forward declaration; full definition in d3d12/pso.hpp
// ─────────────────────────────────────────────────────────────────────────────

// ShaderStage is defined in buffer.hpp (included transitively via context.hpp).
// Amplification and Mesh stages are D3D12 mesh-shader pipeline only.
// They extend the enum defined in buffer.hpp; access via ShaderStage::Amplification
// after including d3d12/pso.hpp which re-opens the enum.
// (Defined here as comments to document the extension points without redefining
// the enum that buffer.hpp owns.)
//
//   ShaderStage::Amplification  — mesh pipeline amplification shader (D3D12)
//   ShaderStage::Mesh           — mesh pipeline mesh shader           (D3D12)

// ─────────────────────────────────────────────────────────────────────────────
//  Shader — a single compiled shader stage
// ─────────────────────────────────────────────────────────────────────────────

struct Shader {
    ShaderHandle         handle;
    ShaderStage          stage    = {};
    ShaderLang           lang     = ShaderLang::HLSL;
    std::string          entry;      // entry-point name (e.g. "VSMain")
    std::string          profile;    // shader model profile (e.g. "vs_5_0")

    // Compiled bytecode (DXBC or DXIL).  Populated by ShaderBuilder::build()
    // so callers that need the raw blob (e.g. D3D12 shadow PSO construction)
    // can access it without a separate context round-trip.
    // Empty when the Shader was produced by a path that does not materialise
    // the blob on the CPU (e.g. a backend that compiles asynchronously).
    std::vector<uint8_t> bytecode;

    [[nodiscard]] bool valid() const noexcept { return is_valid(handle); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ShaderMacro — #define passed to the HLSL compiler
// ─────────────────────────────────────────────────────────────────────────────

struct ShaderMacro {
    std::string name;
    std::string value;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ShaderBuilder — fluent API for compiling or loading shaders
//
//  Usage (HLSL from file):
//      auto shader = ShaderBuilder{}
//          .source_file("shaders/my_vs.hlsl")
//          .entry("VSMain")
//          .profile("vs_5_0")
//          .define("SHADOW_PASS", "1")
//          .build(ctx);
//
//  Usage (inline HLSL source):
//      auto shader = ShaderBuilder{}
//          .source(hlsl_text)
//          .entry("PSMain")
//          .profile("ps_5_0")
//          .build(ctx);
//
//  Usage (pre-compiled bytecode blob):
//      auto shader = ShaderBuilder{}
//          .bytecode(blob_ptr, blob_size)
//          .stage(ShaderStage::Vertex)
//          .build(ctx);
// ─────────────────────────────────────────────────────────────────────────────

class Context; // forward

class ShaderBuilder {
public:
    ShaderBuilder& source_file(std::string path) {
        source_file_ = std::move(path);
        return *this;
    }
    ShaderBuilder& source(std::string hlsl) {
        inline_source_ = std::move(hlsl);
        return *this;
    }
    ShaderBuilder& bytecode(const void* data, size_t size) {
        bytecode_.assign(
            static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + size);
        return *this;
    }
    ShaderBuilder& entry(std::string_view ep) {
        entry_ = ep;
        return *this;
    }
    ShaderBuilder& profile(std::string_view sm) {
        profile_ = sm;
        return *this;
    }
    ShaderBuilder& stage(ShaderStage s) {
        stage_ = s;
        return *this;
    }
    ShaderBuilder& lang(ShaderLang l) {
        lang_ = l;
        return *this;
    }
    ShaderBuilder& define(std::string name, std::string value = "1") {
        macros_.push_back({ std::move(name), std::move(value) });
        return *this;
    }
    ShaderBuilder& include_dir(std::string dir) {
        include_dirs_.push_back(std::move(dir));
        return *this;
    }
    ShaderBuilder& optimization(uint32_t level) {   // 0 = off, 3 = max
        opt_level_ = level;
        return *this;
    }
    ShaderBuilder& debug_info(bool enable = true) {
        debug_info_ = enable;
        return *this;
    }
    // ── D3D12 / DXC-specific options ──────────────────────────────────────────

    // Force DXC (dxcompiler.dll) even when the profile could be compiled by
    // d3dcompiler.  Required for SM 6.x profiles (cs_6_0, ps_6_5, etc.).
    // Ignored on D3D9/D3D11 backends.
    ShaderBuilder& use_dxc(bool enable = true) {
        use_dxc_ = enable;
        return *this;
    }
    // DXIL validation: when true (default for release builds), the compiled
    // DXIL blob is run through the DXC validator before being handed to D3D12.
    // Disable only for faster iteration during shader development.
    ShaderBuilder& validate_dxil(bool enable = true) {
        validate_dxil_ = enable;
        return *this;
    }
    // Root signature embedded in the shader via [RootSignature("")] attribute.
    // When non-empty the string is forwarded as the -rootsig-define DXC flag.
    ShaderBuilder& root_signature_define(std::string_view define) {
        root_sig_define_ = define;
        return *this;
    }

    // Forward-declared; implemented in the vangfx runtime library.
    [[nodiscard]] Result<Shader> build(Context& ctx) const;

    // Read-only accessors used by internal compiler helpers (shader.cpp).
    [[nodiscard]] const std::string&              source_file_path()     const noexcept { return source_file_; }
    [[nodiscard]] const std::string&              inline_source_text()   const noexcept { return inline_source_; }
    [[nodiscard]] const std::vector<uint8_t>&     precompiled_bytecode() const noexcept { return bytecode_; }
    [[nodiscard]] const std::string&              entry_point()          const noexcept { return entry_; }
    [[nodiscard]] const std::string&              shader_profile()       const noexcept { return profile_; }
    [[nodiscard]] const std::vector<ShaderMacro>& shader_macros()        const noexcept { return macros_; }
    [[nodiscard]] const std::vector<std::string>& shader_include_dirs()  const noexcept { return include_dirs_; }
    [[nodiscard]] uint32_t                        optimization_level()   const noexcept { return opt_level_; }
    [[nodiscard]] bool                            emit_debug_info()      const noexcept { return debug_info_; }
    [[nodiscard]] bool                            use_dxc_compiler()     const noexcept { return use_dxc_; }
    [[nodiscard]] bool                            validate_dxil_blob()   const noexcept { return validate_dxil_; }
    [[nodiscard]] const std::string&              root_signature_define()const noexcept { return root_sig_define_; }

private:
    std::string              source_file_;
    std::string              inline_source_;
    std::vector<uint8_t>     bytecode_;
    std::string              entry_    = "main";
    std::string              profile_;
    ShaderStage              stage_    = {};
    ShaderLang               lang_     = ShaderLang::HLSL;
    std::vector<ShaderMacro> macros_;
    std::vector<std::string> include_dirs_;
    uint32_t                 opt_level_ = 3;
    bool                     debug_info_ = false;
    // D3D12 / DXC options
    bool                     use_dxc_        = false;
    bool                     validate_dxil_  = true;
    std::string              root_sig_define_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  EffectHandle — opaque reference to a bound shader effect
// ─────────────────────────────────────────────────────────────────────────────

struct EffectHandle { uint64_t id = 0; };

[[nodiscard]] inline bool is_valid(EffectHandle h) noexcept { return h.id != 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  Effect — a linked shader program across one or more pipeline stages
//
//  At minimum an Effect holds a vertex + pixel shader.  Geometry, hull, and
//  domain shaders are optional.  For compute-only work, only compute is set.
//
//  Call Context::bind_effect(effect) to activate all stages at once and
//  Context::unbind_effect() to restore the shaders that were active before.
// ─────────────────────────────────────────────────────────────────────────────

struct Effect {
    EffectHandle handle;
    Shader       vs;       // vertex shader  (required for graphics effects)
    Shader       ps;       // pixel shader   (required for graphics effects)
    Shader       gs;       // geometry shader (optional, D3D11 only)
    Shader       hs;       // hull shader     (optional, D3D11 only)
    Shader       ds;       // domain shader   (optional, D3D11 only)
    Shader       cs;       // compute shader  (set for compute-only effects)

    [[nodiscard]] bool valid()        const noexcept { return is_valid(handle); }
    [[nodiscard]] bool is_compute()   const noexcept { return is_valid(cs.handle); }
    [[nodiscard]] bool is_graphics()  const noexcept { return is_valid(vs.handle); }
};

} // namespace vangfx
