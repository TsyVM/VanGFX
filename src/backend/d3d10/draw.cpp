/**
 * src/backend/d3d10/draw.cpp — draw and shader hooks on ID3D10Device.
 *
 * Hooked: Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawAuto,
 *         VSSetShader, PSSetShader, GSSetShader.
 *
 * D3D10 does NOT rewrite its own vtable, so none of the heal/settle machinery
 * needed by D3D11 is required here.
 */

#include "d3d10_impl.hpp"
#include <compat/d3d10_slots.hpp>
#include <vangfx/events/draw.hpp>

namespace vangfx::backend::d3d10 {

namespace dev = ::vangfx::d3d10_slots::device;

namespace {

using DrawFn        = void(STDMETHODCALLTYPE*)(ID3D10Device*, UINT, UINT);
using DrawIdxFn     = void(STDMETHODCALLTYPE*)(ID3D10Device*, UINT, UINT, INT);
using DrawInstFn    = void(STDMETHODCALLTYPE*)(ID3D10Device*, UINT, UINT, UINT, UINT);
using DrawIdxInstFn = void(STDMETHODCALLTYPE*)(ID3D10Device*, UINT, UINT, UINT, INT, UINT);
using DrawAutoFn    = void(STDMETHODCALLTYPE*)(ID3D10Device*);
using SetVSFn       = void(STDMETHODCALLTYPE*)(ID3D10Device*, ID3D10VertexShader*);
using SetPSFn       = void(STDMETHODCALLTYPE*)(ID3D10Device*, ID3D10PixelShader*);
using SetGSFn       = void(STDMETHODCALLTYPE*)(ID3D10Device*, ID3D10GeometryShader*);

using hooks::Heal;
hooks::Site s_draw         { dev::Draw,                 "ID3D10Device::Draw",                 Heal::SameModule };
hooks::Site s_draw_idx     { dev::DrawIndexed,          "ID3D10Device::DrawIndexed",          Heal::SameModule };
hooks::Site s_draw_inst    { dev::DrawInstanced,        "ID3D10Device::DrawInstanced",        Heal::SameModule };
hooks::Site s_draw_idx_inst{ dev::DrawIndexedInstanced, "ID3D10Device::DrawIndexedInstanced", Heal::SameModule };
hooks::Site s_draw_auto    { dev::DrawAuto,             "ID3D10Device::DrawAuto",             Heal::SameModule };
hooks::Site s_set_vs       { dev::VSSetShader,          "ID3D10Device::VSSetShader",          Heal::SameModule };
hooks::Site s_set_ps       { dev::PSSetShader,          "ID3D10Device::PSSetShader",          Heal::SameModule };
hooks::Site s_set_gs       { dev::GSSetShader,          "ID3D10Device::GSSetShader",          Heal::SameModule };

static DrawEvent make_event(const DevState& st, DrawType type) noexcept {
    DrawEvent ev{};
    ev.type    = type;
    ev.vs_hash = st.vs_hash;
    ev.ps_hash = st.ps_hash;
    return ev;
}

static void dispatch_draw(ID3D10Device* self, DrawEvent ev) {
    HubReader r;
    DevState* st = dev_state(self);
    if (!st) return;
    ev.vs_hash = st->vs_hash;
    ev.ps_hash = st->ps_hash;
    for (D3D10ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_draw()) continue;
        D3D10FrameContextImpl fc(*ctx, ctx->current_frame_index(), self, st);
        ctx->dispatch_draw(fc, ev);
        if (st->vs_swapped || st->ps_swapped) restore_shaders(self, *st);
    }
}

void STDMETHODCALLTYPE hook_draw(ID3D10Device* self, UINT vc, UINT vs) {
    auto orig = s_draw.original<DrawFn>(self);
    if (hub_idle() || internal()) { orig(self, vc, vs); return; }
    orig(self, vc, vs);
    s_draw.heal(self);
    DrawEvent ev = {}; ev.type = DrawType::Draw;
    ev.vertex_count = vc; ev.start_vertex = vs;
    dispatch_draw(self, ev);
}

void STDMETHODCALLTYPE hook_draw_indexed(ID3D10Device* self, UINT ic, UINT si, INT bv) {
    auto orig = s_draw_idx.original<DrawIdxFn>(self);
    if (hub_idle() || internal()) { orig(self, ic, si, bv); return; }
    orig(self, ic, si, bv);
    s_draw_idx.heal(self);
    DrawEvent ev = {}; ev.type = DrawType::DrawIndexed;
    ev.index_count = ic; ev.start_index = si; ev.base_vertex = bv;
    dispatch_draw(self, ev);
}

void STDMETHODCALLTYPE hook_draw_inst(ID3D10Device* self, UINT vc, UINT ic, UINT sv, UINT si) {
    auto orig = s_draw_inst.original<DrawInstFn>(self);
    if (hub_idle() || internal()) { orig(self, vc, ic, sv, si); return; }
    orig(self, vc, ic, sv, si);
    s_draw_inst.heal(self);
    DrawEvent ev = {}; ev.type = DrawType::DrawInstanced;
    ev.vertex_count = vc; ev.instance_count = ic;
    dispatch_draw(self, ev);
}

void STDMETHODCALLTYPE hook_draw_idx_inst(ID3D10Device* self,
                                           UINT iic, UINT ic, UINT si, INT bv, UINT ii) {
    auto orig = s_draw_idx_inst.original<DrawIdxInstFn>(self);
    if (hub_idle() || internal()) { orig(self, iic, ic, si, bv, ii); return; }
    orig(self, iic, ic, si, bv, ii);
    s_draw_idx_inst.heal(self);
    DrawEvent ev = {}; ev.type = DrawType::DrawIndexedInstanced;
    ev.index_count = iic; ev.instance_count = ic;
    dispatch_draw(self, ev);
}

void STDMETHODCALLTYPE hook_draw_auto(ID3D10Device* self) {
    auto orig = s_draw_auto.original<DrawAutoFn>(self);
    if (hub_idle() || internal()) { orig(self); return; }
    orig(self);
    s_draw_auto.heal(self);
    DrawEvent ev = {}; ev.type = DrawType::DrawAuto;
    dispatch_draw(self, ev);
}

void STDMETHODCALLTYPE hook_set_vs(ID3D10Device* self, ID3D10VertexShader* vs) {
    auto orig = s_set_vs.original<SetVSFn>(self);
    orig(self, vs);
    s_set_vs.heal(self);
    if (hub_idle() || internal()) return;
    HubReader r;
    if (DevState* st = dev_state(self)) {
        st->vs      = vs;
        st->vs_hash = shader_hash(vs);
    }
}

void STDMETHODCALLTYPE hook_set_ps(ID3D10Device* self, ID3D10PixelShader* ps) {
    auto orig = s_set_ps.original<SetPSFn>(self);
    orig(self, ps);
    s_set_ps.heal(self);
    if (hub_idle() || internal()) return;
    HubReader r;
    if (DevState* st = dev_state(self)) {
        st->ps      = ps;
        st->ps_hash = shader_hash(ps);
    }
}

void STDMETHODCALLTYPE hook_set_gs(ID3D10Device* self, ID3D10GeometryShader* /*gs*/) {
    auto orig = s_set_gs.original<SetGSFn>(self);
    orig(self, nullptr); // pass through; GS not tracked
    s_set_gs.heal(self);
}

} // namespace

