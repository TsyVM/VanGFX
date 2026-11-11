// Owns the D3D12 device, the command queue, and the machinery a D3D12 renderer
// needs that D3D11 provided for free.
//
// Frames are pipelined: several are in flight at once, each with its own
// command allocator and its own slice of upload memory, and a fence tracks
// which have completed. Reusing either before the GPU has finished with it
// corrupts work already recorded, so every per-frame resource is indexed by
// the frame in flight rather than shared.
//
// Descriptor heaps are managed in two parts, which is the reason for the two
// classes below.
//
//   CpuDescriptorHeap is staging. Descriptors are created here when a resource
//   is created and live as long as it does. The heap is not shader-visible.
//
//   GpuDescriptorArena is what the shader sees. Descriptors are copied into it
//   in the layout a draw needs, in blocks, and it is refilled per frame.
//
// Copying a block per draw is too slow, so blocks are cached. That cache is
// keyed on the descriptor handles it was built from, plus a per-slot
// generation counter -- because a freed staging slot is reissued immediately,
// so handles alone are not unique over time and a new texture would otherwise
// match a cached block describing a destroyed one.
//
// The debug layer's messages are pumped into the log rather than left for a
// debugger. Without that, validation output only reaches an attached debugger,
// which makes any failure during startup undiagnosable in a normal run.

#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include <vangfx/translate/targets/d3d9_to_d3d12/core/GpuAllocator.h>
#include <vangfx/translate/targets/d3d9_to_d3d12/core/Dx12Common.h>

namespace vangfx::d3d9to12 {

using Microsoft::WRL::ComPtr;

class Resource12;

static constexpr UINT k_dx12FrameCount = 3;

class CpuDescriptorHeap {
public:
    bool Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity) noexcept;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Alloc() noexcept;
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE h) noexcept;

    [[nodiscard]] uint32_t GenerationOf(D3D12_CPU_DESCRIPTOR_HANDLE h) const noexcept;

    [[nodiscard]] bool  Valid() const noexcept { return m_heap != nullptr; }
    [[nodiscard]] UINT  Stride() const noexcept { return m_stride; }

private:
    ComPtr<ID3D12DescriptorHeap>             m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE              m_base{};
    UINT                                     m_stride{ 0 };
    UINT                                     m_capacity{ 0 };
    UINT                                     m_used{ 0 };
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_free;
    std::vector<uint32_t>                    m_generation;
};

class GpuDescriptorArena {
public:
    bool Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type,
              UINT blockSize, UINT blockCount) noexcept;

    [[nodiscard]] ID3D12DescriptorHeap* Heap() const noexcept { return m_heap.Get(); }
    [[nodiscard]] bool Valid() const noexcept { return m_heap != nullptr; }
    [[nodiscard]] UINT Stride() const noexcept { return m_stride; }

    // `useFence` is the fence value the frame being recorded will signal;
    // `completedFence` is what the GPU has already passed. A block whose last
    // use is behind `completedFence` can be taken over.
    bool GetOrCreate(ID3D12Device* dev, uint64_t key,
                     const D3D12_CPU_DESCRIPTOR_HANDLE* srcHandles,
                     D3D12_GPU_DESCRIPTOR_HANDLE* outGpu,
                     UINT64 completedFence, UINT64 useFence) noexcept;

    // Re-stamps a table the caller is about to bind again without rebuilding
    // it, so the recycler does not take it while this frame is still using it.
    // Cheap: one hash lookup and one store.
    //
    // Returns false when the key is no longer resident -- the block was
    // recycled to somebody else, and the GPU handle the caller was about to
    // reuse now points at a different table. The caller MUST re-resolve in
    // that case; binding it anyway samples another draw's textures.
    [[nodiscard]] bool Touch(uint64_t key, UINT64 useFence) noexcept;

    // Drops every cached table. Only for teardown and swap-chain resize --
    // ordinary exhaustion is handled by recycling, not by resetting.
    void Reset() noexcept;

    [[nodiscard]] UINT Capacity()  const noexcept { return m_blockCount; }
    [[nodiscard]] UINT Live()      const noexcept
    { return static_cast<UINT>(m_cache.size()); }
    [[nodiscard]] UINT64 Recycles() const noexcept { return m_recycles; }
    [[nodiscard]] UINT64 Stalls()   const noexcept { return m_stalls; }

