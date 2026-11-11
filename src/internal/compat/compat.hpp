#pragma once

/**
 * vangfx/compat/compat.hpp — Single include for the VanGFX COM / vtable layer.
 *
 * Include this header (or individual sub-headers) in VanGFX runtime internals
 * and in advanced mod code that needs direct access to COM pointers or vtable
 * slots.  Ordinary mod authors should use vangfx/vangfx.hpp instead; the
 * compat layer is an implementation detail.
 *
 * Sub-headers:
 *   vtable.hpp      — vtable patching (patch / restore / ScopedPatch)
 *   com.hpp         — COM smart pointer (Ptr<T>), qi<T>(), hr_ok(), etc.
 *   d3d9_slots.hpp  — IDirect3DDevice9 vtable slot indices
 *   d3d11_slots.hpp — ID3D11DeviceContext / IDXGISwapChain slot indices
 *   d3d12_slots.hpp — ID3D12Device / CommandQueue / GraphicsCommandList slots
 */

#include "com.hpp"
#include "vtable.hpp"
#include "d3d9_slots.hpp"
#include "d3d11_slots.hpp"
#include "d3d12_slots.hpp"
