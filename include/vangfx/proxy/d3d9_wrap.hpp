#pragma once

/**
 * vangfx/proxy/d3d9_wrap.hpp — full passthrough wrappers for IDirect3D9 and
 * IDirect3DDevice9.
 *
 * These classes forward every vtable method to the wrapped real object.  The
 * translator subclasses these and overrides only the methods it handles; every
 * other method reaches the real driver without an extra dispatch.
 *
 * Both classes register themselves in ComWrapRegistry on construction.
 */

#include <vangfx/proxy/com_wrap.hpp>
#include <d3d9.h>

namespace vangfx::proxy {

// ── IDirect3D9Wrap ────────────────────────────────────────────────────────────

class IDirect3D9Wrap : public ComWrap<IDirect3D9> {
public:
    explicit IDirect3D9Wrap(IDirect3D9* real) : ComWrap(real) {}

    // IUnknown: handled by ComWrap<IDirect3D9>.

    // IDirect3D9
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* p) override               { return real_->RegisterSoftwareDevice(p); }
    UINT    STDMETHODCALLTYPE GetAdapterCount() override                              { return real_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* id) override { return real_->GetAdapterIdentifier(a, f, id); }
    UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT fmt) override     { return real_->GetAdapterModeCount(a, fmt); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT fmt, UINT m, D3DDISPLAYMODE* dm) override { return real_->EnumAdapterModes(a, fmt, m, dm); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* dm) override { return real_->GetAdapterDisplayMode(a, dm); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE dt, D3DFORMAT df, D3DFORMAT bf, BOOL ww) override { return real_->CheckDeviceType(a, dt, df, bf, ww); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE dt, D3DFORMAT af, DWORD u, D3DRESOURCETYPE rt, D3DFORMAT cf) override { return real_->CheckDeviceFormat(a, dt, af, u, rt, cf); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE dt, D3DFORMAT sf, BOOL ww, D3DMULTISAMPLE_TYPE ms, DWORD* ql) override { return real_->CheckDeviceMultiSampleType(a, dt, sf, ww, ms, ql); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE dt, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) override { return real_->CheckDepthStencilMatch(a, dt, af, rf, df); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE dt, D3DFORMAT sf, D3DFORMAT df) override { return real_->CheckDeviceFormatConversion(a, dt, sf, df); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE dt, D3DCAPS9* caps) override { return real_->GetDeviceCaps(a, dt, caps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override                    { return real_->GetAdapterMonitor(a); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE dt, HWND hw, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev) override { return real_->CreateDevice(a, dt, hw, flags, pp, dev); }

protected:
    ~IDirect3D9Wrap() = default;
};

// ── IDirect3DDevice9Wrap ──────────────────────────────────────────────────────

class IDirect3DDevice9Wrap : public ComWrap<IDirect3DDevice9> {
public:
    explicit IDirect3DDevice9Wrap(IDirect3DDevice9* real) : ComWrap(real) {}

    // IUnknown: handled by ComWrap<IDirect3DDevice9>.

    // IDirect3DDevice9 — all 119 methods forwarded
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override                        { return real_->TestCooperativeLevel(); }
    UINT    STDMETHODCALLTYPE GetAvailableTextureMem() override                      { return real_->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override                       { return real_->EvictManagedResources(); }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** d3d) override                 { return real_->GetDirect3D(d3d); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* caps) override                 { return real_->GetDeviceCaps(caps); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT sc, D3DDISPLAYMODE* dm) override   { return real_->GetDisplayMode(sc, dm); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override { return real_->GetCreationParameters(p); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, IDirect3DSurface9* s) override { return real_->SetCursorProperties(x, y, s); }
    void    STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD f) override      { real_->SetCursorPosition(x, y, f); }
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL show) override                         { return real_->ShowCursor(show); }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pp, IDirect3DSwapChain9** sc) override { return real_->CreateAdditionalSwapChain(pp, sc); }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT idx, IDirect3DSwapChain9** sc) override { return real_->GetSwapChain(idx, sc); }
    UINT    STDMETHODCALLTYPE GetNumberOfSwapChains() override                       { return real_->GetNumberOfSwapChains(); }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pp) override              { return real_->Reset(pp); }
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND hw, const RGNDATA* rd) override { return real_->Present(src, dst, hw, rd); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT idx, D3DBACKBUFFER_TYPE bt, IDirect3DSurface9** surf) override { return real_->GetBackBuffer(sc, idx, bt, surf); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT sc, D3DRASTER_STATUS* rs) override { return real_->GetRasterStatus(sc, rs); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL enable) override                 { return real_->SetDialogBoxMode(enable); }
    void    STDMETHODCALLTYPE SetGammaRamp(UINT sc, DWORD f, const D3DGAMMARAMP* r) override { real_->SetGammaRamp(sc, f, r); }
    void    STDMETHODCALLTYPE GetGammaRamp(UINT sc, D3DGAMMARAMP* r) override        { real_->GetGammaRamp(sc, r); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT lvl, DWORD u, D3DFORMAT fmt, D3DPOOL p, IDirect3DTexture9** tex, HANDLE* sh) override { return real_->CreateTexture(w, h, lvl, u, fmt, p, tex, sh); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT lvl, DWORD u, D3DFORMAT fmt, D3DPOOL p, IDirect3DVolumeTexture9** tex, HANDLE* sh) override { return real_->CreateVolumeTexture(w, h, d, lvl, u, fmt, p, tex, sh); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT size, UINT lvl, DWORD u, D3DFORMAT fmt, D3DPOOL p, IDirect3DCubeTexture9** tex, HANDLE* sh) override { return real_->CreateCubeTexture(size, lvl, u, fmt, p, tex, sh); }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** vb, HANDLE* sh) override { return real_->CreateVertexBuffer(len, u, fvf, p, vb, sh); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD u, D3DFORMAT fmt, D3DPOOL p, IDirect3DIndexBuffer9** ib, HANDLE* sh) override { return real_->CreateIndexBuffer(len, u, fmt, p, ib, sh); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD mq, BOOL l, IDirect3DSurface9** surf, HANDLE* sh) override { return real_->CreateRenderTarget(w, h, fmt, ms, mq, l, surf, sh); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD mq, BOOL dis, IDirect3DSurface9** surf, HANDLE* sh) override { return real_->CreateDepthStencilSurface(w, h, fmt, ms, mq, dis, surf, sh); }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* d, const POINT* dp) override { return real_->UpdateSurface(s, sr, d, dp); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* s, IDirect3DBaseTexture9* d) override { return real_->UpdateTexture(s, d); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* rt, IDirect3DSurface9* dst) override { return real_->GetRenderTargetData(rt, dst); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT sc, IDirect3DSurface9* dst) override { return real_->GetFrontBufferData(sc, dst); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* d, const RECT* dr, D3DTEXTUREFILTERTYPE f) override { return real_->StretchRect(s, sr, d, dr, f); }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* surf, const RECT* r, D3DCOLOR c) override { return real_->ColorFill(surf, r, c); }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL p, IDirect3DSurface9** surf, HANDLE* sh) override { return real_->CreateOffscreenPlainSurface(w, h, fmt, p, surf, sh); }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD idx, IDirect3DSurface9* rt) override { return real_->SetRenderTarget(idx, rt); }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD idx, IDirect3DSurface9** rt) override { return real_->GetRenderTarget(idx, rt); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* s) override  { return real_->SetDepthStencilSurface(s); }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** s) override { return real_->GetDepthStencilSurface(s); }
    HRESULT STDMETHODCALLTYPE BeginScene() override                                  { return real_->BeginScene(); }
    HRESULT STDMETHODCALLTYPE EndScene() override                                    { return real_->EndScene(); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD s) override { return real_->Clear(count, r, f, c, z, s); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) override { return real_->SetTransform(st, m); }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE st, D3DMATRIX* m) override { return real_->GetTransform(st, m); }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) override { return real_->MultiplyTransform(st, m); }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* vp) override           { return real_->SetViewport(vp); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* vp) override                 { return real_->GetViewport(vp); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* m) override            { return real_->SetMaterial(m); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* m) override                  { return real_->GetMaterial(m); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD idx, const D3DLIGHT9* l) override       { return real_->SetLight(idx, l); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD idx, D3DLIGHT9* l) override             { return real_->GetLight(idx, l); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD idx, BOOL en) override               { return real_->LightEnable(idx, en); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD idx, BOOL* en) override           { return real_->GetLightEnable(idx, en); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD idx, const float* p) override       { return real_->SetClipPlane(idx, p); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD idx, float* p) override             { return real_->GetClipPlane(idx, p); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE st, DWORD v) override { return real_->SetRenderState(st, v); }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE st, DWORD* v) override { return real_->GetRenderState(st, v); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** sb) override { return real_->CreateStateBlock(t, sb); }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override                             { return real_->BeginStateBlock(); }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** sb) override      { return real_->EndStateBlock(sb); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* s) override        { return real_->SetClipStatus(s); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* s) override              { return real_->GetClipStatus(s); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, IDirect3DBaseTexture9** tex) override { return real_->GetTexture(stage, tex); }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* tex) override  { return real_->SetTexture(stage, tex); }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override { return real_->GetTextureStageState(st, t, v); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) override  { return real_->SetTextureStageState(st, t, v); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) override { return real_->GetSamplerState(s, t, v); }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override  { return real_->SetSamplerState(s, t, v); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* np) override                     { return real_->ValidateDevice(np); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT n, const PALETTEENTRY* p) override { return real_->SetPaletteEntries(n, p); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT n, PALETTEENTRY* p) override    { return real_->GetPaletteEntries(n, p); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT n) override              { return real_->SetCurrentTexturePalette(n); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* n) override             { return real_->GetCurrentTexturePalette(n); }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* r) override                 { return real_->SetScissorRect(r); }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* r) override                       { return real_->GetScissorRect(r); }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL sw) override          { return real_->SetSoftwareVertexProcessing(sw); }
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override                 { return real_->GetSoftwareVertexProcessing(); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float v) override                        { return real_->SetNPatchMode(v); }
    float   STDMETHODCALLTYPE GetNPatchMode() override                               { return real_->GetNPatchMode(); }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE pt, UINT sv, UINT pc) override { return real_->DrawPrimitive(pt, sv, pc); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE pt, INT bv, UINT mv, UINT vc, UINT si, UINT pc) override { return real_->DrawIndexedPrimitive(pt, bv, mv, vc, si, pc); }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE pt, UINT pc, const void* vd, UINT vs) override { return real_->DrawPrimitiveUP(pt, pc, vd, vs); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE pt, UINT mv, UINT vc, UINT pc, const void* id, D3DFORMAT ifmt, const void* vd, UINT vs) override { return real_->DrawIndexedPrimitiveUP(pt, mv, vc, pc, id, ifmt, vd, vs); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT ssv, UINT dv, UINT vc, IDirect3DVertexBuffer9* vb, IDirect3DVertexDeclaration9* vd, DWORD f) override { return real_->ProcessVertices(ssv, dv, vc, vb, vd, f); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* elems, IDirect3DVertexDeclaration9** vd) override { return real_->CreateVertexDeclaration(elems, vd); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* vd) override { return real_->SetVertexDeclaration(vd); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** vd) override { return real_->GetVertexDeclaration(vd); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override                             { return real_->SetFVF(fvf); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* fvf) override                            { return real_->GetFVF(fvf); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* fn, IDirect3DVertexShader9** vs) override { return real_->CreateVertexShader(fn, vs); }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* vs) override   { return real_->SetVertexShader(vs); }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** vs) override  { return real_->GetVertexShader(vs); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT reg, const float* d, UINT cnt) override { return real_->SetVertexShaderConstantF(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT reg, float* d, UINT cnt) override       { return real_->GetVertexShaderConstantF(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT reg, const int* d, UINT cnt) override   { return real_->SetVertexShaderConstantI(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT reg, int* d, UINT cnt) override         { return real_->GetVertexShaderConstantI(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT reg, const BOOL* d, UINT cnt) override  { return real_->SetVertexShaderConstantB(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT reg, BOOL* d, UINT cnt) override        { return real_->GetVertexShaderConstantB(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT s, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) override { return real_->SetStreamSource(s, vb, off, stride); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT s, IDirect3DVertexBuffer9** vb, UINT* off, UINT* stride) override { return real_->GetStreamSource(s, vb, off, stride); }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT s, UINT d) override           { return real_->SetStreamSourceFreq(s, d); }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT s, UINT* d) override          { return real_->GetStreamSourceFreq(s, d); }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* ib) override         { return real_->SetIndices(ib); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ib) override        { return real_->GetIndices(ib); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* fn, IDirect3DPixelShader9** ps) override { return real_->CreatePixelShader(fn, ps); }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* ps) override     { return real_->SetPixelShader(ps); }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ps) override    { return real_->GetPixelShader(ps); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT reg, const float* d, UINT cnt) override  { return real_->SetPixelShaderConstantF(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT reg, float* d, UINT cnt) override        { return real_->GetPixelShaderConstantF(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT reg, const int* d, UINT cnt) override    { return real_->SetPixelShaderConstantI(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT reg, int* d, UINT cnt) override          { return real_->GetPixelShaderConstantI(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT reg, const BOOL* d, UINT cnt) override   { return real_->SetPixelShaderConstantB(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT reg, BOOL* d, UINT cnt) override         { return real_->GetPixelShaderConstantB(reg, d, cnt); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT h, const float* nc, const D3DRECTPATCH_INFO* info) override { return real_->DrawRectPatch(h, nc, info); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT h, const float* nc, const D3DTRIPATCH_INFO* info) override   { return real_->DrawTriPatch(h, nc, info); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT h) override                           { return real_->DeletePatch(h); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** q) override { return real_->CreateQuery(t, q); }

protected:
    ~IDirect3DDevice9Wrap() = default;
};

} // namespace vangfx::proxy
