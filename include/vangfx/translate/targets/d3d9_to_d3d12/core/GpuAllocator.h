// GPU memory allocation, through D3D12MA.
//
// This replaces BufferHeapPool. That allocator worked, was tested, and had two
// limits that could not be lifted without rewriting it into this:
//
//   * Buffers only. Its heaps carry ALLOW_ONLY_BUFFERS precisely because it
//     does not implement D3D12's texture alignment rules (4 KB, 64 KB or 4 MB
//     depending on format and sample count), so every texture in a
//     texture-heavy 2005 game was its own committed resource.
//
//   * Fixed ceilings. 384 MB for the default heap and 64 MB for upload, chosen
//     by hand rather than measured. A capture of the garage-to-freeroam
//     transition reached 24 heaps of 16 MB -- exactly the 384 MB ceiling --
//     after which every allocation silently fell back to a committed resource
//     for the rest of the session, with nothing in the log to say so.
//
// D3D12MA handles both: one budget-aware allocator over buffers and textures,
// with defragmentation available, sized against what the adapter actually
// reports rather than against a constant.
//
//
// WHAT THIS CLASS ADDS
//
// D3D12MA is already the allocator; this is not an abstraction over it and does
// not try to hide it. What it adds is the three things MWOn12 needs and the
// library deliberately leaves to its caller:
//
//   * Deferred release. D3D12MA::Allocation::Release() frees the memory there
//     and then. A resource the GPU is still reading must outlive the frame that
//     referenced it, exactly as the pool's blocks did, so releases are queued
//     against a fence and collected when the GPU has passed it. Getting this
//     wrong produces corruption that looks like a driver bug.
//
//   * MWOn12's logging, so a budget or an allocation failure appears in
//     MWOn12-render.log beside everything else rather than nowhere.
//
//   * A name on every allocation, which is what makes the debug layer's leak
//     report readable instead of a list of <unnamed>.
//
//
// LIFETIME
//
// Init after the device, Shutdown before it. Every Allocation must be released
// before the allocator is, and the allocator before the device -- Shutdown()
// drains the pending queue unconditionally to guarantee the first of those, so
// it must not be called while the GPU still has work in flight. Call it after
// the same WaitForGPU that already guards device teardown.

#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <vector>

namespace D3D12MA {
class Allocator;
class Allocation;
}

namespace vangfx::d3d9to12 {

class GpuAllocator {
public:
    // What a resource carries around to say where it lives.
    //
    // Deliberately the same shape as BufferHeapPool::Placement was -- a small
    // trivially-copyable value with Valid() -- so the call sites that only pass
    // it through did not have to change. An invalid Alloc means the resource is
    // committed and owns its own memory, which is still a legitimate outcome
    // when the allocator is off or has refused.
    struct Alloc {
        D3D12MA::Allocation* a{ nullptr };
        [[nodiscard]] bool Valid() const noexcept { return a != nullptr; }
    };

    GpuAllocator() = default;
    ~GpuAllocator();

    GpuAllocator(const GpuAllocator&)            = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    // `adapter` is optional but wanted: with it D3D12MA can read the video
    // memory budget and stay inside it, which is the whole reason for the move.
    bool Init(ID3D12Device* device, IDXGIAdapter* adapter) noexcept;

    // Drains the pending queue and releases the allocator. Safe to call twice.
    void Shutdown() noexcept;

    [[nodiscard]] bool Valid() const noexcept { return m_alloc != nullptr; }

    // Creates a resource and its backing allocation together. Buffers and
    // textures both: D3D12MA picks the ALLOW_ONLY_* heap flag from the
    // description, which is the part BufferHeapPool could not do.
    //
    // On failure `outAlloc` comes back invalid and the caller should fall back
    // to a committed resource of its own -- an allocator that cannot serve a
    // request is not a reason to fail the draw.
    HRESULT CreateResource(const D3D12_RESOURCE_DESC& desc,
                           D3D12_HEAP_TYPE heapType,
                           D3D12_RESOURCE_STATES initState,
                           const D3D12_CLEAR_VALUE* optimizedClear,
                           const wchar_t* debugName,
                           ID3D12Resource** outRes,
                           Alloc* outAlloc) noexcept;

    // Queues an allocation for release once the GPU has passed `fence`.
    // Releasing it directly would free memory the GPU may still be reading.
    void Retire(const Alloc& a, std::uint64_t fence) noexcept;

    // Releases everything whose fence the GPU has passed. Call once a frame,
    // beside the existing garbage collection.
    void Collect(std::uint64_t completedFence) noexcept;

    // Lets D3D12MA age its internal statistics. Once a frame.
    void SetFrameIndex(std::uint32_t frame) noexcept;

    // ── Observability ───────────────────────────────────────────────────────
    //
    // The old pool counted rejections and never logged them, which is why its
    // saturation went unnoticed for so long. These exist so that cannot repeat.

    // Bytes D3D12MA has allocated from the driver, and bytes actually in use.
    // `outBlockCount` is the number of D3D12 heaps and committed resources
    // behind those allocations -- the figure the old pool logged as "grew to N
    // heap(s)", and the one that says whether suballocation is working: many
    // allocations over few blocks is the whole point.
    void Stats(std::uint64_t* outAllocatedBytes,
               std::uint64_t* outUsedBytes,
               std::uint32_t* outAllocationCount,
               std::uint32_t* outBlockCount = nullptr) const noexcept;

    // The adapter's own view: budget, current usage, and what is left.
    // Logs at WARN when usage is over the budget, because that is the state
    // Microsoft documents as causing the process to be intermittently frozen.
    void LogBudget(const char* why) const noexcept;

    [[nodiscard]] std::uint64_t FailedAllocations() const noexcept { return m_failed; }

private:
    struct Pending {
        D3D12MA::Allocation* a{ nullptr };
        std::uint64_t        fence{ 0 };
    };

    D3D12MA::Allocator*  m_alloc{ nullptr };
    std::vector<Pending> m_pending;
    std::uint64_t        m_failed{ 0 };
};

}  // namespace mwon12
