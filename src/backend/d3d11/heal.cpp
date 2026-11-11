/**
 * src/backend/d3d11/heal.cpp — hooks whose only job is keeping the others in
 * place.
 *
 * The runtime swaps its own device-context entries between code paths as it
 * goes -- Map, Flush and Present all can -- and a hook swapped out misses every
 * call until something repairs it. Every VanGFX device-context thunk repairs
 * the table on its way out; these add the calls a game makes before nearly
 * every draw, so a swap is repaired before the draw it would have hidden.
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>

namespace vangfx::backend::d3d11 {

namespace dcs = ::vangfx::d3d11_slots::device_context;
using hooks::Heal;

namespace {

hooks::Site s_map          { dcs::Map,                    "Map",                    Heal::SameModule };
hooks::Site s_input_layout { dcs::IASetInputLayout,       "IASetInputLayout",       Heal::SameModule };
hooks::Site s_vertex_bufs  { dcs::IASetVertexBuffers,     "IASetVertexBuffers",     Heal::SameModule };
hooks::Site s_index_buf    { dcs::IASetIndexBuffer,       "IASetIndexBuffer",       Heal::SameModule };
hooks::Site s_vs_cbuffers  { dcs::VSSetConstantBuffers,   "VSSetConstantBuffers",   Heal::SameModule };
hooks::Site s_ps_cbuffers  { dcs::PSSetConstantBuffers,   "PSSetConstantBuffers",   Heal::SameModule };
hooks::Site s_vs_srvs      { dcs::VSSetShaderResources,   "VSSetShaderResources",   Heal::SameModule };
hooks::Site s_ps_srvs      { dcs::PSSetShaderResources,   "PSSetShaderResources",   Heal::SameModule };
hooks::Site s_copy         { dcs::CopyResource,           "CopyResource",           Heal::SameModule };
hooks::Site s_update       { dcs::UpdateSubresource,      "UpdateSubresource",      Heal::SameModule };
hooks::Site s_clear_rtv    { dcs::ClearRenderTargetView,  "ClearRenderTargetView",  Heal::SameModule };
hooks::Site s_clear_dsv    { dcs::ClearDepthStencilView,  "ClearDepthStencilView",  Heal::SameModule };

// One thunk shape for all of them: chain, then repair.
template <hooks::Site& site, typename Ret, typename... Args>
Ret STDMETHODCALLTYPE heal_after(ID3D11DeviceContext* self, Args... args) {
    const Healer healer{ self };
    using Fn = Ret(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, Args...);
    return site.template original<Fn>(self)(self, args...);
}

} // namespace

void install_heal_hooks(ID3D11DeviceContext* context) noexcept {
    s_map.install(context, &heal_after<s_map, HRESULT, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*>);
    s_input_layout.install(context, &heal_after<s_input_layout, void, ID3D11InputLayout*>);
    s_vertex_bufs.install(context, &heal_after<s_vertex_bufs, void, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*>);
    s_index_buf.install(context, &heal_after<s_index_buf, void, ID3D11Buffer*, DXGI_FORMAT, UINT>);
    s_vs_cbuffers.install(context, &heal_after<s_vs_cbuffers, void, UINT, UINT, ID3D11Buffer* const*>);
    s_ps_cbuffers.install(context, &heal_after<s_ps_cbuffers, void, UINT, UINT, ID3D11Buffer* const*>);
    s_vs_srvs.install(context, &heal_after<s_vs_srvs, void, UINT, UINT, ID3D11ShaderResourceView* const*>);
    s_ps_srvs.install(context, &heal_after<s_ps_srvs, void, UINT, UINT, ID3D11ShaderResourceView* const*>);
    s_copy.install(context, &heal_after<s_copy, void, ID3D11Resource*, ID3D11Resource*>);
    s_update.install(context, &heal_after<s_update, void, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT>);
    s_clear_rtv.install(context, &heal_after<s_clear_rtv, void, ID3D11RenderTargetView*, const FLOAT*>);
    s_clear_dsv.install(context, &heal_after<s_clear_dsv, void, ID3D11DepthStencilView*, UINT, FLOAT, UINT8>);
    for (hooks::Site* s : { &s_map, &s_input_layout, &s_vertex_bufs, &s_index_buf, &s_vs_cbuffers, &s_ps_cbuffers,
                            &s_vs_srvs, &s_ps_srvs, &s_copy, &s_update, &s_clear_rtv, &s_clear_dsv })
        register_context_site(*s);
}

}  // namespace vangfx::backend::d3d11
