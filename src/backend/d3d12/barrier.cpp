/**
 * src/backend/d3d12/barrier.cpp — ID3D12GraphicsCommandList::ResourceBarrier
 * and ID3D12GraphicsCommandList7::Barrier.
 *
 * Each ResourceBarrier call becomes one ResourceBarrierBatch. A callback may
 * suppress the whole call (batch.intercept) or single barriers
 * (barriers[i].intercept); the runtime then receives the barriers nobody
 * suppressed. Either is for debugging: a missing barrier the frame depends on
 * corrupts rendering or removes the device.
 *
 * Whether or not anything listens, the transitions that reach the runtime are
 * followed (hub.hpp, TrackedState), and one moving a bound render target or
 * depth buffer out of the state it was drawn in ends a capture's pass there,
 * before the barrier. Enhanced barriers are followed the same way; they are not
 * reported as events.
 */

#include "d3d12_impl.hpp"
#include <compat/hook_site.hpp>
#include <compat/d3d12_slots.hpp>
#include <vangfx/events/surface.hpp>

#include <vector>

namespace vangfx::backend::d3d12 {

using namespace ::vangfx::d3d12;
namespace lst = ::vangfx::d3d12_slots::graphics_command_list;

namespace {

using BarrierFn  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using Barrier7Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT32, const D3D12_BARRIER_GROUP*);

hooks::Site s_barrier { lst::ResourceBarrier, "ResourceBarrier" };
hooks::Site s_barrier7{ lst::Barrier,         "ID3D12GraphicsCommandList7::Barrier" };

bool covers_first(UINT sub) noexcept { return sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || sub == 0; }

// Follows the transitions about to reach the runtime: a capture's pass ends
// before one that moves a bound target, and the list keeps where each lands.
void follow(ID3D12GraphicsCommandList* self, ListState& st, UINT n, const D3D12_RESOURCE_BARRIER* barriers,
            const ResourceBarrier* converted) {
    bool breaks = false;
    if (capturing(st)) {
        for (UINT i = 0; i < n; ++i) {
            if (converted && converted[i].intercept) continue;
            const D3D12_RESOURCE_BARRIER& b = barriers[i];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)) continue;
            const auto& t = b.Transition;
            if (!covers_first(t.Subresource) || t.StateBefore == t.StateAfter || !st.binds(t.pResource)) continue;
            // The copy the break records starts from where the barrier says it is.
            st.states.try_emplace(t.pResource, TrackedState{ static_cast<uint32_t>(t.StateBefore), false, true });
            breaks = true;
        }
    }
    if (breaks) capture_pass_break(self, st);
    for (UINT i = 0; i < n; ++i) {
        if (converted && converted[i].intercept) continue;
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)) continue;
        if (!covers_first(b.Transition.Subresource) || !b.Transition.pResource) continue;
        st.states[b.Transition.pResource] = { static_cast<uint32_t>(b.Transition.StateAfter), false, true };
    }
}

// Whether an enhanced texture barrier's range includes subresource 0.
bool covers_first(const D3D12_BARRIER_SUBRESOURCE_RANGE& r) noexcept {
    if (r.NumMipLevels == 0) return r.IndexOrFirstMipLevel == 0 || r.IndexOrFirstMipLevel == 0xFFFFFFFFu;
    return r.IndexOrFirstMipLevel == 0 && r.FirstArraySlice == 0 && r.FirstPlane == 0 &&
           r.NumArraySlices != 0 && r.NumPlanes != 0;
}

void STDMETHODCALLTYPE hook_barrier7(ID3D12GraphicsCommandList7* self, UINT32 n, const D3D12_BARRIER_GROUP* groups) {
    auto orig = s_barrier7.original<Barrier7Fn>(self);
    if (hub_idle() || internal() || n == 0 || !groups) return orig(self, n, groups);
    HubReader reader;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self);
    ListState* st = list_state(list);
    if (!st) return orig(self, n, groups);

    bool breaks = false;
    const bool capture = capturing(*st);
    for (UINT32 g = 0; g < n; ++g) {
        if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE || !groups[g].pTextureBarriers) continue;
        for (UINT32 i = 0; i < groups[g].NumBarriers; ++i) {
            const D3D12_TEXTURE_BARRIER& t = groups[g].pTextureBarriers[i];
            if (!t.pResource || !covers_first(t.Subresources)) continue;
            if (capture && t.LayoutBefore != t.LayoutAfter && st->binds(t.pResource)) {
                st->states.try_emplace(t.pResource, TrackedState{ static_cast<uint32_t>(t.LayoutBefore), true, true });
                breaks = true;
            }
        }
    }
    if (breaks) capture_pass_break(list, *st);
    for (UINT32 g = 0; g < n; ++g) {
        if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE || !groups[g].pTextureBarriers) continue;
        for (UINT32 i = 0; i < groups[g].NumBarriers; ++i) {
            const D3D12_TEXTURE_BARRIER& t = groups[g].pTextureBarriers[i];
            if (!t.pResource || !covers_first(t.Subresources) || t.LayoutBefore == t.LayoutAfter) continue;
            st->states[t.pResource] = { static_cast<uint32_t>(t.LayoutAfter), true, true };
        }
    }
    orig(self, n, groups);
}