private:
    // One descriptor block, and when the GPU last needed it.
    //
    // The arena used to be append-only: blocks were handed out until the heap
    // ran out, and recovering meant a full WaitForGPU and throwing the whole
    // cache away -- a visible hitch, and before that, draws dropped outright
    // when it filled mid-frame. Recording the fence a block was last bound
    // under turns it into a ring: the oldest block whose work the GPU has
    // finished is reused, the cache entry that named it is dropped, and
    // nothing waits.
    struct Block {
        uint64_t key{ 0 };        // cache key currently occupying it
        UINT64   fence{ 0 };      // last frame fence this was bound under
        bool     live{ false };
    };

    [[nodiscard]] bool AcquireBlock(UINT64 completedFence, UINT* outIndex) noexcept;

    ComPtr<ID3D12DescriptorHeap>                     m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE                      m_cpuBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE                      m_gpuBase{};
    D3D12_DESCRIPTOR_HEAP_TYPE                       m_type{};
    UINT                                             m_stride{ 0 };
    UINT                                             m_blockSize{ 0 };
    UINT                                             m_blockCount{ 0 };
    UINT                                             m_used{ 0 };   // high-water of first-fill
    UINT                                             m_next{ 0 };   // round-robin recycle cursor
    UINT64                                           m_recycles{ 0 };
    UINT64                                           m_stalls{ 0 };
    std::vector<Block>                               m_blocks;
    std::unordered_map<uint64_t, UINT>               m_cache;  // key -> block index
};

// A frame that overruns its slice of the ring grows into extra chunks rather
// than taking a committed buffer per allocation. The chunks are kept and
// reused on every later visit to that frame index, so a streaming burst pays
// for its peak once instead of allocating thousands of buffers -- which in a
// 32-bit process is not a performance question but an address-space one: a
// mapped 2 MB buffer per allocation exhausts the 2 GB user range in under a
// thousand allocations, and every allocation after that fails.
class UploadRing {
public:
    bool Init(ID3D12Device* dev, UINT bytesPerFrame, UINT64 growthCap) noexcept;
    void Shutdown() noexcept;

    struct Allocation {
        uint8_t*                  cpu{ nullptr };
        D3D12_GPU_VIRTUAL_ADDRESS gpu{ 0 };
        ID3D12Resource*           resource{ nullptr };
        UINT64                    offset{ 0 };
        [[nodiscard]] bool Valid() const noexcept { return cpu != nullptr; }
    };

    // `completedFence` lets a growth chunk last used by a different frame slot
    // be taken over once the GPU has finished with it, instead of every slot
    // growing its own copy of the peak.
    [[nodiscard]] Allocation Alloc(UINT frameIndex, UINT64 bytes, UINT64 align,
                                   UINT64 completedFence, UINT64 useFence) noexcept;

    void ResetFrame(UINT frameIndex) noexcept;

    [[nodiscard]] UINT64 BytesPerFrame()  const noexcept { return m_perFrame; }
    [[nodiscard]] UINT64 GrowthCap()      const noexcept { return m_growthCap; }
    [[nodiscard]] UINT64 GrownBytes()     const noexcept { return m_grownBytes; }
    [[nodiscard]] UINT   GrownChunks()    const noexcept { return m_grownChunks; }
    [[nodiscard]] UINT64 PeakFrameBytes() const noexcept { return m_peakFrameBytes; }
    [[nodiscard]] bool   GrowthCapHit()   const noexcept { return m_growthCapHit; }

private:
    struct Chunk {
        ComPtr<ID3D12Resource>    buffer;
        uint8_t*                  cpu{ nullptr };
        D3D12_GPU_VIRTUAL_ADDRESS gpu{ 0 };
        UINT64                    size{ 0 };
        UINT64                    cursor{ 0 };
        UINT                      owner{ UINT(-1) };  // frame slot currently using it
        UINT64                    fence{ 0 };         // GPU must pass this to reuse
    };

