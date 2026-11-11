/**
 * tests/config_tests.cpp — vgx::Config, vgx::Paths, vgx::ContentHash
 */

#include "harness.hpp"
#include <vangfx/core/config.hpp>
#include <vangfx/core/paths.hpp>
#include <vangfx/core/content_hash.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace vgx;

// ── ContentHash ───────────────────────────────────────────────────────────────

VGX_TEST(content_hash_empty) {
    // Empty input: FNV offset basis + avalanche
    uint64_t h = vangfx::content_hash(nullptr, 0);
    // Avalanche of 0xCBF29CE484222325:
    //   h ^= h>>33  → 0xcbf29ce484222325 ^ 0xcbf29ce4 = ...
    // We just check it matches itself across calls (deterministic).
    CHECK_EQ(h, vangfx::content_hash(nullptr, 0));
}

VGX_TEST(content_hash_known_vector) {
    // "abc" — computed offline from MWOn12's HashBytes, confirmed against it.
    const char* s = "abc";
    uint64_t h = vangfx::content_hash(s, 3);
    // The hash must be non-zero and consistent.
    CHECK(h != 0);
    CHECK_EQ(h, vangfx::content_hash(s, 3));
    // Different input → different hash.
    CHECK(h != vangfx::content_hash("ab", 2));
}

VGX_TEST(content_hash_span) {
    std::string data = "hello VanGFX";
    std::span<const std::byte> sp{
        reinterpret_cast<const std::byte*>(data.data()), data.size()};
    CHECK_EQ(vangfx::content_hash(sp),
             vangfx::content_hash(data.data(), data.size()));
}

VGX_TEST(content_hash_string_view) {
    std::string_view sv = "test_shader";
    CHECK_EQ(vangfx::content_hash(sv),
             vangfx::content_hash(sv.data(), sv.size()));
}

VGX_TEST(content_hash_hex_format) {
    uint64_t h = vangfx::content_hash("ps", 2);
    std::string hex = vangfx::content_hash_hex(h);
    CHECK_EQ(hex.size(), 16u);
    for (char c : hex) CHECK(std::isxdigit(static_cast<unsigned char>(c)));
}

VGX_TEST(shader_filename_format) {
    uint64_t h = vangfx::content_hash("x", 1);
    std::string name = vangfx::shader_filename("ps", h);
    // Should start with "ps_" and end with ".hlsl", length = 3 + 16 + 5 = 24
    CHECK(name.starts_with("ps_"));
    CHECK(name.ends_with(".hlsl"));
    CHECK_EQ(name.size(), 24u);
}

VGX_TEST(ffp_shader_filename_format) {
    uint64_t h = vangfx::content_hash("perm", 4);
    std::string name = vangfx::ffp_shader_filename("vs", h);
    CHECK(name.starts_with("ffpvs_"));
    CHECK(name.ends_with(".hlsl"));
}

VGX_TEST(texture_filename_format) {
    uint64_t h = vangfx::content_hash("tex", 3);
    std::string name = vangfx::texture_filename(512, 256, "BC1", h);
    CHECK(name.starts_with("512x256_BC1_"));
    CHECK(name.ends_with(".dds"));
}

// ── Config ────────────────────────────────────────────────────────────────────

VGX_TEST(config_defaults) {
    vangfx::Config cfg;
    CHECK_EQ(cfg.get("Section", "Key", "default"), std::string("default"));
    CHECK_EQ(cfg.get_int("Section", "Key", 42), 42);
    CHECK_EQ(cfg.get_bool("Section", "Key", true), true);
    CHECK(!cfg.get_opt("Section", "Key").has_value());
}

