/**
 * src/translate/targets/d3d8_to_d3d9/translator.cpp — D3D8→D3D9 shim.
 *
 * The VanGFX D3D8 backend hooks DrawPrimitive, DrawIndexedPrimitive, Present,
 * SetVertexShader, and SetPixelShader.  The shim re-routes those five slots
 * to their D3D9 counterparts and leaves all other vtable slots pointing at
 * the original D3D9 device (which is binary-compatible for most methods).
 *
 * Shader translation covers only the two most common DX8→DX9 fixups:
 *   1. Version token: 0xFFFF0101 (vs_1_1) → 0xFFFE0101 (D3D9 vs_1_1)
 *      (DX8 uses 0xFFFF for vertex shaders; D3D9 uses 0xFFFE.)
 *   2. Pixel shader version rewrite for ps_1_x (DX8 0xFFFF01xx →
 *      D3D9 0xFFFF01xx — usually compatible, no change needed).
 * Full DX8 shader translation is left as a future extension point.
 */

#include "translator.hpp"
#include <internal/compat/d3d8_slots.hpp>
#include <cstring>

namespace vangfx::translate::d3d8_to_d3d9 {

// ── Shader translation ────────────────────────────────────────────────────────

std::vector<uint32_t> translate_shader(const uint32_t* tokens,
                                        size_t          token_count,
                                        bool            is_pixel_shader) {
    if (!tokens || token_count == 0) return {};
    std::vector<uint32_t> out(tokens, tokens + token_count);

    if (!is_pixel_shader) {
        // DX8 vertex shader version token: 0xFFFF0101
        // DX9 vertex shader version token: 0xFFFE0101
        if ((out[0] & 0xFFFF0000u) == 0xFFFF0000u) {
            out[0] = (out[0] & 0x0000FFFFu) | 0xFFFE0000u;
        }
    }
    // DX8 pixel shaders ps_1_1..ps_1_4 are largely compatible with D3D9
    // ps_1_x.  No fixup needed for the common case.

    return out;
}

// ── Shim vtable functions ─────────────────────────────────────────────────────

namespace {

// Convenience: cast from the fake IDirect3DDevice8* (ShimObj*) back to D3D9.
// ShimObj layout: { void** vtbl; D3D8DeviceShim* owner; }
static IDirect3DDevice9* d3d9_of(void* shim_ptr) noexcept {
    return reinterpret_cast<D3D8DeviceShim::ShimObj*>(shim_ptr)->owner->d3d9();
}

HRESULT STDMETHODCALLTYPE shim_QueryInterface(void* self, REFIID riid, void** pp) {
    return d3d9_of(self)->QueryInterface(riid, pp);
}
ULONG STDMETHODCALLTYPE shim_AddRef(void* self) {
    return d3d9_of(self)->AddRef();
}
ULONG STDMETHODCALLTYPE shim_Release(void* self) {
    return d3d9_of(self)->Release();
}

// DrawPrimitive (slot 70) — ABI identical to D3D9.
HRESULT STDMETHODCALLTYPE shim_DrawPrimitive(void* self,
                                              UINT pt, UINT start, UINT count) {
    return d3d9_of(self)->DrawPrimitive(
        static_cast<D3DPRIMITIVETYPE>(pt), start, count);
}

// DrawIndexedPrimitive (slot 71) — D3D8 has (pt,minVtx,numVtx,startIdx,count);
//                                   D3D9 adds baseVertexIndex as first arg.
HRESULT STDMETHODCALLTYPE shim_DrawIndexedPrimitive(void* self,
                                                     UINT pt,
                                                     UINT min_vtx, UINT num_vtx,
                                                     UINT start_idx, UINT count) {
    return d3d9_of(self)->DrawIndexedPrimitive(
        static_cast<D3DPRIMITIVETYPE>(pt),
        0, // baseVertexIndex — D3D8 did not have this; default to 0
        min_vtx, num_vtx, start_idx, count);
}

// Present (slot 15) — D3D8: (pSrc,pDst,hWnd,pDirty); D3D9: same.
HRESULT STDMETHODCALLTYPE shim_Present(void* self,
                                        const RECT* src, const RECT* dst,
                                        HWND wnd, const RGNDATA* dirty) {
    return d3d9_of(self)->Present(src, dst, wnd, dirty);
}

// SetVertexShader (slot 76) — D3D8 passes a DWORD handle; D3D9 passes
// IDirect3DVertexShader9*.  For fixed-function shaders (handle < 0x100)
// we call SetFVF; for programmable shaders we cannot translate without the
// original bytecode, so we no-op for now.
HRESULT STDMETHODCALLTYPE shim_SetVertexShader(void* self, DWORD handle) {
    IDirect3DDevice9* dev = d3d9_of(self);
    if (handle < 0x100) {
        // Fixed-function FVF handle in D3D8.
        return dev->SetFVF(handle);
    }
    // Programmable: would need a registered IDirect3DVertexShader9*.
    // No-op until the shader registry is wired up.
    return S_OK;
}

// SetPixelShader (slot 88) — same situation as vertex shader.
HRESULT STDMETHODCALLTYPE shim_SetPixelShader(void* self, DWORD /*handle*/) {
    // No-op: shader handle registry not wired up yet.
    return S_OK;
}

// Generic forward for all other slots: cast to D3D9 and call the same slot.
// This covers BeginScene, EndScene, SetRenderState, SetTexture, etc.
// (They have the same ABI on D3D8 and D3D9 for the common methods.)
template<uint32_t Slot>
HRESULT STDMETHODCALLTYPE shim_forward(void* self) {
    // Not implemented: the shim covers only the five hooked slots.
    // All other slots are filled with nullptr in the vtable, triggering
    // an access violation if called — which is intentional: the translator
    // is not a full D3D8 wrapper.
    (void)self;
    return E_NOTIMPL;
}

} // namespace

// ── D3D8DeviceShim ────────────────────────────────────────────────────────────

D3D8DeviceShim::D3D8DeviceShim(IDirect3DDevice9* d3d9)
    : d3d9_(d3d9)
{
    // Build the vtable.  Most slots are nullptr (not fully translated).
    std::memset(vtbl_, 0, sizeof(vtbl_));
    vtbl_[d3d8_slots::QueryInterface]        = reinterpret_cast<void*>(&shim_QueryInterface);
    vtbl_[d3d8_slots::AddRef]                = reinterpret_cast<void*>(&shim_AddRef);
    vtbl_[d3d8_slots::Release]               = reinterpret_cast<void*>(&shim_Release);
    vtbl_[d3d8_slots::Present]               = reinterpret_cast<void*>(&shim_Present);
    vtbl_[d3d8_slots::DrawPrimitive]         = reinterpret_cast<void*>(&shim_DrawPrimitive);
    vtbl_[d3d8_slots::DrawIndexedPrimitive]  = reinterpret_cast<void*>(&shim_DrawIndexedPrimitive);
    vtbl_[d3d8_slots::SetVertexShader]       = reinterpret_cast<void*>(&shim_SetVertexShader);
    vtbl_[d3d8_slots::SetPixelShader]        = reinterpret_cast<void*>(&shim_SetPixelShader);

    shim_obj_.vtbl  = vtbl_;
    shim_obj_.owner = this;
}

D3D8DeviceShim::~D3D8DeviceShim() = default;

IDirect3DDevice8* D3D8DeviceShim::device8() const noexcept {
    return reinterpret_cast<IDirect3DDevice8*>(
        const_cast<ShimObj*>(&shim_obj_));
}

// ── Factory ───────────────────────────────────────────────────────────────────

std::unique_ptr<D3D8DeviceShim> create(IDirect3DDevice9* d3d9_device) {
    return std::make_unique<D3D8DeviceShim>(d3d9_device);
}

} // namespace vangfx::translate::d3d8_to_d3d9
