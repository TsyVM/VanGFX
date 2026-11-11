/**
 * src/backend/d3d12/capture.cpp — what a capture needs from D3D12: the hooks'
 * reports to the Recorder, and copies of surfaces the application owns.
 *
 * TWO KINDS OF COPY
 *
 * A pass ends inside a command list the application is still recording (a new
 * binding, a barrier moving a target out of its render-target state,
 * EndRenderPass, Close). Its copy is recorded into that list, right there:
 * transition to COPY_SOURCE, CopyTextureRegion into a readback buffer,
 * transition back. It exists once the list has run, so it is read at Present
 * only if the list was executed by then.
 *
 * The end of the frame is copied at Present on VanGFX's own list, executed on
 * the queue the swap chain presents on, after everything the application
 * submitted. Its transitions start from the states the queue has reached
 * (hub.hpp, TrackedState).
 *
 * WHEN A BUFFER MAY GO
 *
 * Each copy holds a ticket for the list it was recorded into. The ticket is
 * signalled on the executing queue after the list (a fence per queue), or
 * abandoned when the list is reset without having been executed. A copy is
 * released only when its ticket says the GPU is done with it, or never ran:
 * copies read at Present are, and the rest wait in a graveyard that each
 * Present sweeps. One whose list is still pending when the context goes is
 * left allocated rather than freed under the GPU.
 */

#include "d3d12_impl.hpp"
#include <capture/recorder.hpp>
#include <format_map.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vangfx::backend::d3d12 {

namespace {

constexpr DWORD kGpuWaitMs = 5000;

template <typename T>
void release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }

D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES p{};
    p.Type             = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask  = 1;
    return p;
}

// A resolve needs a typed format; a typeless target takes the one a view of it
// would most likely use.
DXGI_FORMAT typed(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
    default:                                return f;
    }
}

// What the bytes of a copy of subresource 0 are. A depth-stencil format is two
// planes in D3D12, and plane 0 -- the one copied -- is the depth alone.
SurfaceFormat copied_format(DXGI_FORMAT resource, SurfaceFormat view, SurfaceRole role) noexcept {
    if (role == SurfaceRole::Depth) {
        switch (resource) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:       return SurfaceFormat::D24S8;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: return SurfaceFormat::D32F;
        case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:                 return SurfaceFormat::D32F;
        case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:                 return SurfaceFormat::D16;
        default: break;
        }
    }
    return view != SurfaceFormat::Unknown ? view : fmtmap::from_dxgi(resource);
}

// The access an enhanced barrier names for work done in `layout`.
D3D12_BARRIER_ACCESS access_for(D3D12_BARRIER_LAYOUT layout) noexcept {
    switch (layout) {
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET:        return D3D12_BARRIER_ACCESS_RENDER_TARGET;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:  return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:   return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS: return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_GENERIC_READ:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:    return D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE:     return D3D12_BARRIER_ACCESS_COPY_SOURCE;
    case D3D12_BARRIER_LAYOUT_COPY_DEST:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST:       return D3D12_BARRIER_ACCESS_COPY_DEST;
    case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:       return D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
    case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:         return D3D12_BARRIER_ACCESS_RESOLVE_DEST;
    case D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE:  return D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE;
    default:                                        return D3D12_BARRIER_ACCESS_COMMON;
    }
}

