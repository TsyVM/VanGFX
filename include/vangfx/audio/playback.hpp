#pragma once
#include <cstdint>

namespace vangfx {

struct SoundHandle {
    uint64_t id = 0;
    constexpr bool valid()                         const noexcept { return id != 0; }
    constexpr bool operator==(const SoundHandle&)  const noexcept = default;
};

} // namespace vangfx