vangfx::d3d12::ResourceBarrier convert(const D3D12_RESOURCE_BARRIER& in) noexcept {
    vangfx::d3d12::ResourceBarrier b{};
    if (in.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)    b.flags = ResourceBarrierFlags::BeginOnly;
    else if (in.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) b.flags = ResourceBarrierFlags::EndOnly;
    switch (in.Type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
        b.type = BarrierType::Transition;
        b.resource_id = resource_id(in.Transition.pResource);
        b.transition.before      = static_cast<ResourceState>(in.Transition.StateBefore);
        b.transition.after       = static_cast<ResourceState>(in.Transition.StateAfter);
        b.transition.subresource = in.Transition.Subresource;
        break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        b.type = BarrierType::Aliasing;
        b.resource_id                 = resource_id(in.Aliasing.pResourceAfter);
        b.aliasing.before_resource_id = resource_id(in.Aliasing.pResourceBefore);
        b.aliasing.after_resource_id  = resource_id(in.Aliasing.pResourceAfter);
        break;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        b.type = BarrierType::Uav;
        b.resource_id = resource_id(in.UAV.pResource);
        break;
    default: break;
    }
    return b;
}

void STDMETHODCALLTYPE hook_barrier(ID3D12GraphicsCommandList* self, UINT n, const D3D12_RESOURCE_BARRIER* barriers) {
    auto orig = s_barrier.original<BarrierFn>(self);
    if (hub_idle() || internal() || n == 0 || !barriers) return orig(self, n, barriers);

    HubReader reader;
    ListState* st = list_state(self);
    if (!st) return orig(self, n, barriers);
    bool wants = false;
    for (D3D12ContextImpl* ctx : st->contexts) wants |= ctx->wants_barrier();
    if (!wants) {
        follow(self, *st, n, barriers, nullptr);
        return orig(self, n, barriers);
    }

    // Every barrier is reported (the old kMaxBarriersPerBatch cap silently
    // dropped the rest from view); the vector is per call, which barrier-heavy
    // frames can afford far better than a missing report.
    std::vector<vangfx::d3d12::ResourceBarrier> converted(n);
    for (UINT i = 0; i < n; ++i) converted[i] = convert(barriers[i]);

    ResourceBarrierBatch batch;
    batch.command_list = { st->id };
    batch.barriers     = converted.data();
    batch.count        = n;
    for (D3D12ContextImpl* ctx : st->contexts) {
        if (!ctx->wants_barrier()) continue;
        D3D12FrameContextImpl fc(*ctx, ctx->current_frame_index(), nullptr, self, st);
        ctx->dispatch_resource_barrier(fc, batch);
    }
    if (batch.intercept) return;

    bool any_dropped = false;
    for (const auto& b : converted) any_dropped |= b.intercept;
    follow(self, *st, n, barriers, converted.data());
    if (!any_dropped) return orig(self, n, barriers);

    std::vector<D3D12_RESOURCE_BARRIER> kept;
    kept.reserve(n);
    for (UINT i = 0; i < n; ++i)
        if (!converted[i].intercept) kept.push_back(barriers[i]);
    if (!kept.empty()) orig(self, static_cast<UINT>(kept.size()), kept.data());
}

} // namespace

void install_barrier_hooks(ID3D12GraphicsCommandList* list) noexcept {
    s_barrier.install(list, &hook_barrier);
    ID3D12GraphicsCommandList7* l7 = nullptr;
    if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l7))) && l7) {
        s_barrier7.install(l7, &hook_barrier7);
        l7->Release();
    }
}
void uninstall_barrier_hooks() noexcept {
    s_barrier.uninstall();
    s_barrier7.uninstall();
}

} // namespace vangfx::backend::d3d12
