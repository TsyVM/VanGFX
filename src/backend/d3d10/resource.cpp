/**
 * src/backend/d3d10/resource.cpp — D3D10 resource-creation hooks.
 *
 * Hooked: CreateVertexShader, CreatePixelShader — to register bytecode hashes.
 */

#include "d3d10_impl.hpp"
#include <compat/d3d10_slots.hpp>
#include <vangfx/core/content_hash.hpp>

namespace vangfx::backend::d3d10 {

namespace dev = ::vangfx::d3d10_slots::device;

namespace {

using CreateVSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*,
                                                const void*, SIZE_T,
                                                ID3D10VertexShader**);
using CreatePSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D10Device*,
                                                const void*, SIZE_T,
                                                ID3D10PixelShader**);

hooks::Site s_create_vs{ dev::CreateVertexShader, "ID3D10Device::CreateVertexShader" };
hooks::Site s_create_ps{ dev::CreatePixelShader,  "ID3D10Device::CreatePixelShader"  };

HRESULT STDMETHODCALLTYPE hook_create_vs(ID3D10Device* self,
                                          const void* bc, SIZE_T bc_size,
                                          ID3D10VertexShader** out) {
    auto orig = s_create_vs.original<CreateVSFn>(self);
    HRESULT hr = orig(self, bc, bc_size, out);
    if (SUCCEEDED(hr) && out && *out && !hub_idle()) {
        const uint64_t h = vangfx::content_hash(bc, bc_size);
        shader_register(*out, h);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_ps(ID3D10Device* self,
                                          const void* bc, SIZE_T bc_size,
                                          ID3D10PixelShader** out) {
    auto orig = s_create_ps.original<CreatePSFn>(self);
    HRESULT hr = orig(self, bc, bc_size, out);
    if (SUCCEEDED(hr) && out && *out && !hub_idle()) {
        const uint64_t h = vangfx::content_hash(bc, bc_size);
        shader_register(*out, h);
    }
    return hr;
}

} // namespace

bool install_device_draw_hooks(ID3D10Device* device) noexcept;  // draw.cpp forward

// Called from context.cpp; installs both draw hooks and resource hooks.
bool install_all_device_hooks(ID3D10Device* device) noexcept {
    bool ok = install_device_draw_hooks(device);
    ok &= s_create_vs.install(device, &hook_create_vs);
    ok &= s_create_ps.install(device, &hook_create_ps);
    return ok;
}

} // namespace vangfx::backend::d3d10
