#pragma once

/**
 * compat/dxgi_slots.hpp — vtable slot indices for the DXGI factory.
 *
 * IUnknown(0-2) → IDXGIObject(3-6) → IDXGIFactory(7-11) → IDXGIFactory1(12-13)
 * → IDXGIFactory2(14-24) → ... Every factory a process creates shares one class,
 * whichever version was asked for. Checked against IDXGIFactory7Vtbl by
 * slot_checks.c (tools/gen_slot_checks.py).
 */

#include <cstdint>

namespace vangfx::dxgi_slots {

namespace factory {
    // IDXGIFactory
    inline constexpr uint32_t EnumAdapters                  =  7;
    inline constexpr uint32_t MakeWindowAssociation         =  8;
    inline constexpr uint32_t GetWindowAssociation          =  9;
    inline constexpr uint32_t CreateSwapChain               = 10;   // ← swap chain creation hook
    inline constexpr uint32_t CreateSoftwareAdapter         = 11;

    inline constexpr uint32_t kSlotCount                    = 12;

    // IDXGIFactory1
    inline constexpr uint32_t EnumAdapters1                 = 12;
    inline constexpr uint32_t IsCurrent                     = 13;

    // IDXGIFactory2
    inline constexpr uint32_t IsWindowedStereoEnabled       = 14;
    inline constexpr uint32_t CreateSwapChainForHwnd        = 15;   // ← swap chain creation hook
    inline constexpr uint32_t CreateSwapChainForCoreWindow  = 16;   // ← swap chain creation hook
    inline constexpr uint32_t GetSharedResourceAdapterLuid  = 17;
    inline constexpr uint32_t CreateSwapChainForComposition = 24;   // ← swap chain creation hook
} // namespace factory

} // namespace vangfx::dxgi_slots
