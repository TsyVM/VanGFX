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

// Implementation of BufferHeapPool. See the header for what it is for and,
// more importantly, what it deliberately is not.

#include <vangfx/translate/targets/d3d9_to_d3d12/core/BufferHeapPool.h>
#include <vangfx/translate/d3d9/Log.h>

#include <algorithm>

namespace vangfx::d3d9to12 {

namespace {

constexpr uint32_t kBitsPerWord = 64;

inline uint32_t WordsFor(uint32_t blocks) noexcept
{
    return (blocks + kBitsPerWord - 1) / kBitsPerWord;
}

inline bool TestBit(const std::vector<uint64_t>& bits, uint32_t i) noexcept
{
    return (bits[i / kBitsPerWord] >> (i % kBitsPerWord)) & 1ull;
}

}  // namespace

bool BufferHeapPool::Init(ID3D12Device* dev, D3D12_HEAP_TYPE type,
                          UINT64 heapBytes, UINT64 maxTotalBytes) noexcept
{
    if (!dev || heapBytes < kBlockBytes) return false;

    m_device        = dev;
    m_type          = type;
    m_heapBytes     = (heapBytes / kBlockBytes) * kBlockBytes;   // whole blocks
    m_maxTotalBytes = maxTotalBytes;
    m_blocksPerHeap = static_cast<uint32_t>(m_heapBytes / kBlockBytes);
    m_usedBlocks    = 0;
    m_pooled        = 0;
    m_rejected      = 0;
    m_heaps.clear();
    m_pending.clear();
    return m_blocksPerHeap > 0;
}

void BufferHeapPool::Shutdown() noexcept
{
    // The heaps themselves go here. Every placed resource that was living in
    // them must already have been released -- a heap outliving its resources is
    // fine, a resource outliving its heap is not, and the caller's teardown
    // order is what guarantees it.
    m_pending.clear();
    m_heaps.clear();
    m_device     = nullptr;
    m_usedBlocks = 0;
}

bool BufferHeapPool::AddHeap() noexcept
{
    if (CommittedBytes() + m_heapBytes > m_maxTotalBytes)
        return false;

    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes                     = m_heapBytes;
    hd.Properties                      = D3D12_HEAP_PROPERTIES{};
    hd.Properties.Type                 = m_type;
    hd.Properties.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hd.Alignment                       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    // Buffers only. A heap flagged this way cannot hold a texture, which is
    // exactly the guarantee wanted: nothing can accidentally place one here
    // and hit the alignment rules this allocator does not implement.
    hd.Flags                           = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;

    Heap h;
    if (FAILED(m_device->CreateHeap(&hd, IID_PPV_ARGS(h.heap.GetAddressOf())))) {
        // Not fatal and not even unusual under memory pressure. The caller
        // falls back to a committed resource.
        return false;
    }

    h.blocks     = m_blocksPerHeap;
    h.freeBlocks = m_blocksPerHeap;
    h.bits.assign(WordsFor(m_blocksPerHeap), 0ull);

    h.heap->SetName(m_type == D3D12_HEAP_TYPE_UPLOAD
                        ? L"MWOn12 Buffer Pool (upload)"
                        : L"MWOn12 Buffer Pool (default)");

    m_heaps.push_back(std::move(h));
    DXLOG_INFO("[dx12] buffer pool (%s) grew to %llu heap(s), %llu MB committed",
               m_type == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "default",
               (unsigned long long)m_heaps.size(),
               (unsigned long long)(CommittedBytes() / (1024 * 1024)));
    return true;
}

// First fit. The scan is over a bitmap rather than a free list because a run
// has to be contiguous and the bitmap answers that directly; heaps are small
// enough (a few hundred blocks) that a word-at-a-time walk is cheaper than
// maintaining a sorted structure.
bool BufferHeapPool::FindRun(Heap& h, uint32_t need, uint32_t* outFirst) const noexcept
{
    if (h.freeBlocks < need) return false;

    uint32_t run = 0;
    for (uint32_t i = 0; i < h.blocks; ++i) {
        // Skip a fully-used word in one step. Most of a busy heap is used, and
        // this is what keeps the walk from being per-block over all of it.
        if ((i % kBitsPerWord) == 0 && h.bits[i / kBitsPerWord] == ~0ull) {
            i += kBitsPerWord - 1;
            run = 0;
            continue;
        }
        if (TestBit(h.bits, i)) { run = 0; continue; }
        if (++run == need) {
            *outFirst = i - need + 1;
            return true;
        }
    }
    return false;
}

void BufferHeapPool::SetRun(Heap& h, uint32_t first, uint32_t count, bool used) noexcept
{
    for (uint32_t i = first; i < first + count; ++i) {
        uint64_t& word = h.bits[i / kBitsPerWord];
        const uint64_t bit = 1ull << (i % kBitsPerWord);
        if (used) word |= bit;
        else      word &= ~bit;
    }
    // Spelled out rather than added as a signed delta: freeBlocks is unsigned,
    // and relying on the wrap to subtract is the kind of thing that is correct
    // until someone changes the type.
    if (used) h.freeBlocks -= count;
    else      h.freeBlocks += count;
}

BufferHeapPool::Placement BufferHeapPool::Allocate(UINT64 bytes) noexcept
{
    Placement out;
    if (!m_device || bytes == 0) return out;

    const UINT64 need64 = (bytes + kBlockBytes - 1) / kBlockBytes;

    // Anything that would take a whole heap to itself is better off committed:
    // pooling it buys nothing and it would fragment the pool for everything
    // else. The threshold is half a heap rather than a whole one so a single
    // large buffer cannot leave a heap permanently half-usable.
    if (need64 * 2 > UINT64(m_blocksPerHeap)) {
        ++m_rejected;
        return out;
    }
    const uint32_t need = static_cast<uint32_t>(need64);

    for (size_t hi = 0; hi < m_heaps.size(); ++hi) {
        uint32_t first = 0;
        if (!FindRun(m_heaps[hi], need, &first)) continue;

        SetRun(m_heaps[hi], first, need, true);
        m_usedBlocks += need;
        ++m_pooled;

        out.heap       = m_heaps[hi].heap.Get();
        out.offset     = UINT64(first) * kBlockBytes;
        out.heapIndex  = static_cast<uint32_t>(hi);
        out.firstBlock = first;
        out.blockCount = need;
        return out;
    }

    if (!AddHeap()) {
        ++m_rejected;
        return out;
    }

    // The heap is empty, so the run is at the start.
    Heap& h = m_heaps.back();
    SetRun(h, 0, need, true);
    m_usedBlocks += need;
    ++m_pooled;

    out.heap       = h.heap.Get();
    out.offset     = 0;
    out.heapIndex  = static_cast<uint32_t>(m_heaps.size() - 1);
    out.firstBlock = 0;
    out.blockCount = need;
    return out;
}

void BufferHeapPool::Free(const Placement& p, UINT64 fence) noexcept
{
    if (!p.Valid() || p.heapIndex >= m_heaps.size()) return;
    m_pending.push_back(PendingFree{ p, fence });
}

void BufferHeapPool::Collect(UINT64 completedFence) noexcept
{
    if (m_pending.empty()) return;

    // Partitioned rather than erased one at a time: a loading burst can retire
    // hundreds of buffers in a frame and erase-from-the-middle on each would be
    // quadratic.
    auto stillBusy = std::partition(
        m_pending.begin(), m_pending.end(),
        [completedFence](const PendingFree& f) { return f.fence > completedFence; });

    for (auto it = stillBusy; it != m_pending.end(); ++it) {
        if (it->p.heapIndex >= m_heaps.size()) continue;
        SetRun(m_heaps[it->p.heapIndex], it->p.firstBlock, it->p.blockCount, false);
        m_usedBlocks -= it->p.blockCount;
    }
    m_pending.erase(stillBusy, m_pending.end());
}

}  // namespace mwon12
