cmake_minimum_required(VERSION 3.20)

# ── DXC headers ──────────────────────────────────────────────────────────────
# Look for dxcapi.h: first check the external/ stub, then the Windows SDK.

set(DXC_INCLUDE_DIR "" CACHE PATH
    "Override path to the directory containing dxcapi.h (leave empty to auto-detect)")

if(DXC_INCLUDE_DIR)
    message(STATUS "vgxc: using DXC headers from DXC_INCLUDE_DIR=${DXC_INCLUDE_DIR}")
else()
    # Windows SDK path via CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(_sdk_ver "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    elseif(DEFINED ENV{WindowsSDKVersion})
        string(REGEX REPLACE "[/\\\\]$" "" _sdk_ver "$ENV{WindowsSDKVersion}")
    else()
        set(_sdk_ver "")
    endif()

    if(_sdk_ver)
        set(_sdk_inc "C:/Program Files (x86)/Windows Kits/10/Include/${_sdk_ver}/um")
        if(EXISTS "${_sdk_inc}/dxcapi.h")
            set(DXC_INCLUDE_DIR "${_sdk_inc}")
            message(STATUS "vgxc: found dxcapi.h in Windows SDK ${_sdk_ver}")
        endif()
    endif()

    # Broad fallback search
    if(NOT DXC_INCLUDE_DIR)
        file(GLOB_RECURSE _dxc_header
             "C:/Program Files (x86)/Windows Kits/10/Include/*/um/dxcapi.h")
        if(_dxc_header)
            list(GET _dxc_header 0 _dxc_header_first)
            get_filename_component(DXC_INCLUDE_DIR "${_dxc_header_first}" DIRECTORY)
            message(STATUS "vgxc: found dxcapi.h via glob: ${DXC_INCLUDE_DIR}")
        endif()
    endif()

    if(NOT DXC_INCLUDE_DIR)
        message(FATAL_ERROR
            "vgxc: could not find dxcapi.h.\n"
            "Install the Windows SDK, or set -DDXC_INCLUDE_DIR=<path/to/dxcapi.h's dir>.")
    endif()
endif()

# ── SPIRV-Cross (optional) ────────────────────────────────────────────────────

option(VANGFX_SPIRV_CROSS "Enable SPIRV-Cross (GLSL/ESSL/MSL targets)" OFF)

# ── vgxc_lib object library (shared by exe and tests) ────────────────────────

add_library(vgxc_lib STATIC
    compile.cpp
    pack.cpp
)

target_compile_features(vgxc_lib PUBLIC cxx_std_20)
set_property(TARGET vgxc_lib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(vgxc_lib PUBLIC
    ${DXC_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(vgxc_lib PUBLIC d3dcompiler dxguid)

if(VANGFX_SPIRV_CROSS)
    target_compile_definitions(vgxc_lib PUBLIC VANGFX_SPIRV_CROSS=1)
    target_include_directories(vgxc_lib PUBLIC
        "${CMAKE_SOURCE_DIR}/external/spirv-cross")
    target_sources(vgxc_lib PRIVATE
        "${CMAKE_SOURCE_DIR}/external/spirv-cross/spirv_cross_c.cpp"
        "${CMAKE_SOURCE_DIR}/external/spirv-cross/spirv_cross.cpp"
        "${CMAKE_SOURCE_DIR}/external/spirv-cross/spirv_glsl.cpp"
        "${CMAKE_SOURCE_DIR}/external/spirv-cross/spirv_msl.cpp"
        "${CMAKE_SOURCE_DIR}/external/spirv-cross/spirv_hlsl.cpp"
        "${CMAKE_SOURCE_DIR}/external/spirv-cross/spirv_parser.cpp"
        "${CMAKE_SOURCE_DIR}/external/spirv-cross/spirv_cfg.cpp"
    )
endif()

# ── vgxc executable ──────────────────────────────────────────────────────────

add_executable(vgxc main.cpp)
target_link_libraries(vgxc PRIVATE vgxc_lib)
set_target_properties(vgxc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)
set_property(TARGET vgxc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

install(TARGETS vgxc RUNTIME DESTINATION tools)