bool copy_readable(TrackedState s) noexcept {
    if (s.layout) {
        const auto l = static_cast<D3D12_BARRIER_LAYOUT>(s.value);
        return l == D3D12_BARRIER_LAYOUT_COPY_SOURCE || l == D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE;
    }
    return (s.value & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
}

// Moves subresource 0 of `res` between `s` and `target` (a legacy state), and
// back when `back`. Enhanced barriers for a resource that has a layout.
bool move(ID3D12GraphicsCommandList* list, ID3D12Resource* res, TrackedState s,
          D3D12_RESOURCE_STATES target, D3D12_BARRIER_LAYOUT target_layout, bool back, std::string& why) {
    if (!s.layout) {
        if (s.value == static_cast<uint32_t>(target)) return true;
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = back ? target : static_cast<D3D12_RESOURCE_STATES>(s.value);
        b.Transition.StateAfter  = back ? static_cast<D3D12_RESOURCE_STATES>(s.value) : target;
        list->ResourceBarrier(1, &b);
        return true;
    }
    const auto layout = static_cast<D3D12_BARRIER_LAYOUT>(s.value);
    if (layout == target_layout) return true;
    ID3D12GraphicsCommandList7* l7 = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&l7))) || !l7) {
        why = "the target is moved by enhanced barriers and this command list cannot record them";
        return false;
    }
    D3D12_TEXTURE_BARRIER tb{};
    tb.pResource    = res;
    tb.Subresources = { 0, 1, 0, 1, 0, 1 };
    const D3D12_BARRIER_SYNC   sync_target   = target_layout == D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE ? D3D12_BARRIER_SYNC_RESOLVE
                                                                                                    : D3D12_BARRIER_SYNC_COPY;
    const D3D12_BARRIER_ACCESS access_target = access_for(target_layout);
    if (!back) {
        tb.SyncBefore = D3D12_BARRIER_SYNC_ALL;   tb.SyncAfter = sync_target;
        tb.AccessBefore = access_for(layout);     tb.AccessAfter = access_target;
        tb.LayoutBefore = layout;                 tb.LayoutAfter = target_layout;
    } else {
        tb.SyncBefore = sync_target;              tb.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        tb.AccessBefore = access_target;          tb.AccessAfter = access_for(layout);
        tb.LayoutBefore = target_layout;          tb.LayoutAfter = layout;
    }
    D3D12_BARRIER_GROUP g{};
    g.Type             = D3D12_BARRIER_TYPE_TEXTURE;
    g.NumBarriers      = 1;
    g.pTextureBarriers = &tb;
    l7->Barrier(1, &g);
    l7->Release();
    return true;
}

// Whether the GPU has finished with, or will never run, the list a copy is in.
struct Ticket {
    ID3D12Fence* fence     = nullptr;   // the executing queue's; weak (the Snapshots own the fences)
    uint64_t     value     = 0;         // 0: not executed yet
    bool         abandoned = false;     // reset or dropped without being executed
    [[nodiscard]] bool done() const noexcept {
        return abandoned || (value && fence && fence->GetCompletedValue() >= value);
    }
};

class Copy final : public capture::Snapshot {
public:
    ~Copy() override { release(buffer); release(temp); }

    Result<capture::Pixels> read() override {
        if (!ticket || !ticket->value) {
            return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed, "the copy was never executed");
        }
        if (!ticket->done()) {
            return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed,
                "the GPU had not finished the copy within 5 s of Present (a queue waiting on a fence the application "
                "signals later?)");
        }
        void* mapped = nullptr;
        const D3D12_RANGE range{ 0, static_cast<SIZE_T>(total) };
        if (FAILED(buffer->Map(0, &range, &mapped)) || !mapped)
            return gfx_err<capture::Pixels>(ShaderError::ReadbackFailed, "Map failed on the readback buffer");
        capture::Pixels px;
        px.format    = format;
        px.width     = width;
        px.height    = height;
        px.row_pitch = static_cast<uint32_t>(row_bytes);
        px.bytes.resize(static_cast<size_t>(row_bytes) * rows);
        const auto* src = static_cast<const uint8_t*>(mapped) + footprint.Offset;
        for (UINT y = 0; y < rows; ++y)
            std::memcpy(px.bytes.data() + static_cast<size_t>(y) * row_bytes,
                        src + static_cast<size_t>(y) * footprint.Footprint.RowPitch, static_cast<size_t>(row_bytes));
        const D3D12_RANGE none{ 0, 0 };
        buffer->Unmap(0, &none);
        return px;
    }
    [[nodiscard]] bool deferred() const noexcept override { return in_list; }

    ID3D12Resource*                    buffer = nullptr;
    ID3D12Resource*                    temp   = nullptr;   // the resolved copy of a multisampled target
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT                               rows      = 0;
    UINT64                             row_bytes = 0;
    UINT64                             total     = 0;
    SurfaceFormat                      format = SurfaceFormat::Unknown;
    uint32_t                           width  = 0;
    uint32_t                           height = 0;
    bool                               in_list = false;
    std::shared_ptr<Ticket>            ticket;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Snapshots
