/**
 * src/effect/uniform.cpp — Uniforms: the eight user slots post passes read.
 *
 * The values live on the context; each backend's post-pass pipeline uploads
 * them with the frame values at Present. (Uniforms used to register an on_draw
 * callback capturing `this` and never remove it, so a Uniforms that went out of
 * scope left the context calling into freed memory on every draw.)
 */

#include <vangfx/effect/uniform.hpp>
#include "../core/context_impl.hpp"

namespace vangfx {

namespace {
ContextImpl& impl(Context* c) noexcept { return *static_cast<ContextImpl*>(c); }
}

void Uniforms::set(uint32_t slot, Float4 v) noexcept {
    if (slot >= kUniformSlots) return;
    impl(ctx_).set_uniform(slot, { v.x, v.y, v.z, v.w });
}

void Uniforms::set(uint32_t slot, float x) noexcept {
    Float4 v = get(slot); v.x = x; set(slot, v);
}
void Uniforms::set(uint32_t slot, float x, float y) noexcept {
    Float4 v = get(slot); v.x = x; v.y = y; set(slot, v);
}
void Uniforms::set(uint32_t slot, float x, float y, float z) noexcept {
    Float4 v = get(slot); v.x = x; v.y = y; v.z = z; set(slot, v);
}
void Uniforms::set(uint32_t slot, float x, float y, float z, float w) noexcept {
    set(slot, Float4{ x, y, z, w });
}

Float4 Uniforms::get(uint32_t slot) const noexcept {
    if (slot >= kUniformSlots) return {};
    const auto all = impl(ctx_).uniforms();
    return { all[slot][0], all[slot][1], all[slot][2], all[slot][3] };
}

void Uniforms::reset() noexcept {
    for (uint32_t i = 0; i < kUniformSlots; ++i) impl(ctx_).set_uniform(i, { 0, 0, 0, 0 });
}

} // namespace vangfx
