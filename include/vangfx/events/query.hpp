#pragma once

/**
 * vangfx/events/query.hpp — GPU query / timestamp event.
 *
 * QueryEvent is fired when the game issues a GPU query (occlusion, timestamp,
 * pipeline statistics, etc.) or when vangfx resolves a query result.
 *
 * Intercept timestamp queries to correlate game-engine render phases with
 * VanGFX frame-capture timing.
 */

#include <cstdint>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  QueryType — D3D9 and D3D11 query types
// ─────────────────────────────────────────────────────────────────────────────

enum class QueryType : uint8_t {
    // Supported on both backends:
    Event,              // signals when GPU work preceding the query is done
    Occlusion,          // pixel pass count
    Timestamp,          // GPU clock tick at this point in the command stream
    TimestampDisjoint,  // validates timestamp readings (D3D11) / always fires (D3D9)

    // D3D11 only:
    PipelineStatistics, // draw, vs/ps invocations, primitive counts, etc.
    OcclusionPredicate,
    StreamOutputStatistics,
    StreamOutputOverflowPredicate,
    StreamOutputStatistics_Stream0,
    StreamOutputStatistics_Stream1,
    StreamOutputStatistics_Stream2,
    StreamOutputStatistics_Stream3,
};

// ─────────────────────────────────────────────────────────────────────────────
//  QueryAction
// ─────────────────────────────────────────────────────────────────────────────

enum class QueryAction : uint8_t {
    Begin,    // D3D query Begin() / D3D9 Issue(D3DISSUE_BEGIN)
    End,      // D3D query End()   / D3D9 Issue(D3DISSUE_END)
    GetData,  // result was retrieved by the game (value field may be valid)
};

// ─────────────────────────────────────────────────────────────────────────────
//  QueryEvent
// ─────────────────────────────────────────────────────────────────────────────

struct QueryEvent {
    QueryType   type         = QueryType::Timestamp;
    QueryAction action       = QueryAction::End;
    uint64_t    query_id     = 0;    // opaque identifier for this query object
    bool        intercept    = false;// set true to suppress the D3D call

    // ── Result data (valid only when action == GetData and result_ready) ──────
    bool        result_ready = false;
    union {
        uint64_t timestamp_ticks;    // QueryType::Timestamp
        uint64_t occlusion_samples;  // QueryType::Occlusion
        uint64_t frequency_hz;       // QueryType::TimestampDisjoint — GPU clock freq

        struct {                     // QueryType::PipelineStatistics (D3D11/D3D12)
            uint64_t vs_invocations;
            uint64_t ps_invocations;
            uint64_t primitives_rendered;
            uint64_t primitives_input;
            uint64_t c_invocations;
            uint64_t c_primitives;
            uint64_t hs_invocations;
            uint64_t ds_invocations;
            uint64_t gs_invocations;
            uint64_t gs_primitives;
            uint64_t ia_vertices;
            uint64_t ia_primitives;
            // D3D12 additional pipeline stats:
            uint64_t as_invocations; // amplification shader (mesh pipeline)
            uint64_t ms_invocations; // mesh shader (mesh pipeline)
        } pipeline_stats;

        uint8_t raw[64];             // fallback raw result bytes
    };

    // ── D3D12-only timestamp heap fields ─────────────────────────────────────
    // D3D12 uses ID3D12QueryHeap instead of ID3D12Query for timestamp queries.
    // These fields are populated for Timestamp and TimestampDisjoint types when
    // the backend is D3D12.
    uint64_t d3d12_query_heap_id = 0;  // opaque heap handle id
    uint32_t d3d12_query_index   = 0;  // index within the query heap
    // GPU virtual address of the resolve destination buffer (ResolveQueryData).
    uint64_t d3d12_resolve_dest_va = 0;
};

} // namespace vangfx
