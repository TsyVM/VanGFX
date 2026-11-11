/**
 * src/backend/d3d11/context1.cpp — Direct3D 11.1 to 11.4 on a device context:
 * the calls that rewrite the context's table, and swapping device context
 * states.
 *
 * Measured on Windows 11 with marker hooks in the immediate context's table,
 * WARP and hardware, 32- and 64-bit alike:
 *   ID3D11DeviceContext1::SwapDeviceContextState rewrites most of the table, and
 *   the first call into the state swapped in rewrites it again -- any call but
 *   GetType and GetContextFlags, a Get included;
 *   ID3D11Multithread::SetMultithreadProtected does the same (Media Foundation
 *   turns protection on to play video on the game's device);
 *   ID3D11DeviceContext3::Flush1 and ID3D11DeviceContext4::Signal and Wait
 *   rewrite the draws, as Present does.
 * The other 11.1 calls (UpdateSubresource1, CopySubresourceRegion1, the
 * *SetConstantBuffers1, ClearView, DiscardResource, DiscardView) and the
 * annotation calls write nothing.
 *
 * So each of those is hooked to settle the table (settle_context): after the
 * call, one harmless call into the context makes the runtime finish its
 * rewrite, and every hook is healed -- before the game's next call, which may
 * be one VanGFX does not hook, followed by a draw.
 *
 * A device context state holds every binding, and SwapDeviceContextState swaps
 * them all at once; Direct2D does it around its own drawing on the game's
 * device. The bindings VanGFX follows are saved with the state swapped out and
 * put back when it is swapped in again (context_state_swapped), so the game's
 * draws after Direct2D's report the game's shaders, not Direct2D's.
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>
#include <d3d11_4.h>

namespace vangfx::backend::d3d11 {

namespace dcs = ::vangfx::d3d11_slots::device_context;
namespace mt  = ::vangfx::d3d11_slots::multithread;
using hooks::Heal;

namespace {

using SwapFn    = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);
using Flush1Fn  = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext3*, D3D11_CONTEXT_TYPE, HANDLE);
using FenceFn   = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext4*, ID3D11Fence*, UINT64);
using ProtectFn = BOOL(STDMETHODCALLTYPE*)(ID3D11Multithread*, BOOL);

hooks::Site s_swap   { dcs::SwapDeviceContextState, "SwapDeviceContextState", Heal::SameModule };
hooks::Site s_flush1 { dcs::Flush1,                 "Flush1",                 Heal::SameModule };
hooks::Site s_signal { dcs::Signal,                 "Signal",                 Heal::SameModule };
hooks::Site s_wait   { dcs::Wait,                   "Wait",                   Heal::SameModule };
hooks::Site s_protect{ mt::SetMultithreadProtected, "ID3D11Multithread::SetMultithreadProtected" };

void STDMETHODCALLTYPE hook_swap(ID3D11DeviceContext1* self, ID3DDeviceContextState* state, ID3DDeviceContextState** previous) {
    // The state swapped out is asked for whether or not the caller does: it
    // names the bindings being saved.
    ID3DDeviceContextState* outgoing = nullptr;
    s_swap.original<SwapFn>(self)(self, state, &outgoing);
    settle_context(self);
    if (state && !hub_idle() && !internal()) {
        HubReader reader;
        if (DcState* st = dc_state(self)) {
            context_state_swapped(self, *st, outgoing, state);
            capture_bind(self, *st);
        }
    }
    if (previous) *previous = outgoing;          // the caller's reference
    else if (outgoing) outgoing->Release();
}

void STDMETHODCALLTYPE hook_flush1(ID3D11DeviceContext3* self, D3D11_CONTEXT_TYPE type, HANDLE event) {
    s_flush1.original<Flush1Fn>(self)(self, type, event);
    settle_context(self);
}

HRESULT STDMETHODCALLTYPE hook_signal(ID3D11DeviceContext4* self, ID3D11Fence* fence, UINT64 value) {
    const HRESULT hr = s_signal.original<FenceFn>(self)(self, fence, value);
    settle_context(self);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_wait(ID3D11DeviceContext4* self, ID3D11Fence* fence, UINT64 value) {
    const HRESULT hr = s_wait.original<FenceFn>(self)(self, fence, value);
    settle_context(self);
    return hr;
}

// ID3D11Multithread is an interface of the context object with a table of its
// own; the context's table is the one to settle.
BOOL STDMETHODCALLTYPE hook_protect(ID3D11Multithread* self, BOOL on) {
    const BOOL was = s_protect.original<ProtectFn>(self)(self, on);
    ID3D11DeviceContext* dc = nullptr;
    if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&dc))) && dc) {
        settle_context(dc);
        dc->Release();
    }
    return was;
}

template <typename I>
bool answers(ID3D11DeviceContext* dc) noexcept {
    I* p = nullptr;
    if (FAILED(dc->QueryInterface(IID_PPV_ARGS(&p))) || !p) return false;
    p->Release();
    return true;
}

} // namespace

void install_context1_hooks(ID3D11DeviceContext* dc) noexcept {
    // Each on the context's own table, and only when the table has the entry:
    // one that does not answer for the interface has a shorter table.
    if (answers<ID3D11DeviceContext1>(dc)) {
        s_swap.install(dc, &hook_swap);
        register_ext_context_site(s_swap);
    }
    if (answers<ID3D11DeviceContext3>(dc)) {
        s_flush1.install(dc, &hook_flush1);
        register_ext_context_site(s_flush1);
    }
    if (answers<ID3D11DeviceContext4>(dc)) {
        s_signal.install(dc, &hook_signal);
        s_wait.install(dc, &hook_wait);
        register_ext_context_site(s_signal);
        register_ext_context_site(s_wait);
    }
    ID3D11Multithread* m = nullptr;
    if (SUCCEEDED(dc->QueryInterface(IID_PPV_ARGS(&m))) && m) {
        s_protect.install(m, &hook_protect);
        m->Release();
    }
}

void uninstall_context1_hooks() noexcept {
    s_protect.uninstall();   // the extended context sites go with the other context sites
}

}  // namespace vangfx::backend::d3d11