    [[nodiscard]] static Allocation Carve(Chunk& c, UINT64 bytes, UINT64 align) noexcept;
    [[nodiscard]] bool CreateChunk(UINT64 bytes, Chunk& out) noexcept;
    void NotePeak(UINT frameIndex) noexcept;

    // Allocation is locked unconditionally. The cursor bump was already racy
    // when a game created the device D3DCREATE_MULTITHREADED -- DeviceContext12
    // has a LockContext(), but nothing has ever called it -- and a race there
    // only produced overlapping allocations. Growth chunks live in a vector,
    // and a torn push_back hands out a wild GPU address, which the driver
    // answers by removing the device. Uncontended SRW acquisition is a few
    // nanoseconds against several thousand allocations a frame.
    SRWLOCK                   m_lock = SRWLOCK_INIT;
    ID3D12Device*             m_device{ nullptr };
    ComPtr<ID3D12Resource>    m_buffer;
    uint8_t*                  m_cpuPtr{ nullptr };
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase{ 0 };
    UINT64                    m_perFrame{ 0 };
    UINT64                    m_cursor[k_dx12FrameCount]{};
    // One shared pool, not one list per frame slot. Per-slot lists meant the
    // same peak was paid for up to three times over: a field log grew 248 MB
    // of chunks to serve a 110 MB frame.
    std::vector<Chunk>        m_overflow;
    UINT64                    m_growthCap{ 0 };
    UINT64                    m_grownBytes{ 0 };
    UINT                      m_grownChunks{ 0 };
    UINT64                    m_peakFrameBytes{ 0 };
    bool                      m_growthCapHit{ false };
};

enum RootParam12 : UINT {
    kRP_VSConst = 0,
    kRP_PSConst = 1,
    kRP_VSEmu   = 2,
    kRP_PSEmu   = 3,
    kRP_SRVs    = 4,
    kRP_Samplers= 5,
    kRP_Count   = 6
};

inline constexpr UINT kMaxTextures12 = 16;
inline constexpr UINT kMaxSamplers12 = 16;

class DeviceContext12 {
public:
    static HRESULT Create(HWND hwnd, const D3DPRESENT_PARAMETERS& pp,
                          bool multiThreaded, UINT adapterOrdinal,
                          DeviceContext12** ppOut) noexcept;

    // WARP path for tests: uses IDXGIFactory4::EnumWarpAdapter instead of a
    // hardware adapter ordinal. Defined in DeviceContext12_Warp.cpp.
    static HRESULT CreateWarp(HWND hwnd, const D3DPRESENT_PARAMETERS& pp,
                              bool multiThreaded,
                              DeviceContext12** ppOut) noexcept;

    void Destroy() noexcept;
    static DeviceContext12* Current() noexcept;

    [[nodiscard]] ID3D12Device*              Device()   const noexcept { return m_device.Get(); }
    [[nodiscard]] ID3D12CommandQueue*        CmdQueue() const noexcept { return m_cmdQueue.Get(); }
    [[nodiscard]] IDXGISwapChain4*           SwapChain()const noexcept { return m_swapChain.Get(); }
    [[nodiscard]] IDXGIFactory4*             Factory()  const noexcept { return m_factory.Get(); }
    [[nodiscard]] ID3D12GraphicsCommandList* CmdList()  const noexcept { return m_cmdList.Get(); }
    [[nodiscard]] ID3D12RootSignature*       RootSignature() const noexcept { return m_rootSig.Get(); }
    [[nodiscard]] HWND                       TargetHwnd() const noexcept { return m_targetHwnd; }

    HRESULT PresentFrame(UINT syncInterval) noexcept;

    // Blocks until the swap chain is ready for the next frame, when the
    // frame-latency waitable object is in use (MWOn12.ini LowLatency=1).
    // Does nothing otherwise. Call at the top of the frame, before the game
    // has sampled its input.
    void WaitForFrameLatency() noexcept;

    [[nodiscard]] bool TearingSupported() const noexcept { return m_tearingSupported; }