// ─────────────────────────────────────────────────────────────────────────────

class Snapshots final : public capture::SnapshotHost {
public:
    explicit Snapshots(D3D12ContextImpl& ctx) : ctx_(ctx) {}
    ~Snapshots() override;

    bool can_snapshot_in(const void* scope, std::string& why) noexcept override;
    bool records_later(const void* scope) noexcept override { return scope != nullptr; }
    std::unique_ptr<capture::Snapshot> snapshot(const void* scope, const capture::TargetInfo& t, std::string& why) override;
    std::unique_ptr<capture::Snapshot> snapshot_backbuffer(FrameContext& fc, capture::TargetInfo& info, std::string& why,
                                                           bool copy) override;
    void flush() override;
    void retire(std::unique_ptr<capture::Snapshot> s) override;

    void present(ID3D12CommandQueue* queue) noexcept;
    void list_reset(const void* list) noexcept;
    void list_executed(ID3D12CommandQueue* queue, const void* list) noexcept;

private:
    struct QueueFence {
        ID3D12CommandQueue* queue = nullptr;   // held
        ID3D12Fence*        fence = nullptr;
        uint64_t            value = 0;
    };

    // Records the copy of `res` (in state `s`) into `list`.
    std::unique_ptr<Copy> record(ID3D12GraphicsCommandList* list, ID3D12Resource* res, TrackedState s,
                                 const capture::TargetInfo& t, std::string& why);
    QueueFence* fence_for(ID3D12CommandQueue* q) noexcept;   // call with mutex_ held
    bool open_own_list(std::string& why);
    void sweep() noexcept;                                   // call with mutex_ held

    D3D12ContextImpl& ctx_;
    std::mutex        mutex_;

    ID3D12CommandQueue*        present_queue_ = nullptr;   // weak: this frame's
    ID3D12CommandAllocator*    own_alloc_ = nullptr;
    ID3D12GraphicsCommandList* own_list_  = nullptr;
    bool                       own_open_  = false;
    std::shared_ptr<Ticket>    own_ticket_;

    std::vector<QueueFence>                                 fences_;
    std::unordered_map<const void*, std::shared_ptr<Ticket>> list_tickets_;   // lists holding copies, not yet executed
    std::vector<ID3D12CommandQueue*>                        to_wait_;         // queues to wait on at flush
    std::vector<std::unique_ptr<capture::Snapshot>>         graveyard_;
    std::atomic<size_t>                                     tickets_{ 0 };   // list_tickets_.size(), read unlocked
};

Snapshots::~Snapshots() {
    std::lock_guard lk(mutex_);
    InternalScope internal_work;
    // Wait (bounded) for copies still on the GPU; one whose list never ran is
    // left allocated: the list may still be executed after this.
    for (QueueFence& f : fences_) {
        if (f.fence->GetCompletedValue() >= f.value) continue;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev && SUCCEEDED(f.fence->SetEventOnCompletion(f.value, ev))) WaitForSingleObject(ev, 2000);
        if (ev) CloseHandle(ev);
    }
    size_t leaked = 0;
    for (auto& s : graveyard_) {
        auto* c = static_cast<Copy*>(s.get());
        if (c->ticket && c->ticket->done()) s.reset();
        else { (void)s.release(); ++leaked; }
    }
    graveyard_.clear();
    if (own_ticket_ && own_ticket_->value && !own_ticket_->done()) {
        (void)own_list_; (void)own_alloc_;   // still on the GPU: left
    } else {
        release(own_list_);
        release(own_alloc_);
    }
    for (QueueFence& f : fences_) {
        if (f.fence->GetCompletedValue() >= f.value) release(f.fence);
        release(f.queue);
    }
    if (leaked) {
        ctx_.log(LogLevel::Warn, "D3D12 capture: " + std::to_string(leaked) + " copy buffer(s) were recorded into command "
                                 "lists that had not run when the context was destroyed; they are left allocated.");
    }
}

