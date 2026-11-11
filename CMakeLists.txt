cmake_minimum_required(VERSION 3.20)
project(vangfx LANGUAGES C CXX)

# C++23, not 20: core/error.hpp is built on std::expected, a C++23 library
# feature. C11 for the one C file, src/internal/compat/slot_checks.c, whose
# _Static_assert checks every vtable slot VanGFX patches against the SDK.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── What to build ────────────────────────────────────────────────────────────
#
# Each backend is optional, so a mod for a D3D9 game links none of the D3D11
# or D3D12 code. ContextBuilder::build() for a backend left out returns
# ShaderError::Unsupported. vangfx/config.hpp is generated with the choices so
# a consumer can test for them.
option(VANGFX_D3D9        "Direct3D 9 backend"                                ON)
option(VANGFX_D3D10       "Direct3D 10 backend"                               ON)
option(VANGFX_D3D11       "Direct3D 11 backend"                               ON)
option(VANGFX_D3D12       "Direct3D 12 backend"                               ON)
option(VANGFX_GL          "OpenGL WGL front end via VanHooks + gpu backend"   OFF)
option(VANGFX_D3D8        "Direct3D 8 front end + d3d8_to_d3d9 translator"   OFF)
option(VANGFX_VULKAN      "Vulkan implicit layer + gpu backend"               OFF)
option(VANGFX_GLES        "GLES/EGL front end (Android NDK 29)"               OFF)
option(VANGFX_METAL       "Metal 3/4 front end (macOS/iOS — experimental)"    OFF)
option(VANGFX_ANDROID     "Android injected .so loader (SHAR, NDK 29)"        OFF)
option(VANGFX_AUDIO       "Audio module (XAudio2 / DirectSound capture)"      OFF)
option(VANGFX_GPU         "Phase 4: gpu::Device/Pass abstract GPU layer"      ON)
option(VANGFX_FX          "Phase 4: fx::Chain, DepthService, EffectDesc"      ON)
option(VANGFX_DEBUG_INFO  "Emit debug info (/Zi) into the static library"     ON)
option(VANGFX_TESTS       "Build the WARP test suite (tests/)"                ${PROJECT_IS_TOP_LEVEL})
option(VANGFX_VGXC        "Build the vgxc effect/shader pack compiler tool"   ${PROJECT_IS_TOP_LEVEL})

