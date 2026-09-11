# VanGFX — Precompiled Libraries

Precompiled static libraries for all supported targets.  
All libraries link the static CRT — no Redistributable required.

## Windows (MSVC)

| Target       | Config   | Path                                    |
|--------------|----------|-----------------------------------------|
| Windows x64  | Release  | `MSVC/win-x64/Release/vangfx.lib`       |
| Windows x64  | Debug    | `MSVC/win-x64/Debug/vangfx.lib`         |
| Windows x86  | Release  | `MSVC/win-x86/Release/vangfx.lib`       |
| Windows x86  | Debug    | `MSVC/win-x86/Debug/vangfx.lib`         |

## CMake (drop-in)

```cmake
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(VGX_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/MSVC/win-x64")
else()
    set(VGX_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/MSVC/win-x86")
endif()

add_library(VanGFX::vangfx STATIC IMPORTED)
set_target_properties(VanGFX::vangfx PROPERTIES
    IMPORTED_LOCATION_RELEASE "${VGX_LIB_DIR}/Release/vangfx.lib"
    IMPORTED_LOCATION_DEBUG   "${VGX_LIB_DIR}/Debug/vangfx.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(my_mod PRIVATE VanGFX::vangfx)
```
