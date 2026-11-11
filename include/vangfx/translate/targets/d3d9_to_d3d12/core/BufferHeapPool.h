// RETIRED — not in the build.
//
// Replaced by GpuAllocator (D3D12MA) on 2026-09-09. Kept on disk rather than
// deleted only because this tree is not under version control; nothing
// includes it, CMakeLists.txt no longer compiles it, and it can be removed as
// soon as somebody is confident they will not want to read it again.
//
// Why it was replaced, in short: buffers only (its heaps carry
// ALLOW_ONLY_BUFFERS, so every texture stayed committed), and hand-picked
// ceilings of 384 MB and 64 MB that a world load reached exactly, after which
// allocation silently reverted to committed resources with nothing logged.
// See GpuAllocator.h.
//
// ─────────────────────────────────────────────────────────────────────────────

// Suballocates D3D12 buffers out of pooled heaps.
//
// Every buffer used to be a CreateCommittedResource, which gives each one its
// own implicit heap. That is correct and it is what the D3D12 documentation
// reaches for first, but it costs three things that matter to this project:
//
//   A kernel allocation per buffer. CreateCommittedResource goes through the
//   video memory manager every time. A D3D9 game that creates and destroys
//   vertex buffers while streaming pays that repeatedly, on the thread that is
//   trying to draw.
//
//   Address space. speed.exe is 32-bit, so the whole process has 2 GB of user
//   address space and every resident allocation takes a slice of it. This is
//   the constraint that actually bites: the upload ring already carries a
//   comment about ~800 mapped buffers exhausting the process outright.
//
//   Allocation count. Thousands of small separate allocations are worse for
//   the OS to manage than a handful of large ones, independently of their
//   total size.
//
// So buffers are placed into pooled heaps instead, and the pool hands out
// 64 KB-aligned runs of blocks.
//
//
// WHAT THIS DOES NOT DO
//
// It does not save bytes. A placed buffer must be aligned to
// D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT (64 KB), exactly as a committed
// one is, so a 2 KB vertex buffer still occupies 64 KB either way. Anyone
// expecting a memory *reduction* from this will not find one; what it reduces
// is the number of allocations, the cost of making them, and the pressure on a
// 32-bit address space.
//
// It does not handle textures. Those need differently-flagged heaps and have
// alignment rules of their own (4 KB, 64 KB or 4 MB depending on the format and
// sample count), and getting them wrong is a silent corruption rather than an
// error. Buffers are where the churn is; textures stay committed.
//
// It does not defragment. Blocks are fixed-size and a run is contiguous, so a
// pool can refuse a large request while holding enough scattered free blocks to
// satisfy it. That is the classic external fragmentation case and the answer
// here is the same one the rest of the renderer uses: fall back to a committed
// resource and carry on. Moving a live resource is not possible in D3D12
// without recreating it, and recreating one the GPU may be reading is not
// something a translation layer can do behind the game's back.
//
//
// LIFETIME
//
// A placement is freed with the fence value the frame that last used it will
// signal, and only reclaimed once the GPU has passed it -- the same rule the
// retired-resource list follows, and for the same reason: releasing the
// ID3D12Resource does not tell the GPU to stop reading the memory underneath.

#pragma once

#ifndef VANGFX_D3D9TO12_DX12_BUFFERHEAPPOOL_H
#define VANGFX_D3D9TO12_DX12_BUFFERHEAPPOOL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace vangfx::d3d9to12 {

using Microsoft::WRL::ComPtr;

class BufferHeapPool {
public:
    // Where a buffer ended up. An invalid Placement means "not pooled" -- the
    // caller made a committed resource instead, and must not call Free.
    struct Placement {
        ID3D12Heap* heap{ nullptr };
        UINT64      offset{ 0 };
        uint32_t    heapIndex{ UINT32_MAX };
        uint32_t    firstBlock{ 0 };
        uint32_t    blockCount{ 0 };

        [[nodiscard]] bool Valid() const noexcept { return heap != nullptr; }
    };

    // `heapBytes` is the size of each pooled heap, `maxTotalBytes` the ceiling
    // on all of them together. Past that ceiling Allocate returns invalid and
    // the caller falls back to a committed resource, so the pool can never be
    // the reason the process runs out of memory.
    bool Init(ID3D12Device* dev, D3D12_HEAP_TYPE type,
              UINT64 heapBytes, UINT64 maxTotalBytes) noexcept;
    void Shutdown() noexcept;

    // Invalid when the request is too large to pool, the ceiling is reached, or
    // no heap has a contiguous run. All three are ordinary and non-fatal.
    [[nodiscard]] Placement Allocate(UINT64 bytes) noexcept;

    // Queues a placement for reuse once the GPU passes `fence`.
    void Free(const Placement& p, UINT64 fence) noexcept;

    // Returns everything whose fence the GPU has passed. Call once a frame.
    void Collect(UINT64 completedFence) noexcept;

    [[nodiscard]] UINT64 HeapBytes()      const noexcept { return m_heapBytes; }
    [[nodiscard]] UINT64 CommittedBytes() const noexcept
    { return UINT64(m_heaps.size()) * m_heapBytes; }
    [[nodiscard]] UINT64 UsedBytes()      const noexcept
    { return UINT64(m_usedBlocks) * kBlockBytes; }
    [[nodiscard]] size_t HeapCount()      const noexcept { return m_heaps.size(); }
    [[nodiscard]] UINT64 PooledCount()    const noexcept { return m_pooled; }
    [[nodiscard]] UINT64 RejectedCount()  const noexcept { return m_rejected; }
    [[nodiscard]] bool   Valid()          const noexcept { return m_device != nullptr; }

    // The placement alignment every buffer is subject to, pooled or not.
    static constexpr UINT64 kBlockBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

private:
    struct Heap {
        ComPtr<ID3D12Heap>    heap;
        std::vector<uint64_t> bits;      // 1 = block in use
        uint32_t              blocks{ 0 };
        uint32_t              freeBlocks{ 0 };
    };

    struct PendingFree {
        Placement p;
        UINT64    fence{ 0 };
    };

    [[nodiscard]] bool AddHeap() noexcept;
    [[nodiscard]] bool FindRun(Heap& h, uint32_t need, uint32_t* outFirst) const noexcept;
    static void        SetRun(Heap& h, uint32_t first, uint32_t count, bool used) noexcept;

    ID3D12Device*            m_device{ nullptr };
    D3D12_HEAP_TYPE          m_type{ D3D12_HEAP_TYPE_DEFAULT };
    UINT64                   m_heapBytes{ 0 };
    UINT64                   m_maxTotalBytes{ 0 };
    uint32_t                 m_blocksPerHeap{ 0 };
    uint32_t                 m_usedBlocks{ 0 };
    UINT64                   m_pooled{ 0 };
    UINT64                   m_rejected{ 0 };
    std::vector<Heap>        m_heaps;
    std::vector<PendingFree> m_pending;
};

}

#endif