file(GLOB VANGFX_SOURCES
    src/core/*.cpp
    src/capture/*.cpp
    src/effect/*.cpp
    src/proxy/*.cpp
)
list(APPEND VANGFX_SOURCES
    src/internal/compat/slot_checks.c
    src/internal/compat/slot_checks.cpp
)
# Shared DXGI front end (always built when any D3D backend is on)
if(VANGFX_D3D9 OR VANGFX_D3D10 OR VANGFX_D3D11 OR VANGFX_D3D12)
    file(GLOB _dxgi src/backend/dxgi/*.cpp)
    list(APPEND VANGFX_SOURCES ${_dxgi})
endif()
if(VANGFX_D3D9)
    file(GLOB _d3d9 src/backend/d3d9/*.cpp)
    list(APPEND VANGFX_SOURCES ${_d3d9})
endif()
if(VANGFX_D3D10)
    file(GLOB _d3d10 src/backend/d3d10/*.cpp)
    list(APPEND VANGFX_SOURCES ${_d3d10})
endif()
if(VANGFX_D3D11)
    file(GLOB _d3d11 src/backend/d3d11/*.cpp)
    list(APPEND VANGFX_SOURCES ${_d3d11})
endif()
if(VANGFX_D3D12)
    file(GLOB _d3d12 src/backend/d3d12/*.cpp)
    list(APPEND VANGFX_SOURCES ${_d3d12})
endif()
if(VANGFX_GL)
    file(GLOB _gl src/backend/gl/*.cpp)
    list(APPEND VANGFX_SOURCES ${_gl})
    if(VANGFX_GPU)
        list(APPEND VANGFX_SOURCES src/gpu/gl/device_gl.cpp)
    endif()
endif()
if(VANGFX_D3D8)
    file(GLOB _d3d8 src/backend/d3d8/*.cpp)
    list(APPEND VANGFX_SOURCES ${_d3d8})
    file(GLOB _translate_d3d8 src/translate/targets/d3d8_to_d3d9/*.cpp)
    list(APPEND VANGFX_SOURCES ${_translate_d3d8})
endif()
if(VANGFX_VULKAN)
    file(GLOB _vulkan src/backend/vulkan/*.cpp)
    list(APPEND VANGFX_SOURCES ${_vulkan})
    if(VANGFX_GPU)
        list(APPEND VANGFX_SOURCES src/gpu/vulkan/device_vulkan.cpp)
    endif()
endif()
if(VANGFX_GLES)
    file(GLOB _gles src/backend/gles/*.cpp)
    list(APPEND VANGFX_SOURCES ${_gles})
endif()
if(VANGFX_D3D9 AND VANGFX_VULKAN)
    file(GLOB _translate_vk src/translate/targets/d3d9_to_vulkan/*.cpp)
    list(APPEND VANGFX_SOURCES ${_translate_vk})
endif()
if(VANGFX_METAL)
    file(GLOB _metal src/backend/metal/*.cpp)
    list(APPEND VANGFX_SOURCES ${_metal})
    if(VANGFX_GPU)
        list(APPEND VANGFX_SOURCES src/gpu/metal/device_metal.cpp)
    endif()
endif()
if(VANGFX_ANDROID)
    list(APPEND VANGFX_SOURCES src/loader/android/android_loader.cpp)
endif()
if(VANGFX_AUDIO)
    file(GLOB_RECURSE _audio src/audio/*.cpp)
    list(APPEND VANGFX_SOURCES ${_audio})
endif()
# Phase 4: gpu abstract layer (Device / Pass) ─────────────────────────────────
if(VANGFX_GPU AND VANGFX_D3D10)
    list(APPEND VANGFX_SOURCES src/gpu/d3d10/device_d3d10.cpp)
endif()
if(VANGFX_GPU AND VANGFX_D3D11)
    list(APPEND VANGFX_SOURCES src/gpu/d3d11/device_d3d11.cpp)
endif()
if(VANGFX_GPU AND VANGFX_D3D9)
    list(APPEND VANGFX_SOURCES src/gpu/d3d9/device_d3d9.cpp)
endif()
if(VANGFX_GPU AND VANGFX_D3D12)
    list(APPEND VANGFX_SOURCES src/gpu/d3d12/device_d3d12.cpp)
endif()

# Phase 4: fx layer (Chain, EffectDesc, DepthService) ─────────────────────────
if(VANGFX_FX)
    list(APPEND VANGFX_SOURCES
        src/fx/chain.cpp
        src/fx/depth.cpp
        src/fx/effect_desc.cpp
    )
endif()

# ASI loader (no graphics API dependency)
list(APPEND VANGFX_SOURCES src/loader/asi/AsiLoader.cpp)
# D3D9 translator core: SM1-3 → HLSL, FFP → HLSL, format tables, state tracker
if(VANGFX_D3D9 AND VANGFX_D3D12)
    file(GLOB _translate_d3d9 src/translate/d3d9/*.cpp)
    list(APPEND VANGFX_SOURCES ${_translate_d3d9})
    file(GLOB _translate_dx12_core    src/translate/targets/d3d9_to_d3d12/core/*.cpp)
    file(GLOB _translate_dx12_proxy   src/translate/targets/d3d9_to_d3d12/d3d9proxy/*.cpp)
    list(APPEND VANGFX_SOURCES ${_translate_dx12_core} ${_translate_dx12_proxy})
endif()

configure_file(cmake/config.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/vangfx/config.hpp @ONLY)

add_library(vangfx STATIC ${VANGFX_SOURCES})
add_library(vangfx::vangfx ALIAS vangfx)

target_include_directories(vangfx
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
            $<INSTALL_INTERFACE:include>
    PRIVATE src/internal/          # compat/** stays out of the SDK
            src/                   # context_impl.hpp and other internal headers
)

# NOMINMAX is not optional: windows.h's min/max macros turn every
# std::numeric_limits<T>::max() into "illegal token on right side of '::'".
target_compile_definitions(vangfx PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Debug info in a static library is CodeView records inside every object, and
# roughly half of vangfx.lib with it on: worth it while working on VanGFX,
# bytes nobody needs in a release package.
target_compile_options(vangfx PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /MP /permissive- /Zc:__cplusplus>
    $<$<BOOL:${VANGFX_DEBUG_INFO}>:/Zi>
)
set_property(TARGET vangfx PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(vangfx PUBLIC dxgi dxguid d3dcompiler pathcch)
if(VANGFX_D3D9)
    target_link_libraries(vangfx PUBLIC d3d9)
endif()
if(VANGFX_D3D10)
    target_link_libraries(vangfx PUBLIC d3d10)
endif()
if(VANGFX_D3D11)
    target_link_libraries(vangfx PUBLIC d3d11)
endif()
if(VANGFX_D3D12)
    target_link_libraries(vangfx PUBLIC d3d12)
endif()
if(VANGFX_AUDIO)
    target_link_libraries(vangfx PUBLIC xaudio2 dsound)
endif()
if(VANGFX_GL)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_VH_ARCH win-x64)
    else()
        set(_VH_ARCH win-x86)
    endif()
    set(_VH_SDK "C:/Users/Ty/Desktop/TeamVanilla/Projects/VanHooks/VanHooksSDK")
    target_include_directories(vangfx PRIVATE "${_VH_SDK}/include")
    target_link_libraries(vangfx PUBLIC opengl32
        "$<$<CONFIG:Debug>:${_VH_SDK}/lib/${_VH_ARCH}/Debug/vanhooks.lib>"
        "$<$<CONFIG:Release>:${_VH_SDK}/lib/${_VH_ARCH}/Release/vanhooks.lib>")
    unset(_VH_SDK)
    unset(_VH_ARCH)
endif()
if(VANGFX_D3D8)
    if(NOT VANGFX_D3D9)
        target_link_libraries(vangfx PUBLIC d3d9)
    endif()
endif()

if(VANGFX_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VANGFX_VGXC AND WIN32)
    add_subdirectory(tools/vgxc)
endif()

# ── Install / SDK drop ───────────────────────────────────────────────────────
# Public headers, the generated config and the library; never src/.
install(TARGETS vangfx EXPORT vangfx-targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/vangfx/config.hpp DESTINATION include/vangfx)
install(EXPORT vangfx-targets NAMESPACE vangfx:: DESTINATION lib/cmake/vangfx FILE vangfx-config.cmake)
