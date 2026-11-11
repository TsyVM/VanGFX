/**
 * src/backend/d3d9/query.cpp — CreateQuery on the device, Issue and GetData on
 * the query.
 *
 * Direct3D 9 has no Begin/End on the device: a query is driven through its own
 * object, Issue(D3DISSUE_BEGIN / D3DISSUE_END) and GetData. Every query shares
 * one table in d3d9.dll, hooked once through a query made for the purpose (the
 * old code patched each query's shared table as it was created and chained
 * through the slot it had just patched). Each query is reported under an id it
 * keeps for its lifetime; GetData after the call, with the result decoded when
 * the GPU had it ready (S_OK).
 */

#include "d3d9_impl.hpp"
#include <compat/d3d9_slots.hpp>
#include <vangfx/events/query.hpp>

#include <cstring>

namespace vangfx::backend::d3d9 {

namespace s = ::vangfx::d3d9_slots;
using hooks::Heal;

namespace {

using CreateQueryFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
using IssueFn       = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, DWORD);
using GetDataFn     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, void*, DWORD, DWORD);

hooks::Site s_create_query{ s::CreateQuery,    "CreateQuery", Heal::SameModule };
hooks::Site s_issue       { s::query::Issue,   "IDirect3DQuery9::Issue" };
hooks::Site s_get_data    { s::query::GetData, "IDirect3DQuery9::GetData" };

void decode(QueryEvent& ev, D3DQUERYTYPE raw, const void* data, DWORD size) noexcept {
    if (!data || !size) return;
    ev.result_ready = true;
    switch (raw) {
    case D3DQUERYTYPE_OCCLUSION:        // a DWORD count of samples
        if (size >= sizeof(DWORD)) { DWORD n = 0; std::memcpy(&n, data, sizeof n); ev.occlusion_samples = n; }
        break;
    case D3DQUERYTYPE_TIMESTAMP:        // a UINT64 tick
        if (size >= sizeof(uint64_t)) std::memcpy(&ev.timestamp_ticks, data, sizeof(uint64_t));
        break;
    case D3DQUERYTYPE_TIMESTAMPFREQ:    // a UINT64 frequency in Hz
        if (size >= sizeof(uint64_t)) std::memcpy(&ev.frequency_hz, data, sizeof(uint64_t));
        break;
    default:                            // BOOLs (event, disjoint) and the statistics structures
        std::memcpy(ev.raw, data, size < sizeof(ev.raw) ? size : sizeof(ev.raw));
        break;
    }
}

// Reports one call. Returns whether a callback suppressed it.
bool report(IDirect3DQuery9* query, QueryAction action, const void* data, DWORD size, bool ready) {
    if (hub_idle() || internal() || !query) return false;
    const QueryInfo info = query_info(query);
    if (!info.device) return false;
    HubReader reader;
    DevState* st = dev_state(info.device);
    if (!st) return false;
    bool wants = false;
    for (D3D9ContextImpl* ctx : st->contexts) wants |= ctx->wants_query();
    if (!wants) return false;
    QueryEvent ev{};
    ev.type     = info.type;
    ev.action   = action;
    ev.query_id = info.id;
    if (ready) decode(ev, info.raw, data, size);
    for (D3D9ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_query()) continue;
        D3D9FrameContextImpl fc(*ctx, ctx->current_frame_index(), st);
        ctx->dispatch_query(fc, ev);
    }
    return ev.intercept;
}

HRESULT STDMETHODCALLTYPE hook_create_query(IDirect3DDevice9* self, D3DQUERYTYPE type, IDirect3DQuery9** out) {
    const Healer healer{ self };
    const HRESULT hr = s_create_query.original<CreateQueryFn>(self)(self, type, out);
    // A null `out` only asks whether the type is supported.
    if (SUCCEEDED(hr) && out && *out && !hub_idle() && !internal()) (void)query_register(*out);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_issue(IDirect3DQuery9* self, DWORD flags) {
    const QueryAction action = (flags & D3DISSUE_BEGIN) ? QueryAction::Begin : QueryAction::End;
    if (report(self, action, nullptr, 0, false)) return S_OK;
    return s_issue.original<IssueFn>(self)(self, flags);
}

HRESULT STDMETHODCALLTYPE hook_get_data(IDirect3DQuery9* self, void* data, DWORD size, DWORD flags) {
    const HRESULT hr = s_get_data.original<GetDataFn>(self)(self, data, size, flags);
    // Reported after the fact: the result is already written, so there is
    // nothing for intercept to suppress.
    (void)report(self, QueryAction::GetData, data, size, hr == S_OK);
    return hr;
}

} // namespace

void install_query_hooks(IDirect3DDevice9* device) noexcept {
    s_create_query.install(device, &hook_create_query);
    register_device_site(s_create_query);
    InternalScope internal_work;
    IDirect3DQuery9* q = nullptr;
    if (SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q) {
        s_issue.install(q, &hook_issue);
        s_get_data.install(q, &hook_get_data);
        q->Release();
    }
    register_class_site(s_issue);
    register_class_site(s_get_data);
}

}  // namespace vangfx::backend::d3d9
