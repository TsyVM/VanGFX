#pragma once

/**
 * tests/harness.hpp — the VanGFX test runner's tiny framework.
 *
 *   VGX_TEST(d3d12_draws) {
 *       ...
 *       CHECK(ok);
 *       CHECK_EQ(draws, 6);
 *   }
 *
 * Each test runs in a process of its own (the runner starts itself with
 * --run <name>), so a crash, a stack overflow or a hang fails that test and the
 * rest still run. Tests use WARP unless VGX_HW=1 is set, which picks the first
 * hardware adapter instead.
 */

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace vgx {

struct Test {
    const char* name;
    void (*fn)();
};

std::vector<Test>& tests();
int add(const char* name, void (*fn)());

[[noreturn]] void fail(const char* file, int line, const std::string& what);
void note(const char* fmt, ...);

bool use_hardware();   // VGX_HW=1

// RGBA8 as read from memory (little endian: 0xAABBGGRR).
constexpr uint32_t kRed   = 0xFF0000FF;
constexpr uint32_t kGreen = 0xFF00FF00;
constexpr uint32_t kBlue  = 0xFFFF0000;

template <typename T>
std::string str(const T& v) {
    if constexpr (std::is_same_v<T, bool>)          return v ? "true" : "false";
    else if constexpr (std::is_enum_v<T>)           return std::to_string(static_cast<long long>(v));
    else if constexpr (std::is_arithmetic_v<T>)     return std::to_string(v);
    else if constexpr (std::is_pointer_v<T>) {
        char b[32]; std::snprintf(b, sizeof(b), "%p", static_cast<const void*>(v)); return b;
    }
    else if constexpr (std::is_convertible_v<T, std::string>) return std::string(v);
    else return "?";
}

} // namespace vgx

#define VGX_TEST(name)                                                                  \
    static void vgx_test_##name();                                                      \
    static const int vgx_reg_##name = ::vgx::add(#name, &vgx_test_##name);              \
    static void vgx_test_##name()

#define CHECK(cond)                                                                     \
    do { if (!(cond)) ::vgx::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        const auto vgx_a = (a);                                                         \
        const auto vgx_b = (b);                                                         \
        if (!(vgx_a == vgx_b))                                                          \
            ::vgx::fail(__FILE__, __LINE__, std::string("CHECK_EQ(" #a ", " #b ")  ") + \
                        ::vgx::str(vgx_a) + " != " + ::vgx::str(vgx_b));                \
    } while (0)

#define CHECK_MSG(cond, msg)                                                            \
    do { if (!(cond)) ::vgx::fail(__FILE__, __LINE__, std::string("CHECK(" #cond ")  ") + (msg)); } while (0)
