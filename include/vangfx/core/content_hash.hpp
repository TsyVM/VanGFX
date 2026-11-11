#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>
#include <string>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  ContentHash v1  — FNV-1a with a final avalanche mix
//
//  This is bit-exact with MWOn12's ShaderMods.cpp HashBytes().
//  The mix matters: raw FNV-1a has weak diffusion in the high bits, and these
//  values are printed as filenames (ps_<16hex>.hlsl) where close hashes would
//  produce names that differ by one character.
//
//  Do NOT change this algorithm.  Every existing shader-kit file depends on it.
//  A new hash algorithm belongs in a future ContentHash v2 with a different
//  name prefix.
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] constexpr uint64_t content_hash(const void* data,
                                               size_t      len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

[[nodiscard]] inline uint64_t content_hash(std::span<const std::byte> data) noexcept {
    return content_hash(data.data(), data.size());
}

[[nodiscard]] inline uint64_t content_hash(std::string_view s) noexcept {
    return content_hash(s.data(), s.size());
}

// Format a hash as the 16-character lowercase hex used in file names.
[[nodiscard]] std::string content_hash_hex(uint64_t h);

// ── File name helpers ─────────────────────────────────────────────────────────
// These produce the canonical names the shader kit uses.

// "vs_<16hex>.hlsl", "ps_<16hex>.hlsl", etc.
[[nodiscard]] std::string shader_filename(std::string_view stage,
                                          uint64_t         hash,
                                          std::string_view ext = ".hlsl");

// "ffpvs_<16hex>.hlsl" / "ffpps_<16hex>.hlsl"  for fixed-function shaders.
[[nodiscard]] std::string ffp_shader_filename(std::string_view stage,
                                              uint64_t         perm_key_hash,
                                              std::string_view ext = ".hlsl");

// "<w>x<h>_<fmt>_<16hex>.dds"
[[nodiscard]] std::string texture_filename(uint32_t         width,
                                           uint32_t         height,
                                           std::string_view fmt,
                                           uint64_t         hash);

} // namespace vangfx
