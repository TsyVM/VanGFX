#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>

namespace vangfx {

// ─────────────────────────────────────────────────────────────────────────────
//  Config — INI-based configuration with environment variable override
//
//  Priority (highest first):
//    1. Environment variable  VANGFX_<SECTION>_<KEY>  (upper-case, _ separator)
//    2. The loaded INI file
//    3. The default value you pass to get()
//
//  Keys are case-insensitive.  Section and key names may not contain '=' or ']'.
// ─────────────────────────────────────────────────────────────────────────────

class Config {
public:
    Config() = default;

    // Load from file; silently keeps defaults if the file is absent.
    // Returns false only if the file exists but could not be parsed.
    bool load(const std::filesystem::path& ini_path);

    // ── Read ──────────────────────────────────────────────────────────────────

    // Returns the value for [section] key= or the default.
    // Env override: VANGFX_<SECTION>_<KEY>  (sections and keys uppercased,
    // non-alphanumeric characters replaced with '_').
    [[nodiscard]] std::string get(std::string_view section,
                                  std::string_view key,
                                  std::string_view default_value = {}) const;

    [[nodiscard]] int         get_int (std::string_view section,
                                       std::string_view key,
                                       int              default_value = 0) const;

    [[nodiscard]] bool        get_bool(std::string_view section,
                                       std::string_view key,
                                       bool             default_value = false) const;

    // Returns nullopt when the key is absent (no env, no ini entry).
    [[nodiscard]] std::optional<std::string> get_opt(std::string_view section,
                                                      std::string_view key) const;

    // ── Write (in-memory only; call save() to persist) ────────────────────────
    void set(std::string_view section, std::string_view key, std::string_view value);

    bool save(const std::filesystem::path& ini_path) const;

    // True if at least one key was loaded or set.
    [[nodiscard]] bool empty() const noexcept;

private:
    struct Impl;
    struct Impl* impl_ = nullptr;  // pimpl; allocated on first write or load

    [[nodiscard]] std::string env_key(std::string_view section,
                                      std::string_view key) const;
};

} // namespace vangfx
