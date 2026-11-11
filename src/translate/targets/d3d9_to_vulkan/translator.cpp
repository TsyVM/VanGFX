/**
 * src/translate/targets/d3d9_to_vulkan/translator.cpp — stub implementation.
 *
 * Returns empty vectors until DXC is wired into the CMake build.
 * To enable full translation: add DXC to external/dxc and implement
 * the DXC invocation path here.
 */

#include "translator.hpp"

namespace vangfx::translate::d3d9_to_vulkan {

std::vector<uint32_t> translate_dxbc_to_spirv(const uint32_t* /*dxbc*/,
                                               size_t          /*count*/,
                                               bool            /*is_ps*/) {
    // TODO: implement via DXC --spirv when external/dxc is available.
    return {};
}

std::vector<uint32_t> translate_hlsl_to_spirv(const char* /*hlsl*/,
                                               const char* /*entry*/,
                                               bool        /*is_ps*/) {
    // TODO: implement via DXC --spirv when external/dxc is available.
    return {};
}

} // namespace vangfx::translate::d3d9_to_vulkan
