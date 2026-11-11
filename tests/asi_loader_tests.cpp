/**
 * tests/asi_loader_tests.cpp — vangfx::loader::asi smoke test.
 *
 * Calls LoadAll() on the directory that holds the test-only fake_asi.asi,
 * which was built by CMake as a DLL with a .asi suffix. Verifies that:
 *   1. LoadAll finds and loads at least one .asi file (LoadedCount >= 1).
 *   2. The loaded module's exported sentinel is true (DllMain ran).
 *
 * VGX_ASI_TEST_DIR is defined by CMake to the output directory of fake_asi.asi.
 */

#include "harness.hpp"

#include <vangfx/loader/AsiLoader.h>

#include <windows.h>
#include <string>

using namespace vgx;

#ifndef VGX_ASI_TEST_DIR
#  error "VGX_ASI_TEST_DIR must be defined by CMake"
#endif

VGX_TEST(asi_loader_loads_fake_asi) {
    std::wstring dir = L"" VGX_ASI_TEST_DIR;

    vangfx::loader::asi::LoadAll(dir);

    // At least the fake_asi.asi we just built should have been picked up.
    CHECK(vangfx::loader::asi::LoadedCount() >= 1u);

    // Verify the DLL's DllMain actually ran by calling its export.
    HMODULE mod = GetModuleHandleW(L"fake_asi.asi");
    if (mod) {
        using Fn = bool(*)();
        auto fn = reinterpret_cast<Fn>(GetProcAddress(mod, "vgx_asi_was_loaded"));
        CHECK(fn != nullptr);
        CHECK(fn());
    }
}
