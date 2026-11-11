#pragma once

/**
 * tests/fake_runtime.hpp — a stand-in for a graphics runtime DLL, for the hook
 * tests: a class whose vtable lives in another module (vgx_fake_runtime.dll),
 * as a COM class's lives in d3d11.dll, plus a second function of that module
 * with the same signature, for the runtime swapping its own entries.
 */

#ifdef VGX_FAKE_RUNTIME_BUILD
#  define VGX_FAKE_API extern "C" __declspec(dllexport)
#else
#  define VGX_FAKE_API extern "C" __declspec(dllimport)
#endif

// __stdcall as COM methods are (STDMETHODCALLTYPE), so a hook written as a
// free function taking `self` first calls and is called the same way in 32-bit
// code; a plain C++ virtual is __thiscall there, with `this` in ECX.
struct IWidget {
    virtual int  __stdcall value(int x) = 0;   // slot 0: x + 1
    virtual void __stdcall destroy()    = 0;   // slot 1
};

VGX_FAKE_API IWidget* vgx_make_widget();
VGX_FAKE_API int __stdcall vgx_alternate_value(IWidget* self, int x);   // x + 2
