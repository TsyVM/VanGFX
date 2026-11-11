# DXC — DirectX Shader Compiler

**Source:** https://github.com/microsoft/DirectXShaderCompiler  
**License:** University of Illinois Open Source License (NCSA / UIUC)  
**Version used:** Windows SDK 10.0.26100.0 (ships with VS 2022 / Windows 11 SDK)

## What is vendored here

Nothing — DXC ships with the Windows SDK and is found automatically by CMake
via `find_package(WindowsSDK)` or by searching the SDK's `Include/*/um/` and
`Bin/*/x64/` directories.

Required files (from the Windows SDK, not committed here):
- `dxcapi.h`         — public C++ API (IDxcCompiler3, IDxcUtils, …)
- `dxcompiler.dll`   — runtime (loaded via `LoadLibraryW` at runtime in vgxc)
- `dxil.dll`         — DXIL signing (optional; vgxc calls `dxcompiler.dll` only)

## Finding headers at configure time

CMake: `tools/vgxc/CMakeLists.txt` checks `CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION`
to locate the SDK and sets `DXC_INCLUDE_DIR`. The build fails with a clear
message when the SDK is not found.

## If you want a standalone DXC build (not SDK-embedded)

Download a tagged release from https://github.com/microsoft/DirectXShaderCompiler/releases
and copy `inc/dxcapi.h` into this directory. Set `DXC_INCLUDE_DIR` in CMake
to override the SDK path.
