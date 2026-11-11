/**
 * src/backend/d3d12/query.cpp — BeginQuery, EndQuery, ResolveQueryData.
 *
 * D3D12 has no query objects: a query is an index into an ID3D12QueryHeap,
 * begun and ended on a command list and resolved into a buffer. Each call is
 * reported as a QueryEvent with the heap's id and the index; ResolveQueryData is
 * reported once per query in the range, as GetData, carrying the GPU address the
 * result will land at (the value itself exists only after the GPU has run the
 * list, which is after the callback).
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <vangfx/events/query.hpp>

namespace vangfx::backend::d3d12 {

namespace lst = ::vangfx::d3d12_slots::graphics_command_list;

namespace {

using QueryFn   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT,
                                           ID3D12Resource*, UINT64);

hooks::Site s_begin  { lst::BeginQuery,       "BeginQuery" };
hooks::Site s_end    { lst::EndQuery,         "EndQuery" };
hooks::Site s_resolve{ lst::ResolveQueryData, "ResolveQueryData" };

QueryType to_type(D3D12_QUERY_TYPE t) noexcept {
    switch (t) {
    case D3D12_QUERY_TYPE_OCCLUSION:             return QueryType::Occlusion;
    case D3D12_QUERY_TYPE_BINARY_OCCLUSION:      return QueryType::OcclusionPredicate;
    case D3D12_QUERY_TYPE_TIMESTAMP:             return QueryType::Timestamp;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS1:  return QueryType::PipelineStatistics;
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0: return QueryType::StreamOutputStatistics_Stream0;
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1: return QueryType::StreamOutputStatistics_Stream1;
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2: return QueryType::StreamOutputStatistics_Stream2;
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3: return QueryType::StreamOutputStatistics_Stream3;
    default:                                     return QueryType::Event;
    }
}

// Reports one query. Returns whether a callback suppressed the call.
bool report(ID3D12GraphicsCommandList* self, ListState& st, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
            UINT index, QueryAction action, uint64_t resolve_va) {
    const uint64_t heap_id = id_of(heap);
    QueryEvent ev{};
    ev.type                  = to_type(type);
    ev.action                = action;
    ev.query_id              = (heap_id << 32) | index;
    ev.d3d12_query_heap_id   = heap_id;
    ev.d3d12_query_index     = index;
    ev.d3d12_resolve_dest_va = resolve_va;
    for (D3D12ContextImpl* ctx : st.contexts) {
        if (!ctx->wants_query()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index(), nullptr, self, &st);
        ctx->dispatch_query(fc, ev);
    }
    return ev.intercept;
}

bool anyone_wants(const ListState& st) noexcept {
    for (D3D12ContextImpl* ctx : st.contexts) if (ctx->wants_query()) return true;
    return false;
}

template <typename Call>
void query_thunk(ID3D12GraphicsCommandList* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index,
                 QueryAction action, Call&& call) {
    if (hub_idle() || internal()) { call(); return; }
    bool intercept = false;
    {
        HubReader reader;
        ListState* st = list_state(self);
        if (st && anyone_wants(*st)) intercept = report(self, *st, heap, type, index, action, 0);
    }
    if (!intercept) call();
}

void STDMETHODCALLTYPE hook_begin(ID3D12GraphicsCommandList* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    query_thunk(self, heap, type, index, QueryAction::Begin,
                [&] { s_begin.original<QueryFn>(self)(self, heap, type, index); });
}

void STDMETHODCALLTYPE hook_end(ID3D12GraphicsCommandList* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    query_thunk(self, heap, type, index, QueryAction::End,
                [&] { s_end.original<QueryFn>(self)(self, heap, type, index); });
}

void STDMETHODCALLTYPE hook_resolve(ID3D12GraphicsCommandList* self, ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                    UINT start, UINT count, ID3D12Resource* dest, UINT64 offset) {
    auto call = [&] { s_resolve.original<ResolveFn>(self)(self, heap, type, start, count, dest, offset); };
    if (hub_idle() || internal() || !dest) { call(); return; }
    bool intercept = false;
    {
        HubReader reader;
        ListState* st = list_state(self);
        if (st && anyone_wants(*st)) {
            // Each result is 8 bytes except pipeline and stream-output statistics.
            UINT64 stride = sizeof(UINT64);
            if (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS)  stride = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
            if (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1) stride = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
            if (type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 && type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3)
                stride = sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
            const uint64_t base = dest->GetGPUVirtualAddress() + offset;
            for (UINT i = 0; i < count; ++i)
                intercept |= report(self, *st, heap, type, start + i, QueryAction::GetData, base + i * stride);
        }
    }
    if (!intercept) call();
}

} // namespace

void install_query_list_hooks(ID3D12GraphicsCommandList* list) noexcept {
    s_begin.install(list, &hook_begin);
    s_end.install(list, &hook_end);
    s_resolve.install(list, &hook_resolve);
}

void uninstall_query_hooks() noexcept {
    s_begin.uninstall();
    s_end.uninstall();
    s_resolve.uninstall();
}

} // namespace vangfx::backend::d3d12
