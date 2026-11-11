#include <vangfx/core/content_hash.hpp>

#include <format>

namespace vangfx {

std::string content_hash_hex(uint64_t h) {
    return std::format("{:016x}", h);
}

std::string shader_filename(std::string_view stage,
                             uint64_t         hash,
                             std::string_view ext) {
    return std::format("{}_{}{}",
                       stage,
                       content_hash_hex(hash),
                       ext);
}

std::string ffp_shader_filename(std::string_view stage,
                                 uint64_t         perm_key_hash,
                                 std::string_view ext) {
    return std::format("ffp{}_{}{}",
                       stage,
                       content_hash_hex(perm_key_hash),
                       ext);
}

std::string texture_filename(uint32_t         width,
                              uint32_t         height,
                              std::string_view fmt,
                              uint64_t         hash) {
    return std::format("{}x{}_{}_{}.dds",
                       width, height, fmt,
                       content_hash_hex(hash));
}

} // namespace vangfx
