# VanGFX — consumer CMake drop-in
#
# Usage:
#   add_subdirectory(path/to/VanGFX)   # or find_package after install
#   target_link_libraries(my_mod PRIVATE VanGFX::vangfx)

cmake_minimum_required(VERSION 3.20)
project(VanGFX LANGUAGES CXX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_VGX_ARCH win-x64)
else()
    set(_VGX_ARCH win-x86)
endif()

add_library(VanGFX::vangfx STATIC IMPORTED GLOBAL)
set_target_properties(VanGFX::vangfx PROPERTIES
    IMPORTED_LOCATION_RELEASE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/MSVC/${_VGX_ARCH}/Release/vangfx.lib"
    IMPORTED_LOCATION_DEBUG
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/MSVC/${_VGX_ARCH}/Debug/vangfx.lib"
    INTERFACE_INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INTERFACE_COMPILE_DEFINITIONS
        "WIN32_LEAN_AND_MEAN;NOMINMAX"
    INTERFACE_COMPILE_OPTIONS
        "$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>"
)

unset(_VGX_ARCH)
