#pragma once

/**
 * vangfx/pipeline/topology.hpp — PrimitiveTopology enum.
 *
 * TriangleFan is D3D9 only.
 * Adjacency and patch-list topologies are D3D11 only.
 */

namespace vangfx {

enum class PrimitiveTopology : uint32_t {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
    TriangleFan,            // D3D9 only
    LineListAdj,            // D3D11 only
    LineStripAdj,           // D3D11 only
    TriangleListAdj,        // D3D11 only
    TriangleStripAdj,       // D3D11 only
    PatchList_1,            // D3D11 tessellation
    PatchList_2,
    PatchList_3,
    PatchList_4,
    PatchList_5,
    PatchList_6,
    PatchList_7,
    PatchList_8,
    PatchList_9,
    PatchList_10,
    PatchList_11,
    PatchList_12,
    PatchList_13,
    PatchList_14,
    PatchList_15,
    PatchList_16,
    PatchList_17,
    PatchList_18,
    PatchList_19,
    PatchList_20,
    PatchList_21,
    PatchList_22,
    PatchList_23,
    PatchList_24,
    PatchList_25,
    PatchList_26,
    PatchList_27,
    PatchList_28,
    PatchList_29,
    PatchList_30,
    PatchList_31,
    PatchList_32,
};

} // namespace vangfx
