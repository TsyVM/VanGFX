# The VanGFX test suite: one executable, every test in a process of its own.
#
#   ctest -C Release                   everything, on WARP
#   vangfx_tests d3d12                 the D3D12 tests
#   set VGX_HW=1 && vangfx_tests       on the first hardware adapter instead

set(VGX_TEST_SOURCES main.cpp core_tests.cpp capture_writer_tests.cpp capture_tests.cpp config_tests.cpp proxy_tests.cpp com_wrap_tests.cpp)
if(VANGFX_D3D9)
    list(APPEND VGX_TEST_SOURCES d3d9_tests.cpp)
endif()
if(VANGFX_D3D10)
    list(APPEND VGX_TEST_SOURCES d3d10_tests.cpp)
endif()
if(VANGFX_D3D11)
    list(APPEND VGX_TEST_SOURCES d3d11_tests.cpp)
endif()
if(VANGFX_D3D12)
    list(APPEND VGX_TEST_SOURCES d3d12_tests.cpp)
endif()
if(VANGFX_AUDIO)
    list(APPEND VGX_TEST_SOURCES audio_tests.cpp)
endif()
# Phase 3: translator core (CPU tests, no GPU required)
if(VANGFX_D3D9 AND VANGFX_D3D12)
    list(APPEND VGX_TEST_SOURCES translator_tests.cpp)
    list(APPEND VGX_TEST_SOURCES d3d9_to_d3d12_tests.cpp)
endif()
# ASI loader (always built; fake_asi.asi lands in the same output directory)
list(APPEND VGX_TEST_SOURCES asi_loader_tests.cpp)
# Phase 4: gpu and fx layer tests
if(VANGFX_GPU)
    list(APPEND VGX_TEST_SOURCES gpu_tests.cpp fx_tests.cpp)
endif()
# Phase 5: vgxc compile/pack tests
if(VANGFX_VGXC)
    list(APPEND VGX_TEST_SOURCES vgxc_tests.cpp)
endif()
# Phase 7: OpenGL WGL front end
if(VANGFX_GL)
    list(APPEND VGX_TEST_SOURCES gl_tests.cpp)
endif()
# Phase 7: D3D8 front end (32-bit; skip gracefully on x64)
if(VANGFX_D3D8)
    list(APPEND VGX_TEST_SOURCES d3d8_tests.cpp)
endif()

# A stand-in for a runtime DLL, so the hook tests patch a vtable in another
# module as they would d3d11.dll's. Both land in one folder so the DLL is found.
add_library(vgx_fake_runtime SHARED fake_runtime.cpp)

# A minimal .asi for the loader test: a DLL renamed to .asi.
add_library(vgx_fake_asi SHARED fake_asi.cpp)
set_target_properties(vgx_fake_asi PROPERTIES
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>)
set_property(TARGET vgx_fake_asi PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(vangfx_tests ${VGX_TEST_SOURCES})
target_link_libraries(vangfx_tests PRIVATE vangfx vgx_fake_runtime d3dcompiler d3d11 windowscodecs ole32)
if(VANGFX_VGXC)
    target_link_libraries(vangfx_tests PRIVATE vgxc_lib)
    target_include_directories(vangfx_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/tools/vgxc")
endif()
if(VANGFX_D3D9)
    target_link_libraries(vangfx_tests PRIVATE d3d9)
endif()
if(VANGFX_D3D10)
    target_link_libraries(vangfx_tests PRIVATE d3d10)
endif()
if(VANGFX_D3D12)
    target_link_libraries(vangfx_tests PRIVATE d3d12 dxgi)
endif()
if(VANGFX_GL)
    target_link_libraries(vangfx_tests PRIVATE opengl32)
endif()
set_target_properties(vgx_fake_runtime vangfx_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>)
target_include_directories(vangfx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../src/internal)
target_compile_options(vangfx_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
set_property(TARGET vangfx_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Tell the ASI loader test where to find fake_asi.asi at runtime.
target_compile_definitions(vangfx_tests PRIVATE
    VGX_ASI_TEST_DIR=L"$<TARGET_FILE_DIR:vgx_fake_asi>"
    $<$<BOOL:${VANGFX_D3D9}>:VANGFX_D3D9>
    $<$<BOOL:${VANGFX_D3D10}>:VANGFX_D3D10>
    $<$<BOOL:${VANGFX_D3D11}>:VANGFX_D3D11>
    $<$<BOOL:${VANGFX_D3D12}>:VANGFX_D3D12>
    $<$<BOOL:${VANGFX_GPU}>:VANGFX_GPU>
    $<$<BOOL:${VANGFX_FX}>:VANGFX_FX>
    $<$<BOOL:${VANGFX_VGXC}>:VANGFX_VGXC>
    $<$<BOOL:${VANGFX_GL}>:VANGFX_GL>
    $<$<BOOL:${VANGFX_D3D8}>:VANGFX_D3D8>
)

# One ctest entry per test, discovered from the executable after each build.
add_test(NAME vangfx_tests COMMAND vangfx_tests)
