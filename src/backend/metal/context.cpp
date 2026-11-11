/**
 * src/backend/metal/context.cpp — MetalContextImpl stub + factory.
 *
 * Phase 7 stub: all operations return Unsupported.
 * Full implementation requires macOS + Xcode 15+ (Metal 3/4 present paths).
 */

#include "metal_impl.hpp"
#include <vangfx/core/shader.hpp>
#include <vangfx/core/error.hpp>

namespace vangfx::backend::metal {

MetalContextImpl::MetalContextImpl(const ContextBuilder& b, void* device)
    : ContextImpl(b), metal_device_(device) {}

MetalContextImpl::~MetalContextImpl() = default;

Result<Shader> MetalContextImpl::compile_shader(const ShaderBuilder&) {
    return gfx_err<Shader>(ShaderError::Unsupported,
        "Metal backend: compile_shader not yet implemented (requires macOS + Metal SDK)");
}

Result<Effect> MetalContextImpl::link_effect(Shader, Shader, Shader, Shader, Shader) {
    return gfx_err<Effect>(ShaderError::Unsupported,
        "Metal backend: link_effect not yet implemented (requires macOS + Metal SDK)");
}

Result<Effect> MetalContextImpl::link_compute_effect(Shader) {
    return gfx_err<Effect>(ShaderError::Unsupported,
        "Metal backend: link_compute_effect not yet implemented (requires macOS + Metal SDK)");
}

void MetalContextImpl::destroy(ShaderHandle h) noexcept { ContextImpl::destroy(h); }
void MetalContextImpl::destroy(EffectHandle h) noexcept { ContextImpl::destroy(h); }

} // namespace vangfx::backend::metal

// ── Factory ───────────────────────────────────────────────────────────────────

namespace vangfx::backend {

using namespace vangfx::backend::metal;

Result<std::unique_ptr<Context>> make_metal_context(const ContextBuilder& b) {
    return std::make_unique<MetalContextImpl>(b, b.metal_device());
}

} // namespace vangfx::backend