    // ── Buffer allocation ───────────────────────────────────────────────────
    //
    // Creates a buffer, out of a pooled heap when one can take it and as a
    // committed resource when not. `outPlacement` comes back invalid in the
    // committed case; pass whatever comes out to RetireBuffer either way and
    // it does the right thing.
    //
    // Textures come through here too, which is the point of the move to
    // D3D12MA: it picks the ALLOW_ONLY_* heap flag from the description, so a
    // texture no longer has to be a committed resource.
    HRESULT CreateResource(const D3D12_RESOURCE_DESC& desc,
                           D3D12_HEAP_TYPE heapType,
                           D3D12_RESOURCE_STATES initState,
                           const D3D12_CLEAR_VALUE* optimizedClear,
                           const wchar_t* debugName,
                           ID3D12Resource** outRes,
                           GpuAllocator::Alloc* outAlloc) noexcept;

    // Releases the resource and returns its pool blocks, both deferred until
    // the GPU has finished with the frame that last used them.
    // The heap type is no longer a parameter: the allocation knows which heap
    // it came from, which is one fewer thing for a caller to get wrong.
    void RetireResource(ID3D12Resource* res,
                        const GpuAllocator::Alloc& alloc) noexcept;

    [[nodiscard]] const GpuAllocator& Allocator() const noexcept
    { return m_gpuAlloc; }

    HRESULT FlushAndWait() noexcept;

    void WaitForGPU() noexcept;

    [[nodiscard]] UINT   FrameIndex() const noexcept { return m_frameIndex; }
    [[nodiscard]] UINT64 CurrentFence() const noexcept { return m_globalFenceValue; }

    [[nodiscard]] UINT64 CompletedFence() const noexcept
    {
        return m_fence ? m_fence->GetCompletedValue() : 0;
    }

    // The value the frame currently being recorded will signal once submitted.
    // Anything stamped with this is in flight until the GPU passes it.
    [[nodiscard]] UINT64 PendingFence() const noexcept
    {
        return m_globalFenceValue + 1;
    }

    [[nodiscard]] UINT64 FrameCounter() const noexcept { return m_frameCounter; }

    [[nodiscard]] bool CommandStateDirty() const noexcept { return m_cmdStateDirty; }
    void ClearCommandStateDirty() noexcept { m_cmdStateDirty = false; }
    void MarkCommandStateDirty() noexcept { m_cmdStateDirty = true; }

    [[nodiscard]] UINT            BackBufferCount()  const noexcept { return k_dx12FrameCount; }
    [[nodiscard]] UINT            BackBufferIndex()  const noexcept { return m_backBufferIndex; }
    [[nodiscard]] ID3D12Resource* BackBuffer(UINT i) const noexcept
    {
        return i < k_dx12FrameCount ? m_backBuffers[i].Get() : nullptr;
    }
    [[nodiscard]] DXGI_FORMAT BackBufferFormat() const noexcept { return m_bbFormat; }
    [[nodiscard]] UINT        BackBufferWidth()  const noexcept { return m_bbWidth; }
    [[nodiscard]] UINT        BackBufferHeight() const noexcept { return m_bbHeight; }
    [[nodiscard]] D3DFORMAT   BackBufferD3D9Format() const noexcept { return m_bbD3D9Format; }

    [[nodiscard]] D3D12_RESOURCE_STATES BackBufferState(UINT i) const noexcept
    {
        return i < k_dx12FrameCount ? m_bbState[i] : D3D12_RESOURCE_STATE_COMMON;
    }
    void SetBackBufferState(UINT i, D3D12_RESOURCE_STATES s) noexcept
    {
        if (i < k_dx12FrameCount) m_bbState[i] = s;
    }

    [[nodiscard]] CpuDescriptorHeap&   RtvHeap()      noexcept { return m_rtvHeap; }
    [[nodiscard]] CpuDescriptorHeap&   DsvHeap()      noexcept { return m_dsvHeap; }

    [[nodiscard]] CpuDescriptorHeap&   SrvStaging()   noexcept { return m_srvStaging; }

    [[nodiscard]] CpuDescriptorHeap&   SamplerStaging() noexcept { return m_sampStaging; }
    [[nodiscard]] GpuDescriptorArena&  SrvArena()     noexcept { return m_srvArena; }
    [[nodiscard]] GpuDescriptorArena&  SamplerArena() noexcept { return m_sampArena; }

