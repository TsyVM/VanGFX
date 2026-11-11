#include <vangfx/core/paths.hpp>
#include <vangfx/core/config.hpp>

#include <windows.h>

namespace vangfx {

namespace fs = std::filesystem;

Paths::Paths(fs::path game_dir)
    : game_dir_(std::move(game_dir))
{
    rebuild_defaults();
}

void Paths::rebuild_defaults() {
    // Env override for root directory
    char buf[MAX_PATH];
    if (GetEnvironmentVariableA("VANGFX_ROOT_DIR", buf, sizeof(buf)) > 0) {
        root_ = fs::path(buf);
    } else {
        root_ = game_dir_ / L"VanGFX";
    }

    // Env override for ini name
    char ini_buf[256] = "vgx.ini";
    GetEnvironmentVariableA("VANGFX_INI_NAME", ini_buf, sizeof(ini_buf));
    ini_      = root_ / ini_buf;
    logs_     = root_ / L"logs";
    effects_  = root_ / L"effects";
    shaders_  = root_ / L"shaders";
    textures_ = root_ / L"textures";
}

fs::path Paths::resolve(std::string_view rel) const {
    fs::path p(rel);
    if (p.is_absolute()) return p;
    return root_ / p;
}

Paths& Paths::apply(const Config& cfg) {
    auto logs_v     = cfg.get_opt("Paths", "LogDir");
    auto shaders_v  = cfg.get_opt("Paths", "ShaderDir");
    auto textures_v = cfg.get_opt("Paths", "TextureDir");
    auto effects_v  = cfg.get_opt("Paths", "EffectDir");
    if (logs_v)     logs_     = resolve(*logs_v);
    if (shaders_v)  shaders_  = resolve(*shaders_v);
    if (textures_v) textures_ = resolve(*textures_v);
    if (effects_v)  effects_  = resolve(*effects_v);
    return *this;
}

Paths& Paths::with_root(fs::path root) {
    root_ = std::move(root);
    // Recompute defaults under new root (keeps any apply() overrides only
    // applied after this call; a fresh Paths would need apply() again)
    ini_      = root_ / L"vgx.ini";
    logs_     = root_ / L"logs";
    effects_  = root_ / L"effects";
    shaders_  = root_ / L"shaders";
    textures_ = root_ / L"textures";
    return *this;
}

Paths& Paths::with_ini(std::string_view ini_name) {
    ini_ = root_ / fs::path(ini_name);
    return *this;
}

const fs::path& Paths::game_dir()  const noexcept { return game_dir_; }
const fs::path& Paths::root()      const noexcept { return root_; }
const fs::path& Paths::ini()       const noexcept { return ini_; }
const fs::path& Paths::logs()      const noexcept { return logs_; }
const fs::path& Paths::effects()   const noexcept { return effects_; }
const fs::path& Paths::shaders()   const noexcept { return shaders_; }
const fs::path& Paths::textures()  const noexcept { return textures_; }

bool Paths::ensure_dirs() const {
    bool ok = true;
    for (auto* p : { &root_, &logs_, &effects_, &shaders_, &textures_ }) {
        std::error_code ec;
        fs::create_directories(*p, ec);
        if (ec) ok = false;
    }
    return ok;
}

} // namespace vangfx
