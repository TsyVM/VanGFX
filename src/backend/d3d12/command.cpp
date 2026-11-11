/**
 * src/backend/d3d12/command.cpp — command queues and command lists: the queue's
 * ExecuteCommandLists / Signal / Wait, the list's Reset / Close, and the hooks
 * that find every command list class in the first place.
 *
 * FINDING THE LISTS
 *
 * The list hooks go on the list classes' vtables, and a vtable can only be
 * reached through an object. At attach time VanGFX makes a direct list and a
 * bundle (their classes differ) and installs through those, which covers every
 * list the application already has; CreateCommandList and CreateCommandList1 are
 * hooked as well, so a class not seen yet is picked up the moment one is made.
 * (The old code installed the list hooks from inside the Reset hook, which
 * nothing ever installed — so no list was ever hooked.)
 *
 * SLOTS BEYOND THE BASE INTERFACE
 *
 * A method added by ID3D12Device4 or ID3D12GraphicsCommandList6 lives past the
 * end of the base interface's vtable. Patching it on an object that does not
 * implement that interface would write past the end of its table, so every such
 * install goes through the pointer QueryInterface returns for the interface.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <vangfx/d3d12/command.hpp>

#include <vector>

namespace vangfx::backend::d3d12 {

using namespace ::vangfx::d3d12;
namespace dev = ::vangfx::d3d12_slots::device;
namespace q   = ::vangfx::d3d12_slots::command_queue;
namespace lst = ::vangfx::d3d12_slots::graphics_command_list;

namespace {

using CreateQueueFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using CreateListFn   = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                   ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using CreateList1Fn  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                   D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
using ExecuteFn      = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using FenceOpFn      = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using CloseFn        = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using ResetFn        = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);

hooks::Site s_create_queue { dev::CreateCommandQueue,  "ID3D12Device::CreateCommandQueue" };
hooks::Site s_create_list  { dev::CreateCommandList,   "ID3D12Device::CreateCommandList" };
hooks::Site s_create_list1 { dev::CreateCommandList1,  "ID3D12Device4::CreateCommandList1" };
hooks::Site s_execute      { q::ExecuteCommandLists,   "ID3D12CommandQueue::ExecuteCommandLists" };
hooks::Site s_signal       { q::Signal,                "ID3D12CommandQueue::Signal" };
hooks::Site s_wait         { q::Wait,                  "ID3D12CommandQueue::Wait" };
hooks::Site s_close        { lst::Close,               "ID3D12GraphicsCommandList::Close" };
hooks::Site s_reset        { lst::Reset,               "ID3D12GraphicsCommandList::Reset" };

CommandListType to_list_type(D3D12_COMMAND_LIST_TYPE t) noexcept {
    switch (t) {
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:        return CommandListType::Bundle;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:       return CommandListType::Compute;
    case D3D12_COMMAND_LIST_TYPE_COPY:          return CommandListType::Copy;
    case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE:  return CommandListType::VideoDecode;
    case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS: return CommandListType::VideoProcess;
    case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE:  return CommandListType::VideoEncode;
    default:                                    return CommandListType::Direct;
    }
}

// A new list's class may be one not hooked yet (the first bundle, say).
void adopt_new_list(void** out, REFIID riid) noexcept {
    if (!out || !*out) return;
    auto* unk = static_cast<IUnknown*>(*out);
    ID3D12GraphicsCommandList* gcl = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&gcl))) && gcl) {
        install_list_hooks(gcl);
        gcl->Release();
    }
    (void)riid;
}

// ── Device ───────────────────────────────────────────────────────────────────

HRESULT STDMETHODCALLTYPE hook_create_queue(ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                            REFIID riid, void** out) {
    const HRESULT hr = s_create_queue.original<CreateQueueFn>(self)(self, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12CommandQueue* queue = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
            install_queue_hooks(queue);
            queue->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_list(ID3D12Device* self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                           ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso,
                                           REFIID riid, void** out) {
    const HRESULT hr = s_create_list.original<CreateListFn>(self)(self, mask, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr)) adopt_new_list(out, riid);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_list1(ID3D12Device4* self, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                            D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** out) {
    const HRESULT hr = s_create_list1.original<CreateList1Fn>(self)(self, mask, type, flags, riid, out);
    if (SUCCEEDED(hr)) adopt_new_list(out, riid);
    return hr;
}

// ── Queue ────────────────────────────────────────────────────────────────────

void STDMETHODCALLTYPE hook_execute(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists) {
    auto orig = s_execute.original<ExecuteFn>(self);
    if (hub_idle() || internal() || !n || !lists) return orig(self, n, lists);

    // The graphics lists among them (a list is alive while it is executed, so
    // the pointers outlive the QueryInterface references).
    std::vector<ID3D12GraphicsCommandList*> gcls;
    gcls.reserve(n);
    for (UINT i = 0; i < n; ++i) {
        ID3D12GraphicsCommandList* gcl = nullptr;
        if (lists[i] && SUCCEEDED(lists[i]->QueryInterface(IID_PPV_ARGS(&gcl))) && gcl) gcl->Release();
        gcls.push_back(gcl);
    }

    bool intercept = false;
    {
        HubReader reader;
        const ContextSet contexts = contexts_for(device_of(self));
        if (!contexts) return orig(self, n, lists);
        bool wants = false;
        for (D3D12ContextImpl* ctx : contexts) wants |= ctx->wants_command();
        if (wants) {
            CommandListEvent ev{};
            ev.action          = CommandListAction::Submit;
            ev.command_queue   = { id_of(self) };
            ev.submitted_count = n;
            for (UINT i = 0; i < n && i < kMaxSubmittedLists; ++i)
                if (ListState* st = list_state(gcls[i])) ev.submitted_lists[i] = { st->id };
            for (D3D12ContextImpl* ctx : contexts) {
                if (!ctx->wants_command()) continue;
                D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index());
                ctx->dispatch_command_list(fc, ev);
            }
            intercept = ev.intercept;
        }
    }
    if (intercept) return;
    orig(self, n, lists);

    // What the lists' barriers did is now the queue's state, and copies
    // recorded into them will have run once the queue gets this far.
    HubReader reader;
    for (ID3D12GraphicsCommandList* gcl : gcls) {
        ListState* st = list_state(gcl);
        if (!st) continue;
        for (const auto& [res, state] : st->states) {
            if (!resource_id(res)) announce_resource(st->device, res);   // met here first: watch its destruction
            state_set(res, state);
        }
        capture_executed(self, gcl, *st);
    }
}

HRESULT fence_op(hooks::Site& site, CommandListAction action, ID3D12CommandQueue* self, ID3D12Fence* fence, UINT64 value) {
    auto orig = site.original<FenceOpFn>(self);
    if (hub_idle() || internal()) return orig(self, fence, value);
    bool intercept = false;
    {
        HubReader reader;
        const ContextSet contexts = contexts_for(device_of(self));
        if (contexts) {
            CommandListEvent ev{};
            ev.action        = action;
            ev.command_queue = { id_of(self) };
            ev.fence         = { id_of(fence) };
            ev.fence_value   = value;
            for (D3D12ContextImpl* ctx : contexts) {
                if (!ctx->wants_command()) continue;
                D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index());
                ctx->dispatch_command_list(fc, ev);
            }
            intercept = ev.intercept;
        }
    }
    return intercept ? S_OK : orig(self, fence, value);
}

HRESULT STDMETHODCALLTYPE hook_signal(ID3D12CommandQueue* self, ID3D12Fence* fence, UINT64 value) {
    return fence_op(s_signal, CommandListAction::Signal, self, fence, value);
}
HRESULT STDMETHODCALLTYPE hook_wait(ID3D12CommandQueue* self, ID3D12Fence* fence, UINT64 value) {
    return fence_op(s_wait, CommandListAction::Wait, self, fence, value);
}

// ── List ─────────────────────────────────────────────────────────────────────

bool list_event(ID3D12GraphicsCommandList* self, ListState* st, CommandListAction action) {
    CommandListEvent ev{};
    ev.action       = action;
    ev.list_type    = to_list_type(st->type);
    ev.command_list = { st->id };
    for (D3D12ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_command()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index(), nullptr, self, st);
        ctx->dispatch_command_list(fc, ev);
    }
    return ev.intercept;
}

HRESULT STDMETHODCALLTYPE hook_reset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* alloc,
                                     ID3D12PipelineState* initial) {
    auto orig = s_reset.original<ResetFn>(self);
    if (hub_idle() || internal()) return orig(self, alloc, initial);
    bool intercept = false;
    {
        HubReader reader;
        if (ListState* st = list_state(self)) {
            // A reset list starts over with nothing bound but the initial state;
            // copies recorded into it that never ran are dropped.
            capture_reset(self, *st);
            st->reset();
            if (initial) {
                const PsoRecord rec = pso_find(initial);
                st->pso = { rec.id };
                st->pso_raw = initial;
                st->vs_hash = rec.vs_hash;
                st->ps_hash = rec.ps_hash;
            }
            intercept = list_event(self, st, CommandListAction::Reset);
        }
    }
    return intercept ? S_OK : orig(self, alloc, initial);
}

HRESULT STDMETHODCALLTYPE hook_close(ID3D12GraphicsCommandList* self) {
    auto orig = s_close.original<CloseFn>(self);
    if (hub_idle() || internal()) return orig(self);
    bool intercept = false;
    {
        HubReader reader;
        if (ListState* st = list_state(self)) {
            // A pass still open ends here: its copies go in before the list closes.
            capture_close(self, *st);
            intercept = list_event(self, st, CommandListAction::Close);
        }
    }
    return intercept ? S_OK : orig(self);
}

} // namespace

// ── Installation ─────────────────────────────────────────────────────────────

bool install_queue_hooks(ID3D12CommandQueue* queue) noexcept {
    bool ok = s_execute.install(queue, &hook_execute);
    ok &= s_signal.install(queue, &hook_signal);
    ok &= s_wait.install(queue, &hook_wait);
    return ok;
}

bool install_list_hooks(ID3D12GraphicsCommandList* list) noexcept {
    if (!list) return false;
    bool ok = s_close.install(list, &hook_close);
    ok &= s_reset.install(list, &hook_reset);
    install_draw_hooks(list);
    install_ia_hooks(list);
    install_barrier_hooks(list);
    install_rendertarget_hooks(list);
    install_state_hooks(list);
    install_query_list_hooks(list);
    return ok;
}

bool install_device_hooks(ID3D12Device* device) noexcept {
    bool ok = s_create_queue.install(device, &hook_create_queue);
    ok &= s_create_list.install(device, &hook_create_list);
    ID3D12Device4* d4 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d4))) && d4) {
        s_create_list1.install(d4, &hook_create_list1);
        d4->Release();
    }
    install_pso_hooks(device);
    install_resource_hooks(device);
    install_descriptor_hooks(device);
    return ok;
}

bool prime_list_hooks(ID3D12Device* device) noexcept {
    InternalScope internal_work;
    bool any = false;
    const D3D12_COMMAND_LIST_TYPE types[] = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_BUNDLE };
    for (auto type : types) {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc))) && alloc &&
            SUCCEEDED(device->CreateCommandList(0, type, alloc, nullptr, IID_PPV_ARGS(&list))) && list) {
            any |= install_list_hooks(list);
            list->Close();
        }
        if (list)  list->Release();
        if (alloc) alloc->Release();
    }
    return any;
}

void uninstall_command_hooks() noexcept {
    for (hooks::Site* s : { &s_create_queue, &s_create_list, &s_create_list1, &s_execute,
                            &s_signal, &s_wait, &s_close, &s_reset })
        s->uninstall();
}

void uninstall_all_hooks() noexcept {
    uninstall_factory_hooks();
    uninstall_present_hooks();
    uninstall_command_hooks();
    uninstall_draw_hooks();
    uninstall_ia_hooks();
    uninstall_barrier_hooks();
    uninstall_rendertarget_hooks();
    uninstall_state_hooks();
    uninstall_query_hooks();
    uninstall_pso_hooks();
    uninstall_resource_hooks();
    uninstall_descriptor_hooks();
}

} // namespace vangfx::backend::d3d12