bool install_device_draw_hooks(ID3D10Device* device) noexcept {
    bool ok = true;
    ok &= s_draw.install(device, &hook_draw);
    ok &= s_draw_idx.install(device, &hook_draw_indexed);
    ok &= s_draw_inst.install(device, &hook_draw_inst);
    ok &= s_draw_idx_inst.install(device, &hook_draw_idx_inst);
    ok &= s_draw_auto.install(device, &hook_draw_auto);
    ok &= s_set_vs.install(device, &hook_set_vs);
    ok &= s_set_ps.install(device, &hook_set_ps);
    ok &= s_set_gs.install(device, &hook_set_gs);
    return ok;
}

void heal_device_draw_hooks(ID3D10Device* device) noexcept {
    s_draw.heal(device);
    s_draw_idx.heal(device);
    s_draw_inst.heal(device);
    s_draw_idx_inst.heal(device);
    s_draw_auto.heal(device);
    s_set_vs.heal(device);
    s_set_ps.heal(device);
    s_set_gs.heal(device);
}

void uninstall_all_hooks() noexcept {
    s_draw.uninstall();
    s_draw_idx.uninstall();
    s_draw_inst.uninstall();
    s_draw_idx_inst.uninstall();
    s_draw_auto.uninstall();
    s_set_vs.uninstall();
    s_set_ps.uninstall();
    s_set_gs.uninstall();
}

void set_vs_unreported(ID3D10Device* dev, ID3D10VertexShader* vs) noexcept {
    auto orig = s_set_vs.original<SetVSFn>(dev);
    InternalScope sc;
    orig(dev, vs);
}

void set_ps_unreported(ID3D10Device* dev, ID3D10PixelShader* ps) noexcept {
    auto orig = s_set_ps.original<SetPSFn>(dev);
    InternalScope sc;
    orig(dev, ps);
}

void restore_shaders(ID3D10Device* dev, DevState& st) noexcept {
    if (st.vs_swapped) { set_vs_unreported(dev, st.vs); st.vs_swapped = false; }
    if (st.ps_swapped) { set_ps_unreported(dev, st.ps); st.ps_swapped = false; }
}

} // namespace vangfx::backend::d3d10
