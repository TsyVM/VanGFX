#pragma once

#include <filesystem>

namespace vangfx {

class Config;

// ─────────────────────────────────────────────────────────────────────────────
//  Paths — canonical paths for the game folder structure
//
//  All paths are resolved relative to game_dir (the directory that contains
//  the game executable).  By default:
//
//    <game_dir>\VanGFX\               root()
//    <game_dir>\VanGFX\vgx.ini       ini()
//    <game_dir>\VanGFX\logs\          logs()
//    <game_dir>\VanGFX\effects\       effects()
//    <game_dir>\VanGFX\shaders\       shaders()
//    <game_dir>\VanGFX\textures\      textures()
//
//  The root path and ini name can be overridden by VANGFX_ROOT_DIR and
//  VANGFX_INI_NAME environment variables, or by calling with_root() /
//  with_ini().  Sub-directories can also be overridden via Config:
//
//    [Paths]
//    LogDir   = C:\SomePath\logs
//    ShaderDir = shaders   ; relative → resolved under root()
// ─────────────────────────────────────────────────────────────────────────────

class Paths {
public:
    // Build from the directory that contains the game executable.
    explicit Paths(std::filesystem::path game_dir);

    // Apply [Paths] overrides from a loaded Config.
    Paths& apply(const Config& cfg);

    // Fluent overrides (for tests or tools).
    Paths& with_root(std::filesystem::path root);
    Paths& with_ini (std::string_view ini_name);

    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] const std::filesystem::path& game_dir()  const noexcept;
    [[nodiscard]] const std::filesystem::path& root()      const noexcept;
    [[nodiscard]] const std::filesystem::path& ini()       const noexcept;
    [[nodiscard]] const std::filesystem::path& logs()      const noexcept;
    [[nodiscard]] const std::filesystem::path& effects()   const noexcept;
    [[nodiscard]] const std::filesystem::path& shaders()   const noexcept;
    [[nodiscard]] const std::filesystem::path& textures()  const noexcept;

    // Create all standard directories (logs, effects, shaders, textures).
    // Returns false if any creation fails.
    bool ensure_dirs() const;

private:
    std::filesystem::path game_dir_;
    std::filesystem::path root_;
    std::filesystem::path ini_;
    std::filesystem::path logs_;
    std::filesystem::path effects_;
    std::filesystem::path shaders_;
    std::filesystem::path textures_;

    void rebuild_defaults();
    [[nodiscard]] std::filesystem::path resolve(std::string_view rel) const;
};

} // namespace vangfx