bool Snapshots::can_snapshot_in(const void* scope, std::string& why) noexcept {
    if (!scope) return true;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(const_cast<void*>(scope));
    const ListState* st = list_state(list);
    if (!st) { why = "the command list is not being followed"; return false; }
    if (st->type != D3D12_COMMAND_LIST_TYPE_DIRECT) { why = "not a direct command list"; return false; }
    if (st->in_render_pass) { why = "inside a render pass, where nothing can be copied"; return false; }
    if (st->suspended) { why = "the list's render pass was suspended, and nothing may follow it"; return false; }
    return true;
}

Snapshots::QueueFence* Snapshots::fence_for(ID3D12CommandQueue* q) noexcept {
    for (QueueFence& f : fences_) if (f.queue == q) return &f;
    QueueFence f;
    if (FAILED(ctx_.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence))) || !f.fence) return nullptr;
    f.queue = q;
    q->AddRef();
    fences_.push_back(f);
    return &fences_.back();
}

std::unique_ptr<Copy> Snapshots::record(ID3D12GraphicsCommandList* list, ID3D12Resource* res, TrackedState s,
                                        const capture::TargetInfo& t, std::string& why) {
    ID3D12Device* device = ctx_.device();
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) { why = "not a 2D texture"; return nullptr; }
    if (s.layout && static_cast<D3D12_BARRIER_LAYOUT>(s.value) == D3D12_BARRIER_LAYOUT_UNDEFINED) {
        why = "its layout is UNDEFINED: there is nothing to copy";
        return nullptr;
    }
    const bool msaa = desc.SampleDesc.Count > 1;
    if (msaa && t.role == SurfaceRole::Depth) { why = "a multisampled depth buffer cannot be resolved to copy"; return nullptr; }

    auto c = std::make_unique<Copy>();
    c->format = copied_format(desc.Format, t.format, t.role);
    c->width  = static_cast<uint32_t>(desc.Width);
    c->height = desc.Height;

    // The resolve target of a multisampled one.
    D3D12_RESOURCE_DESC copy_desc = desc;
    DXGI_FORMAT resolve_format = typed(t.format != SurfaceFormat::Unknown ? fmtmap::to_dxgi(t.format) : desc.Format);
    if (resolve_format == DXGI_FORMAT_UNKNOWN) resolve_format = typed(desc.Format);
    if (msaa) {
        copy_desc.SampleDesc = { 1, 0 };
        copy_desc.MipLevels  = 1;
        copy_desc.DepthOrArraySize = 1;
        copy_desc.Format     = resolve_format;
        copy_desc.Flags      = D3D12_RESOURCE_FLAG_NONE;
        copy_desc.Layout     = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        copy_desc.Alignment  = 0;
        const auto heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &copy_desc, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                                   nullptr, IID_PPV_ARGS(&c->temp)))) {
            why = "could not create the texture to resolve the multisampled target into";
            return nullptr;
        }
        c->format = copied_format(resolve_format, SurfaceFormat::Unknown, t.role);
    }

    device->GetCopyableFootprints(&copy_desc, 0, 1, 0, &c->footprint, &c->rows, &c->row_bytes, &c->total);
    if (!c->total) { why = "the format has no copyable footprint"; return nullptr; }
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = c->total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&c->buffer)))) {
        why = "could not create a readback buffer of " + std::to_string(c->total) + " bytes";
        return nullptr;
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = c->buffer;
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = c->footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    if (msaa) {
        if (!move(list, res, s, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE, false, why)) return nullptr;
        list->ResolveSubresource(c->temp, 0, res, 0, resolve_format);
        move(list, res, s, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE, true, why);
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = c->temp;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        src.pResource = c->temp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    } else if (copy_readable(s)) {
        src.pResource = res;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    } else {
        if (!move(list, res, s, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE, false, why)) return nullptr;
        src.pResource = res;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        move(list, res, s, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE, true, why);
    }
    return c;
}

