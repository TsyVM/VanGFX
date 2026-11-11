// Runs the generated fixed-function shaders under D3D12.
//
// Shares the shader generator with the D3D11 backend -- the same permutation
// key produces the same HLSL -- and differs only in compiling for D3D12
// targets and in owning the constant buffer as a D3D12 resource.
//
// The permutation cache matters more here than it might appear. The key is
// built from render state that changes constantly, but the number of distinct
// combinations a game reaches is small; without the cache, HLSL compilation
// would happen mid-frame.

#pragma once

#ifndef VANGFX_D3D9TO12_DX12_FFPEMULATOR12_H
#define VANGFX_D3D9TO12_DX12_FFPEMULATOR12_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <synchapi.h>
#include <memory>
#include <unordered_map>
#include <vector>

#include <vangfx/translate/d3d9/FFPShaderGen.h>
#include <vangfx/translate/targets/d3d9_to_d3d12/core/ShaderCache12.h>

namespace vangfx::d3d9to12 {

using vangfx::d3d9::FFPPermKey;
using vangfx::d3d9::FFPPermKeyHash;

class DeviceContext12;

struct FFPProgram12 {
    CompiledShader12 vs;
    CompiledShader12 ps;
    bool             vsValid{ false };
    bool             psValid{ false };
};

class FFPEmulator12 {
public:
    explicit FFPEmulator12(DeviceContext12* ctx) noexcept : m_ctx(ctx) {}

    HRESULT GetOrCompile(const FFPPermKey& key, const FFPProgram12** ppOut) noexcept;

    [[nodiscard]] size_t PermutationCount() const noexcept;

private:
    HRESULT Compile(const FFPPermKey& key, FFPProgram12* out) noexcept;

    DeviceContext12* m_ctx{ nullptr };
    mutable SRWLOCK  m_lock = SRWLOCK_INIT;
    std::unordered_map<FFPPermKey, std::unique_ptr<FFPProgram12>, FFPPermKeyHash> m_cache;
};

}

#endif
