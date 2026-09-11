# VanGFX 2026 Release — build record

Public headers and precompiled libraries. Nothing in this folder is generated at
consume time: copy it next to your project and link.

## Toolchain

| | |
|---|---|
| Compiler | MSVC 19.x, Visual Studio 18 (2026) Community, 18.7.11911.148 |
| Generator | Visual Studio 18 2026 |
| CMake | 4.2.2 |
| C++ standard | C++23 (`core/error.hpp` is built on `std::expected`) |
| Date | 2026-09-11 |

Configured from the project's own `CMakeLists.txt`, target `vangfx`, with
`-DVANGFX_DEBUG_INFO=OFF` for Release.

## Library matrix

| Path | Arch | Config | Size |
|---|---|---|---|
| `libs/MSVC/win-x64/Release/vangfx.lib` | x64 | Release | 27.64 MB |
| `libs/MSVC/win-x64/Debug/vangfx.lib` | x64 | Debug | 65.37 MB |
| `libs/MSVC/win-x86/Release/vangfx.lib` | x86 | Release | 20.20 MB |
| `libs/MSVC/win-x86/Debug/vangfx.lib` | x86 | Debug | 46.76 MB |

Architecture was confirmed by reading the COFF machine field out of each archive's
object members, not inferred from the folder name.

The Release libraries are built without `/Zi`. Debug information in a static
library is CodeView records inside every object and roughly doubles the archive,
which is bytes every consumer downloads to step through code they do not have.
The Debug libraries keep it, from CMake's own Debug flags.

> The x64 Debug library is 65 MB. GitHub warns above 50 MB and rejects a file
> above 100 MB in a repository, so this package fits but is not small; a release
> asset or Git LFS is the better home for it.

## CRT

**Static CRT** (`/MT` Release, `/MTd` Debug), set in the project's
`CMakeLists.txt`. No Visual C++ Redistributable is required, and your own target
must use the same setting or the link fails with `LNK2038`:

```cmake
set_property(TARGET my_mod PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

## Linking

`vangfx.lib` is a static library, not a self-contained one. A consumer also links
the Direct3D import libraries:

```
d3d9 d3d11 d3d12 dxgi dxguid d3dcompiler
```

and, for SM 6.x DXIL through DXC, `dxcompiler.lib` from the Windows SDK. See the
header comment at the top of `include/vangfx/vangfx.hpp` and `libs/README.md`.

## What ships

32 public headers under `include/vangfx/`. The `compat/` internals — vtable
plumbing, the per-API slot tables, the COM helpers — stay in the private source
tree under `src/internal/` and are not part of this package.

## Verified

A consumer project outside this tree was built Release x64 against these headers
and `libs/MSVC/win-x64/Release/vangfx.lib` with `/MT`, and run. It reported:

```
vangfx linked | build() with no device -> code 11, message: BackendKind::Auto was
not resolved — no D3D DLL found in process.
ShaderBuilder constructed
```

which is the correct typed error for a builder with no device attached, so the
headers are self-contained and the archive resolves the symbols a consumer calls.

## Other targets

ARM64 and any custom configuration require a build from the VanGFX source
repository. See `README.md`.