std::unique_ptr<capture::Snapshot> Snapshots::snapshot(const void* scope, const capture::TargetInfo& t, std::string& why) {
    ID3D12Resource* res = resource_from_id(t.id);
    if (!res) { why = "the resource is gone"; return nullptr; }

    // The state it is in where the copy goes.
    TrackedState s = state_of(res);
    ListState* st = nullptr;
    if (scope) {
        st = list_state(static_cast<ID3D12GraphicsCommandList*>(const_cast<void*>(scope)));
        if (!st) { why = "the command list is not being followed"; return nullptr; }
        if (auto it = st->states.find(res); it != st->states.end()) {
            s = it->second;
        } else {
            // Not moved in this list since it was bound, so it is still in the
            // state it was drawn in.
            const bool layout = s.known && s.layout;
            if (t.role == SurfaceRole::Depth) {
                const bool read_only = layout ? s.value == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ
                                              : s.known && s.value == D3D12_RESOURCE_STATE_DEPTH_READ;
                s = layout ? TrackedState{ static_cast<uint32_t>(read_only ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ
                                                                           : D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE), true, true }
                           : TrackedState{ static_cast<uint32_t>(read_only ? D3D12_RESOURCE_STATE_DEPTH_READ
                                                                           : D3D12_RESOURCE_STATE_DEPTH_WRITE), false, true };
            } else {
                s = layout ? TrackedState{ static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_RENDER_TARGET), true, true }
                           : TrackedState{ static_cast<uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET), false, true };
            }
        }
    } else if (!s.known) {
        // Never seen created or moved: it is taken to be where drawing left it.
        s = { static_cast<uint32_t>(t.role == SurfaceRole::Depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                                 : D3D12_RESOURCE_STATE_RENDER_TARGET), false, true };
    }

    std::lock_guard lk(mutex_);
    InternalScope internal_work;
    ID3D12GraphicsCommandList* list = nullptr;
    std::shared_ptr<Ticket> ticket;
    if (scope) {
        list = static_cast<ID3D12GraphicsCommandList*>(const_cast<void*>(scope));
        auto& tk = list_tickets_[scope];
        if (!tk) tk = std::make_shared<Ticket>();
        ticket = tk;
        tickets_.store(list_tickets_.size(), std::memory_order_release);
    } else {
        if (!open_own_list(why)) return nullptr;
        list   = own_list_;
        ticket = own_ticket_;
    }
    auto c = record(list, res, s, t, why);
    if (!c) return nullptr;
    c->in_list = scope != nullptr;
    c->ticket  = std::move(ticket);
    return c;
}

std::unique_ptr<capture::Snapshot> Snapshots::snapshot_backbuffer(FrameContext& fc, capture::TargetInfo& info, std::string& why,
                                                                  bool copy) {
    IDXGISwapChain* sc = static_cast<D3D12FrameContextImpl&>(fc).swap_chain();
    IDXGISwapChain3* sc3 = nullptr;
    if (!sc || FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) { why = "no flip-model swap chain"; return nullptr; }
    ID3D12Resource* bb = nullptr;
    const HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) { why = "the swap chain would not give its back buffer"; return nullptr; }
    bb->Release();   // the swap chain holds it

    const D3D12_RESOURCE_DESC d = bb->GetDesc();
    info.id = resource_id(bb);
    if (!info.id) { announce_resource(ctx_.device(), bb); info.id = resource_id(bb); }
    info.role   = SurfaceRole::Backbuffer;
    info.format = fmtmap::from_dxgi(d.Format);
    info.width  = static_cast<uint32_t>(d.Width);
    info.height = d.Height;
    if (!copy) return nullptr;

    TrackedState s = state_of(bb);
    if (!s.known) s = { static_cast<uint32_t>(D3D12_RESOURCE_STATE_PRESENT), false, true };
    std::lock_guard lk(mutex_);
    InternalScope internal_work;
    if (!open_own_list(why)) return nullptr;
    auto c = record(own_list_, bb, s, info, why);
    if (!c) return nullptr;
    c->ticket = own_ticket_;
    return c;
}

