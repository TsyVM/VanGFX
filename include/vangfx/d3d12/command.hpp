#pragma once

/**
 * vangfx/d3d12/command.hpp — Command list, command queue, and command allocator
 * handle types, plus the CommandListEvent fired around every command list
 * submission.
 *
 * D3D12 replaced the immediate-mode D3D11 device context with explicit
 * command recording (ID3D12GraphicsCommandList) and submission
 * (ID3D12CommandQueue::ExecuteCommandLists).
 *
 * VanGFX wraps these in opaque handles and fires CommandListEvent at:
 *   - Reset     (allocator + command list are ready to record)
 *   - Close     (command list sealed; next step is Submit)
 *   - Submit    (ExecuteCommandLists called on the queue)
 *
 * All handles are valid for the lifetime of the underlying D3D12 object and
 * become zero after destruction.
 */

#include <cstdint>

namespace vangfx::d3d12 {

// ─────────────────────────────────────────────────────────────────────────────
//  CommandListType — D3D12_COMMAND_LIST_TYPE
// ─────────────────────────────────────────────────────────────────────────────

enum class CommandListType : uint8_t {
    Direct,      // graphics + compute + copy
    Bundle,      // pre-recorded sub-list of draw calls (no resource state changes)
    Compute,     // async compute queue
    Copy,        // dedicated DMA / copy engine
    VideoDecode,
    VideoProcess,
    VideoEncode,
};

// ─────────────────────────────────────────────────────────────────────────────
//  CommandQueuePriority — D3D12_COMMAND_QUEUE_PRIORITY
// ─────────────────────────────────────────────────────────────────────────────

enum class CommandQueuePriority : int32_t {
    Normal        =      0,
    High          =    100,
    GlobalRealtime = 10000,
};

// ─────────────────────────────────────────────────────────────────────────────
//  Opaque handles
// ─────────────────────────────────────────────────────────────────────────────

struct CommandListHandle      { uint64_t id = 0; };
struct CommandQueueHandle     { uint64_t id = 0; };
struct CommandAllocatorHandle { uint64_t id = 0; };
struct FenceHandle            { uint64_t id = 0; };

[[nodiscard]] inline bool is_valid(CommandListHandle h)      noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(CommandQueueHandle h)     noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(CommandAllocatorHandle h) noexcept { return h.id != 0; }
[[nodiscard]] inline bool is_valid(FenceHandle h)            noexcept { return h.id != 0; }

// ─────────────────────────────────────────────────────────────────────────────
//  CommandListAction — what happened to trigger the CommandListEvent
// ─────────────────────────────────────────────────────────────────────────────

enum class CommandListAction : uint8_t {
    Reset,     // ID3D12GraphicsCommandList::Reset() — recording started
    Close,     // ID3D12GraphicsCommandList::Close() — recording finished
    Submit,    // ID3D12CommandQueue::ExecuteCommandLists() — GPU submission
    Signal,    // ID3D12CommandQueue::Signal() — fence signalled
    Wait,      // ID3D12CommandQueue::Wait()   — queue stalled on fence
};

// ─────────────────────────────────────────────────────────────────────────────
//  CommandQueueDesc — mirrors D3D12_COMMAND_QUEUE_DESC
// ─────────────────────────────────────────────────────────────────────────────

struct CommandQueueDesc {
    CommandListType     type      = CommandListType::Direct;
    CommandQueuePriority priority = CommandQueuePriority::Normal;
    uint32_t            flags     = 0;      // D3D12_COMMAND_QUEUE_FLAGS
    uint32_t            node_mask = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CommandListEvent — fired on Reset, Close, and Submit
//
//  On Submit, submitted_lists[] is populated with the handles of every command
//  list passed to ExecuteCommandLists.  max_submitted_lists limits the array;
//  submitted_count carries the true count even if it exceeds the array bound.
//
//  Set intercept = true to suppress the underlying D3D12 call (Reset / Close /
//  ExecuteCommandLists).  Intercepting Submit blocks GPU work — use with care.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint32_t kMaxSubmittedLists = 16;

struct CommandListEvent {
    CommandListAction    action     = CommandListAction::Reset;
    CommandListType      list_type  = CommandListType::Direct;

    CommandListHandle      command_list      = {};
    CommandAllocatorHandle command_allocator = {};
    CommandQueueHandle     command_queue     = {};   // valid on Submit/Signal/Wait

    // ── Submit-specific ───────────────────────────────────────────────────────
    CommandListHandle submitted_lists[kMaxSubmittedLists] = {};
    uint32_t          submitted_count = 0;  // may exceed kMaxSubmittedLists

    // ── Fence-specific (Signal / Wait) ────────────────────────────────────────
    FenceHandle fence       = {};
    uint64_t    fence_value = 0;

    bool intercept = false;
};

} // namespace vangfx::d3d12