    void BindDescriptorHeaps() noexcept;

    [[nodiscard]] UploadRing& Upload() noexcept { return m_upload; }

    [[nodiscard]] UploadRing::Allocation AllocUpload(UINT64 bytes, UINT64 align) noexcept;

    // Video-memory budget and 32-bit process address space, written to the log
    // alongside any allocation failure. A resource creation that fails on a
    // player's machine and not on ours is nearly always one of these two, and
    // without them the report is just an HRESULT.
    // Resource-state journal.
    //
    // AppendTransition advances a resource's tracked state as soon as the
    // barrier is *recorded*, which is only correct if the list is then
    // submitted. Drop the list and the tracker believes in transitions the GPU
    // never performed, so every later barrier carries a wrong StateBefore --
    // which D3D12 rejects at ExecuteCommandLists, and that removes the device.
    // Recording what each barrier changed lets a dropped list be undone.
    void JournalState(Resource12* res, UINT sub,
                      D3D12_RESOURCE_STATES before) noexcept;
    void JournalBackBufferState(UINT index, D3D12_RESOURCE_STATES before) noexcept;
    void ForgetInStateJournal(const Resource12* res) noexcept;

    void ReportMemoryState(const char* why) noexcept;

    void ReportDredOutput(HRESULT reason) noexcept;

    void Retire(ID3D12Resource* res) noexcept;
    void Retire(ID3D12PipelineState* pso) noexcept;
    void Retire(ID3D12GraphicsCommandList* list) noexcept;

    void CollectGarbage() noexcept;

    HRESULT ResizeSwapChain(UINT w, UINT h, const D3DPRESENT_PARAMETERS& pp) noexcept;
    void    ApplyWindowMode(const D3DPRESENT_PARAMETERS& pp) noexcept;
    [[nodiscard]] DXGI_FORMAT ResolveBackBufferFormat(const D3DPRESENT_PARAMETERS& pp) const noexcept;

    void LockContext()   noexcept;
    void UnlockContext() noexcept;

    [[nodiscard]] bool IsDeviceLost() const noexcept { return m_deviceLost; }

    // Feed any failing HRESULT through here before logging it. Once the device
    // is gone every call fails the same way, and the caller that reports it
    // loudest is rarely the one that caused it -- one field log carried sixty
    // identical PSO-creation errors above the removal that produced them.
    // Returns true when the device is lost, meaning the caller should give up
    // quietly rather than log.
    bool NoteDeviceRemoved(HRESULT hr, const char* where) noexcept;
    void NotePresentResult(HRESULT hr) noexcept;

    void ReportDeviceRemoved(const char* where) noexcept;

    void DrainInfoQueue() noexcept;

    ~DeviceContext12();

private:
    DeviceContext12() = default;

    HRESULT InitD3D12Device(IDXGIAdapter1* adapter) noexcept;
    HRESULT InitCommandInfra() noexcept;
    HRESULT InitSwapChain(HWND hwnd, const D3DPRESENT_PARAMETERS& pp) noexcept;
    HRESULT InitDescriptorHeaps() noexcept;
    HRESULT InitUploadRing() noexcept;

    // Close the open list and submit it, refusing to submit one that failed to
    // close. Returns whether anything was actually executed.
    bool    CloseAndSubmit(const char* where) noexcept;
    HRESULT RecreateCommandList() noexcept;
    HRESULT InitRootSignature() noexcept;
    HRESULT AcquireBackBuffers() noexcept;
    void    ReleaseBackBuffers() noexcept;
    HRESULT OpenCommandList() noexcept;

    ComPtr<IDXGIFactory4>             m_factory;
    ComPtr<IDXGIAdapter1>             m_adapter;
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12CommandQueue>        m_cmdQueue;
    ComPtr<IDXGISwapChain4>           m_swapChain;