bool Snapshots::open_own_list(std::string& why) {
    if (own_open_) return true;
    ID3D12Device* device = ctx_.device();
    if (!own_alloc_) {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&own_alloc_))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, own_alloc_, nullptr, IID_PPV_ARGS(&own_list_)))) {
            release(own_alloc_);
            why = "could not create a command list for the copies";
            return false;
        }
    } else {
        // The last frame's copies must be done before the allocator is reused.
        if (own_ticket_ && own_ticket_->value && !own_ticket_->done()) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev && SUCCEEDED(own_ticket_->fence->SetEventOnCompletion(own_ticket_->value, ev)))
                WaitForSingleObject(ev, kGpuWaitMs);
            if (ev) CloseHandle(ev);
            if (!own_ticket_->done()) { why = "the previous capture's copies are still on the GPU"; return false; }
        }
        if (FAILED(own_alloc_->Reset()) || FAILED(own_list_->Reset(own_alloc_, nullptr))) {
            why = "could not reset the copy command list";
            return false;
        }
    }
    own_open_   = true;
    own_ticket_ = std::make_shared<Ticket>();
    return true;
}

void Snapshots::flush() {
    std::lock_guard lk(mutex_);
    InternalScope internal_work;
    if (own_open_) {
        own_open_ = false;
        ID3D12CommandQueue* q = present_queue_ ? present_queue_ : ctx_.queue();
        if (SUCCEEDED(own_list_->Close())) {
            ID3D12CommandList* lists[] = { own_list_ };
            q->ExecuteCommandLists(1, lists);
            if (QueueFence* f = fence_for(q); f && SUCCEEDED(q->Signal(f->fence, ++f->value))) {
                own_ticket_->fence = f->fence;
                own_ticket_->value = f->value;
                if (std::find(to_wait_.begin(), to_wait_.end(), q) == to_wait_.end()) to_wait_.push_back(q);
            } else {
                own_ticket_->abandoned = true;
            }
        } else {
            own_ticket_->abandoned = true;
        }
    }
    // Every queue that ran a list holding copies, and the present queue.
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (ID3D12CommandQueue* q : to_wait_) {
        QueueFence* f = fence_for(q);
        if (!f || f->fence->GetCompletedValue() >= f->value) continue;
        if (ev && SUCCEEDED(f->fence->SetEventOnCompletion(f->value, ev))) WaitForSingleObject(ev, kGpuWaitMs);
    }
    if (ev) CloseHandle(ev);
    to_wait_.clear();
}

void Snapshots::retire(std::unique_ptr<capture::Snapshot> s) {
    if (!s) return;
    auto* c = static_cast<Copy*>(s.get());
    std::lock_guard lk(mutex_);
    if (c->ticket && c->ticket->done()) return;   // freed here
    graveyard_.push_back(std::move(s));
}

void Snapshots::sweep() noexcept {
    size_t n = 0;
    for (auto& s : graveyard_) {
        auto* c = static_cast<Copy*>(s.get());
        if (!(c->ticket && c->ticket->done())) graveyard_[n++] = std::move(s);
    }
    graveyard_.resize(n);
}

void Snapshots::present(ID3D12CommandQueue* queue) noexcept {
    std::lock_guard lk(mutex_);
    present_queue_ = queue;
    if (!graveyard_.empty()) sweep();
}

void Snapshots::list_reset(const void* list) noexcept {
    if (!tickets_.load(std::memory_order_acquire)) return;
    std::lock_guard lk(mutex_);
    auto it = list_tickets_.find(list);
    if (it == list_tickets_.end()) return;
    it->second->abandoned = true;   // recorded, never executed: the GPU never saw it
    list_tickets_.erase(it);
    tickets_.store(list_tickets_.size(), std::memory_order_release);
    if (!graveyard_.empty()) sweep();
}