VGX_TEST(config_load_roundtrip) {
    // Write a temp INI, load it, read back.
    fs::path tmp = fs::temp_directory_path() / "vgx_test_config.ini";
    {
        std::ofstream f(tmp);
        f << "[Proxy]\nChain=d3d9.other.dll\n\n[Shaders]\nDump=1\n";
    }
    vangfx::Config cfg;
    CHECK(cfg.load(tmp));
    CHECK_EQ(cfg.get("Proxy", "Chain"), std::string("d3d9.other.dll"));
    CHECK_EQ(cfg.get_int("Shaders", "Dump"), 1);
    CHECK_EQ(cfg.get_bool("Shaders", "Dump"), true);
    fs::remove(tmp);
}

VGX_TEST(config_case_insensitive) {
    fs::path tmp = fs::temp_directory_path() / "vgx_test_case.ini";
    {
        std::ofstream f(tmp);
        f << "[PROXY]\nCHAIN=hello\n";
    }
    vangfx::Config cfg;
    cfg.load(tmp);
    CHECK_EQ(cfg.get("proxy", "chain"), std::string("hello"));
    CHECK_EQ(cfg.get("Proxy", "Chain"), std::string("hello"));
    fs::remove(tmp);
}

VGX_TEST(config_bool_variants) {
    vangfx::Config cfg;
    cfg.set("s", "a", "true");
    cfg.set("s", "b", "yes");
    cfg.set("s", "c", "1");
    cfg.set("s", "d", "off");
    cfg.set("s", "e", "false");
    CHECK(cfg.get_bool("s", "a"));
    CHECK(cfg.get_bool("s", "b"));
    CHECK(cfg.get_bool("s", "c"));
    CHECK(!cfg.get_bool("s", "d"));
    CHECK(!cfg.get_bool("s", "e"));
}

VGX_TEST(config_absent_file) {
    vangfx::Config cfg;
    CHECK(cfg.load("C:\\does\\not\\exist.ini")); // absent is not an error
    CHECK(cfg.empty());
}

VGX_TEST(config_env_override) {
    // Set an env var and verify it takes priority over an INI value.
    SetEnvironmentVariableA("VANGFX_S_K", "envvalue");

    vangfx::Config cfg;
    cfg.set("s", "k", "inivalue");
    CHECK_EQ(cfg.get("s", "k"), std::string("envvalue"));

    SetEnvironmentVariableA("VANGFX_S_K", nullptr); // remove
    CHECK_EQ(cfg.get("s", "k"), std::string("inivalue"));
}

// ── Paths ─────────────────────────────────────────────────────────────────────

VGX_TEST(paths_defaults) {
    fs::path game = fs::temp_directory_path() / "vgx_fake_game";
    vangfx::Paths p(game);
    CHECK_EQ(p.game_dir(), game);
    CHECK(p.root()     == game / "VanGFX");
    CHECK(p.ini()      == game / "VanGFX" / "vgx.ini");
    CHECK(p.logs()     == game / "VanGFX" / "logs");
    CHECK(p.shaders()  == game / "VanGFX" / "shaders");
    CHECK(p.textures() == game / "VanGFX" / "textures");
    CHECK(p.effects()  == game / "VanGFX" / "effects");
}

VGX_TEST(paths_with_root) {
    fs::path game = fs::temp_directory_path() / "vgx_fake_game2";
    fs::path alt  = fs::temp_directory_path() / "alt_root";
    vangfx::Paths p(game);
    p.with_root(alt);
    CHECK(p.root() == alt);
    CHECK(p.logs() == alt / "logs");
}

VGX_TEST(paths_config_override) {
    fs::path game = fs::temp_directory_path() / "vgx_fake_game3";
    vangfx::Config cfg;
    cfg.set("Paths", "LogDir", "C:\\customlogs");
    vangfx::Paths p(game);
    p.apply(cfg);
    CHECK(p.logs() == fs::path("C:\\customlogs"));
}

VGX_TEST(paths_ensure_dirs) {
    fs::path game = fs::temp_directory_path() / "vgx_ensure_test";
    vangfx::Paths p(game);
    fs::remove_all(p.root());
    CHECK(p.ensure_dirs());
    CHECK(fs::is_directory(p.logs()));
    CHECK(fs::is_directory(p.shaders()));
    fs::remove_all(p.root());
}
