#pragma once

/**
 * src/internal/hash.hpp — 64-bit FNV-1a, the hash VanGFX names shaders by.
 *
 * DrawEvent::vs_hash / ps_hash and the draw census use it. FNV-1a is not
 * cryptographic and does not need to be: it only has to give the same shader
 * the same number on every run, and two different shaders different numbers
 * with overwhelming likelihood, cheaply enough to run on every shader a game
 * creates.
 */

#include <cstddef>
#include <cstdint>

namespace vangfx {

[[nodiscard]] inline uint64_t hash_bytes(const void* data, size_t size) noexcept {
    if (!data || !size) return 0;
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1;   // 0 means "no shader"
}

} // namespace vangfx
