/**
 * src/backend/d3d11/query.cpp — Begin, End, GetData on a device context.
 *
 * Each query object is reported under an id it keeps for its lifetime. GetData
 * is reported after the call, with the result decoded into the event when the
 * GPU had it ready (S_OK).
 */

#include "d3d11_impl.hpp"
#include <compat/d3d11_slots.hpp>
#include <vangfx/events/query.hpp>

#include <cstring>

namespace vangfx::backend::d3d11 {

namespace dcs = ::vangfx::d3d11_slots::device_context;
using hooks::Heal;

namespace {

using BeginEndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*);
using GetDataFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);

hooks::Site s_begin   { dcs::Begin,   "Begin",   Heal::SameModule };
hooks::Site s_end     { dcs::End,     "End",     Heal::SameModule };
hooks::Site s_get_data{ dcs::GetData, "GetData", Heal::SameModule };

QueryType type_of(ID3D11Asynchronous* async) noexcept {
    ID3D11Query* q = nullptr;
    if (FAILED(async->QueryInterface(IID_PPV_ARGS(&q))) || !q) return QueryType::Event;   // a counter
    D3D11_QUERY_DESC d{};
    q->GetDesc(&d);
    q->Release();
    switch (d.Query) {
    case D3D11_QUERY_OCCLUSION:             return QueryType::Occlusion;
    case D3D11_QUERY_TIMESTAMP:             return QueryType::Timestamp;
    case D3D11_QUERY_TIMESTAMP_DISJOINT:    return QueryType::TimestampDisjoint;
    case D3D11_QUERY_PIPELINE_STATISTICS:   return QueryType::PipelineStatistics;
    case D3D11_QUERY_OCCLUSION_PREDICATE:   return QueryType::OcclusionPredicate;
    case D3D11_QUERY_SO_STATISTICS:         return QueryType::StreamOutputStatistics;
    case D3D11_QUERY_SO_OVERFLOW_PREDICATE: return QueryType::StreamOutputOverflowPredicate;
    case D3D11_QUERY_SO_STATISTICS_STREAM0: return QueryType::StreamOutputStatistics_Stream0;
    case D3D11_QUERY_SO_STATISTICS_STREAM1: return QueryType::StreamOutputStatistics_Stream1;
    case D3D11_QUERY_SO_STATISTICS_STREAM2: return QueryType::StreamOutputStatistics_Stream2;
    case D3D11_QUERY_SO_STATISTICS_STREAM3: return QueryType::StreamOutputStatistics_Stream3;
    default:                                return QueryType::Event;
    }
}

void decode(QueryEvent& ev, const void* data, UINT size) noexcept {
    if (!data || !size) return;
    ev.result_ready = true;
    switch (ev.type) {
    case QueryType::Timestamp:
    case QueryType::Occlusion:
    case QueryType::TimestampDisjoint:   // D3D11_QUERY_DATA_TIMESTAMP_DISJOINT starts with Frequency
        if (size >= sizeof(uint64_t)) std::memcpy(&ev.timestamp_ticks, data, sizeof(uint64_t));
        break;
    case QueryType::PipelineStatistics:
        if (size >= sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS)) {
            D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
            std::memcpy(&s, data, sizeof(s));
            auto& o = ev.pipeline_stats;
            o.ia_vertices = s.IAVertices;       o.ia_primitives = s.IAPrimitives;
            o.vs_invocations = s.VSInvocations; o.gs_invocations = s.GSInvocations;
            o.gs_primitives = s.GSPrimitives;   o.c_invocations = s.CInvocations;
            o.c_primitives = s.CPrimitives;     o.ps_invocations = s.PSInvocations;
            o.hs_invocations = s.HSInvocations; o.ds_invocations = s.DSInvocations;
        }
        break;
    default:
        std::memcpy(ev.raw, data, size < sizeof(ev.raw) ? size : sizeof(ev.raw));
        break;
    }
}

// Reports one call. Returns whether a callback suppressed it.
bool report(ID3D11DeviceContext* self, ID3D11Asynchronous* async, QueryAction action,
            const void* data, UINT size, bool ready) {
    if (hub_idle() || internal() || !async) return false;
    HubReader reader;
    DcState* st = dc_state(self);
    if (!st) return false;
    bool wants = false;
    for (D3D11ContextImpl* ctx : st->contexts) wants |= ctx->wants_query();
    if (!wants) return false;
    QueryEvent ev{};
    ev.type     = type_of(async);
    ev.action   = action;
    ev.query_id = id_of(async);
    if (ready) decode(ev, data, size);
    for (D3D11ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_query()) continue;
        D3D11FrameContextImpl fc(*ctx, ctx->current_frame_index(), self, st);
        ctx->dispatch_query(fc, ev);
    }
    return ev.intercept;
}

void STDMETHODCALLTYPE hook_begin(ID3D11DeviceContext* self, ID3D11Asynchronous* async) {
    const Healer healer{ self };
    if (!report(self, async, QueryAction::Begin, nullptr, 0, false)) s_begin.original<BeginEndFn>(self)(self, async);
}

void STDMETHODCALLTYPE hook_end(ID3D11DeviceContext* self, ID3D11Asynchronous* async) {
    const Healer healer{ self };
    if (!report(self, async, QueryAction::End, nullptr, 0, false)) s_end.original<BeginEndFn>(self)(self, async);
}

HRESULT STDMETHODCALLTYPE hook_get_data(ID3D11DeviceContext* self, ID3D11Asynchronous* async, void* data, UINT size, UINT flags) {
    const Healer healer{ self };
    const HRESULT hr = s_get_data.original<GetDataFn>(self)(self, async, data, size, flags);
    // Reported after the fact: the result is already written, so there is
    // nothing for intercept to suppress.
    (void)report(self, async, QueryAction::GetData, data, size, hr == S_OK);
    return hr;
}

} // namespace

void install_query_hooks(ID3D11DeviceContext* context) noexcept {
    s_begin.install(context, &hook_begin);
    s_end.install(context, &hook_end);
    s_get_data.install(context, &hook_get_data);
    register_context_site(s_begin);
    register_context_site(s_end);
    register_context_site(s_get_data);
}

}  // namespace vangfx::backend::d3d11
