#pragma once

/**
 * src/translate/targets/d3d8_to_d3d9/translator.hpp
 *
 * D3D8 → D3D9 translation layer.
 *
 * Presents an IDirect3DDevice8-compatible vtable in front of a real
 * IDirect3DDevice9.  Game code targeting D3D8 calls through this shim,
 * which forwards to D3D9 with the necessary adjustments:
 *
 *  - DrawPrimitive / DrawIndexedPrimitive → D3D9 equivalents (unchanged ABI)
 *  - Present → IDirect3DDevice9::Present (extra HWND arg dropped)
 *  - CreateVertexShader / CreatePixelShader:
 *      DX8 assembly bytecode → DX9 bytecode via d3d8_shader_translate()
 *  - SetVertexShader / SetPixelShader:
 *      DWORD handle → IDirect3DVertexShader9* / IDirect3DPixelShader9*
 *
 * This shim does NOT implement the full D3D8 surface; it covers the subset
 * that the VanGFX D3D8 backend hooks (draw, present, shader set/create).
 * A complete D3D8→D3D9 wrapper would require the DirectX 8.1 SDK headers
 * and is out of scope for the current release.
 *
 * Usage (from code that already has an IDirect3DDevice9*):
 *
 *   auto translator = vangfx::translate::d3d8_to_d3d9::create(d3d9_device);
 *   // translator->device8() returns the fake IDirect3DDevice8* shim.
 *
 * The returned translator owns the shim object lifetime.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>

#include <memory>
#include <vector>
#include <cstdint>

// Forward-declare opaque D3D8 types (no d3d8.h required).
struct IDirect3DDevice8;

namespace vangfx::translate::d3d8_to_d3d9 {

/**
 * Translate a DX8 vertex/pixel shader token stream to DX9 bytecode.
 *
 * DX8 and DX9 shader bytecode are very similar; the main differences are:
 *   - DX8 vs_1_1/ps_1_x use different version tokens (0xFFFF0101 vs 0xFFFE0101)
 *   - DX8 ps_1_x texld instructions map to DX9 tex* instructions
 *   - DX8 input declarations lack D3D9's dcl_ prefix
 *
 * Returns the translated bytecode on success, or an empty vector on failure.
 * The input tokens are the DWORD stream that follows Direct3DDevice8::
 * CreateVertexShader() / CreatePixelShader().
 */
std::vector<uint32_t> translate_shader(const uint32_t* tokens,
                                        size_t          token_count,
                                        bool            is_pixel_shader);

// ── Shim device ───────────────────────────────────────────────────────────────

/**
 * D3D8DeviceShim — presents a fake IDirect3DDevice8* vtable backed by D3D9.
 *
 * Only the slots hooked by VanGFX are translated; all others forward verbatim
 * through the same slot on the wrapped D3D9 device.
 */
class D3D8DeviceShim {
public:
    explicit D3D8DeviceShim(IDirect3DDevice9* d3d9);
    ~D3D8DeviceShim();

    // Returns the fake IDirect3DDevice8* pointer for use with the D3D8 backend.
    IDirect3DDevice8* device8() const noexcept;

    IDirect3DDevice9* d3d9() const noexcept { return d3d9_; }

    // The shim object layout: vtbl pointer first (COM convention), then a back-
    // pointer to the owning D3D8DeviceShim so hook stubs can recover d3d9_.
    // Public so translator.cpp can cast the fake IDirect3DDevice8* back to it.
    struct ShimObj {
        void**           vtbl  = nullptr; // must be first
        D3D8DeviceShim*  owner = nullptr;
    };

private:
    IDirect3DDevice9*  d3d9_ = nullptr;
    ShimObj            shim_obj_;
    void*              vtbl_[97]{};  // d3d8_slots::kSlotCount = 97
};

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<D3D8DeviceShim> create(IDirect3DDevice9* d3d9_device);

} // namespace vangfx::translate::d3d8_to_d3d9
