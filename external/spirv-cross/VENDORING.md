# SPIRV-Cross

**Source:** https://github.com/KhronosGroup/SPIRV-Cross  
**License:** Apache-2.0  
**Used for:** SPIR-V → GLSL / ESSL / MSL cross-compilation in `vgxc`

## Status

Source not yet vendored.  GLSL, ESSL and MSL targets in `vgxc --targets` will
skip gracefully with a warning when this is absent.

Enable by:
1. Cloning `https://github.com/KhronosGroup/SPIRV-Cross` and copying the C API
   header + implementation files into this directory:
   - `spirv_cross_c.h`
   - `spirv_cross_c.cpp`
   - `spirv_cross.hpp` / `.cpp`
   - `spirv_glsl.hpp` / `.cpp`
   - `spirv_msl.hpp` / `.cpp`
   - `spirv_hlsl.hpp` / `.cpp`
   - `spirv_parser.hpp` / `.cpp`
   - `spirv_cfg.hpp` / `.cpp`
   - `spirv.h`, `GLSL.std.450.h`
2. Set CMake option `-DVANGFX_SPIRV_CROSS=ON`.

The C API (`spirv_cross_c.h`) is the stable surface; `vgxc` uses it exclusively
so the internal implementation can be replaced with a newer version without
touching `vgxc` source.
