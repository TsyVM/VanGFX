/**
 * src/proxy/com_wrap_registry.cpp — global wrap/unwrap table
 *
 * Two maps maintained together under one mutex:
 *   wrapped → real   (the primary direction: unwrap)
 *   real    → wrapped (the reverse direction: re-use existing wrappers)
 */

#include <vangfx/proxy/com_wrap.hpp>

#include <mutex>
#include <unordered_map>

namespace vangfx::proxy {

namespace {

struct Registry {
    std::mutex                             mtx;
    std::unordered_map<IUnknown*, IUnknown*> wrapped_to_real;
    std::unordered_map<IUnknown*, IUnknown*> real_to_wrapped;
};

Registry& reg() {
    static Registry r;
    return r;
}

} // namespace

void ComWrapRegistry::register_wrap(IUnknown* wrapped, IUnknown* real) {
    if (!wrapped || !real) return;
    auto& r = reg();
    std::lock_guard lock(r.mtx);
    r.wrapped_to_real[wrapped] = real;
    r.real_to_wrapped[real]    = wrapped;
}

void ComWrapRegistry::unregister_wrap(IUnknown* wrapped) {
    if (!wrapped) return;
    auto& r = reg();
    std::lock_guard lock(r.mtx);
    auto it = r.wrapped_to_real.find(wrapped);
    if (it != r.wrapped_to_real.end()) {
        r.real_to_wrapped.erase(it->second);
        r.wrapped_to_real.erase(it);
    }
}

IUnknown* ComWrapRegistry::real_of(IUnknown* wrapped) {
    if (!wrapped) return nullptr;
    auto& r = reg();
    std::lock_guard lock(r.mtx);
    auto it = r.wrapped_to_real.find(wrapped);
    return it != r.wrapped_to_real.end() ? it->second : nullptr;
}

IUnknown* ComWrapRegistry::wrap_of(IUnknown* real) {
    if (!real) return nullptr;
    auto& r = reg();
    std::lock_guard lock(r.mtx);
    auto it = r.real_to_wrapped.find(real);
    return it != r.real_to_wrapped.end() ? it->second : nullptr;
}

} // namespace vangfx::proxy
