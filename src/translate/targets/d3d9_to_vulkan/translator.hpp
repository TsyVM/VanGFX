#pragma once

/**
 * src/translate/targets/d3d9_to_vulkan/translator.hpp
 *
 * D3D9 → Vulkan translation layer skeleton.
 *
 * Phase 8 skeleton: provides the interface for translating DXBC (D3D9 shader
 * bytecode) to SPIR-V via DXC (DirectX Shader Compiler with --spirv).
 *
 * This file establishes the translation pipeline contract.  The
 * implementation in translator.cpp is a no-op stub until DXC is available
 * in external/dxc (CMake target: dxcompiler).
 *
 * Full implementation plan:
 *   1. translate_dxbc_to_spirv: invoke DXC with -T vs_5_0 -spirv on the
 *      disassembled D3D9 bytecode (requires DXC SDK).
 *   2. D3D9SurfaceTracker: wraps vkSwapchainKHR and hooks
 *      IDirect3DDevice9::Present to submit the Vulkan command buffer.
 *   3. VulkanPassBridge: creates a VkRenderPass/VkFramebuffer pair backed
 *      by the game's swap-chain image; maps to gpu::Pass.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace vangfx::translate::d3d9_to_vulkan {

/**
 * Translate DXBC (D3D9 shader bytecode) to SPIR-V.
 *
 * Returns the SPIR-V uint32_t word stream on success, or empty on failure.
 *
 * stub: always returns empty until DXC is available.
 */
std::vector<uint32_t> translate_dxbc_to_spirv(const uint32_t* dxbc_tokens,
                                               size_t          token_count,
                                               bool            is_pixel_shader);

/**
 * Translate HLSL source to SPIR-V via DXC --spirv.
 *
 * Returns the SPIR-V uint32_t word stream on success, or empty on failure.
 *
 * stub: always returns empty until DXC is available.
 */
std::vector<uint32_t> translate_hlsl_to_spirv(const char* hlsl_source,
                                               const char* entry_point,
                                               bool        is_pixel_shader);

} // namespace vangfx::translate::d3d9_to_vulkan