void Snapshots::list_executed(ID3D12CommandQueue* queue, const void* list) noexcept {
    if (!tickets_.load(std::memory_order_acquire)) return;
    std::lock_guard lk(mutex_);
    auto it = list_tickets_.find(list);
    if (it == list_tickets_.end()) return;
    InternalScope internal_work;
    if (QueueFence* f = fence_for(queue); f && SUCCEEDED(queue->Signal(f->fence, ++f->value))) {
        it->second->fence = f->fence;
        it->second->value = f->value;
        if (std::find(to_wait_.begin(), to_wait_.end(), queue) == to_wait_.end()) to_wait_.push_back(queue);
    }
    list_tickets_.erase(it);
    tickets_.store(list_tickets_.size(), std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
//  The context's side
// ─────────────────────────────────────────────────────────────────────────────

Snapshots* make_snapshots(D3D12ContextImpl& ctx) noexcept {
    try { return new Snapshots(ctx); } catch (...) { return nullptr; }
}
void destroy_snapshots(Snapshots* s) noexcept { delete s; }
void snapshots_present(Snapshots* s, ID3D12CommandQueue* q) noexcept { if (s) s->present(q); }

capture::SnapshotHost* D3D12ContextImpl::snapshot_host() noexcept { return snapshots_; }

// ─────────────────────────────────────────────────────────────────────────────
//  The hooks' side
// ─────────────────────────────────────────────────────────────────────────────

namespace {

capture::TargetInfo target_info(const BoundTarget& b, SurfaceRole role, ID3D12Device* device) noexcept {
    capture::TargetInfo t;
    if (!b.res) return t;
    const D3D12_RESOURCE_DESC d = b.res->GetDesc();
    t.id = resource_id(b.res);
    if (!t.id) { announce_resource(device, b.res); t.id = resource_id(b.res); }
    t.role   = role;
    t.format = fmtmap::from_dxgi(b.format != DXGI_FORMAT_UNKNOWN ? b.format : d.Format);
    t.width  = static_cast<uint32_t>(d.Width);
    t.height = d.Height;
    return t;
}

capture::TargetSet target_set(const ListState& st) noexcept {
    capture::TargetSet set;
    set.count = st.bound_count;
    for (uint32_t i = 0; i < st.bound_count; ++i) set.colour[i] = target_info(st.bound[i], SurfaceRole::Color, st.device);
    set.depth = target_info(st.bound_depth, SurfaceRole::Depth, st.device);
    return set;
}

template <typename Fn>
void each_recorder(const ListState& st, Fn&& fn) {
    for (D3D12ContextImpl* ctx : st.contexts)
        if (auto rec = ctx->recorder()) fn(*ctx, *rec);
}

} // namespace

bool capturing(const ListState& st) noexcept {
    for (D3D12ContextImpl* ctx : st.contexts) if (ctx->capture_active()) return true;
    return false;
}

void capture_bind(ID3D12GraphicsCommandList* list, ListState& st) {
    if (!capturing(st)) return;
    const capture::TargetSet set = target_set(st);
    each_recorder(st, [&](D3D12ContextImpl&, capture::Recorder& rec) { rec.bind(list, set); });
}

void capture_draw(ID3D12GraphicsCommandList* list, ListState& st) {
    for (D3D12ContextImpl* ctx : st.contexts) ctx->note_draw();
    if (!capturing(st)) return;
    each_recorder(st, [&](D3D12ContextImpl&, capture::Recorder& rec) {
        if (!rec.knows(list)) rec.bind(list, target_set(st));
        rec.draw(list);
    });
}

void capture_pass_break(ID3D12GraphicsCommandList* list, ListState& st) {
    if (!capturing(st)) return;
    each_recorder(st, [&](D3D12ContextImpl&, capture::Recorder& rec) { rec.pass_break(list); });
}

void capture_close(ID3D12GraphicsCommandList* list, ListState& st) {
    if (!capturing(st)) return;
    each_recorder(st, [&](D3D12ContextImpl&, capture::Recorder& rec) { rec.scope_end(list); });
}

void capture_reset(ID3D12GraphicsCommandList* list, ListState& st) {
    for (D3D12ContextImpl* ctx : st.contexts)
        if (Snapshots* s = ctx->snapshots()) s->list_reset(list);
}

void capture_executed(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ListState& st) {
    for (D3D12ContextImpl* ctx : st.contexts) {
        if (Snapshots* s = ctx->snapshots()) s->list_executed(queue, list);
        if (auto rec = ctx->recorder()) rec->submitted(list);
    }
}

} // namespace vangfx::backend::d3d12