    ComPtr<ID3D12CommandAllocator>    m_cmdAlloc[k_dx12FrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;

    ComPtr<ID3D12Fence>               m_fence;
    UINT64                            m_frameFence[k_dx12FrameCount]{};
    UINT64                            m_globalFenceValue{ 0 };

    // Last reported arena-exhaustion count, so the warning is emitted when it
    // changes rather than every frame it stays true.
    UINT64                            m_lastArenaStalls{ 0 };

    // ── Presentation ────────────────────────────────────────────────────────
    // Whichever of ALLOW_TEARING and FRAME_LATENCY_WAITABLE_OBJECT the swap
    // chain was actually created with. Kept because ResizeBuffers must be
    // given the same flags the swap chain was created with, and because the
    // tearing flag has to be repeated on every Present that wants it.
    UINT                              m_swapChainFlags{ 0 };
    bool                              m_tearingSupported{ false };
    bool                              m_frameLatencyWaitable{ false };
    HANDLE                            m_frameLatencyEvent{ nullptr };
    HANDLE                            m_fenceEvent{ nullptr };
    UINT                              m_frameIndex{ 0 };
    UINT64                            m_frameCounter{ 0 };

    ComPtr<ID3D12Resource>            m_backBuffers[k_dx12FrameCount];
    D3D12_RESOURCE_STATES             m_bbState[k_dx12FrameCount]{};
    UINT                              m_backBufferIndex{ 0 };
    DXGI_FORMAT                       m_bbFormat{ DXGI_FORMAT_B8G8R8A8_UNORM };
    D3DFORMAT                         m_bbD3D9Format{ D3DFMT_X8R8G8B8 };
    UINT                              m_bbWidth{ 0 };
    UINT                              m_bbHeight{ 0 };

    ComPtr<ID3D12RootSignature>       m_rootSig;

    CpuDescriptorHeap                 m_rtvHeap;
    CpuDescriptorHeap                 m_dsvHeap;
    CpuDescriptorHeap                 m_srvStaging;
    CpuDescriptorHeap                 m_sampStaging;
    GpuDescriptorArena                m_srvArena;
    GpuDescriptorArena                m_sampArena;

    UploadRing                        m_upload;

    struct Retired {
        ComPtr<IUnknown> object;
        UINT64           fence{ 0 };
    };

    // Buffers suballocated out of pooled heaps. Textures are not pooled -- see
    // BufferHeapPool's header for why.
    GpuAllocator                      m_gpuAlloc;
    bool                              m_gpuAllocEnabled{ false };

    // Guards m_retired, and only m_retired.
    //
    // The three Retire overloads are reached from ~Resource12, which runs
    // whenever the game releases a texture or a buffer -- on whatever thread
    // did the release, which under D3DCREATE_MULTITHREADED is not the render
    // thread. CollectGarbage pops from the render thread. A push_back racing a
    // pop_front on a std::deque corrupts its block index, and the fault lands
    // later, inside an allocator, with a stack that names neither.
    //
    // This does not make DeviceContext12 thread-safe -- see the note in
    // D9Root12::CreateDeviceInternal; the rest of it is still written for one
    // thread. It closes the one place where the game's own threading reaches
    // shared state without going through the upload ring, which locks itself.
    mutable SRWLOCK                   m_retiredLock = SRWLOCK_INIT;
    std::deque<Retired>               m_retired;

    struct StateJournalEntry {
        Resource12*           res;      // null means the back buffer named by index
        UINT                  sub;
        UINT                  bbIndex;
        D3D12_RESOURCE_STATES before;
    };
    std::vector<StateJournalEntry>    m_stateJournal;

    void RollbackStateJournal() noexcept;

    HWND    m_targetHwnd{ nullptr };
    bool    m_multiThreaded{ false };
    SRWLOCK m_ctxLock = SRWLOCK_INIT;
    bool    m_deviceLost{ false };
    bool    m_removedReported{ false };
    ComPtr<ID3D12InfoQueue> m_infoQueue;
    bool    m_dredEnabled{ false };
    bool    m_dredBreadcrumbs{ false };
    bool    m_cmdListOpen{ false };
    bool    m_cmdStateDirty{ true };
    bool    m_uploadCapReported{ false };
    bool    m_resetFailReported{ false };
    bool    m_cmdListBroken{ false };
    unsigned m_closeFailures{ 0 };

    static std::atomic<DeviceContext12*> s_current;
};

}
